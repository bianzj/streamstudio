// ==========================================================================
// 文件：rasters.mjs
// 功能：STREAM 影像工作台的栅格辅助：只读 GeoTIFF 头部的检查、查看器用的波段采样与统计、
//   按地表类型（IGBP）分类的产品统计
// 主要模块：
//   1. tiffTypeSize / readTiffHeader：不解码像元，只读 IFD 取尺寸、波段、数据类型、CRS、
//      像元大小与左上角原点、NoData、波段名（来自 ImageDescription 的 band names = {…}）
//   2. createRasterTools 工厂：注入 server 里的 readTiff（完整解码）与路径换算
//      - inspectRasters：批量检查栅格（最多 32 个）
//      - rasterBand：取某波段供显示（大图块采样）并给出统计与 96 档直方图
//      - classStatistics：按类型栅格逐类统计运行产品各波段的均值、标准差与面积
// 数据流：路由层传入路径 → 本文件读头/调 readTiff → 返回 JSON（values 为 Float32 的 base64）
// 与其他文件的关系：readTiff 由 server.mjs 提供（本文件的头部解析与它部分重复，见下方
//   DEFERRED 说明）；产品文件名与 STREAM 引擎输出一致（optical_reflectance.tif 等）
// 约定：坐标/像元大小沿用栅格自身单位（度或米，投影单位待核对）；面积单位 km2；
//   NaN 表示无效值，统计忽略非有限值
// ==========================================================================

// Raster inspection, display sampling and per-class statistics for the STREAM image workspace.
import { closeSync, existsSync, openSync, readSync, statSync } from 'node:fs'
import { extname, isAbsolute, join, resolve } from 'node:path'

// TIFF 字段类型编号 → 单个值的字节数（1–12 依次为 BYTE/ASCII/SHORT/LONG/RATIONAL/
// SBYTE/UNDEFINED/SSHORT/SLONG/SRATIONAL/FLOAT/DOUBLE）；未知类型返回 0。
const tiffTypeSize = (type) => ({ 1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8 })[type] || 0

// 说明：只支持经典 TIFF（魔数 42），不支持 BigTIFF；只读取下面白名单中的标签：
// 256 宽、257 高、258 位深、277 每像元样本数（波段数）、339 样本格式、
// 33550 像元大小、33922 控制点、34735 GeoKey 目录、34737 GeoASCII、42113 NoData、270 描述。
// 原点 = 控制点坐标 − 控制点像元号 × 像元大小；CRS 取 ProjectedCSType(3072) 或
// GeographicType(2048)，32767（用户自定义）视为未知。
// Header-only GeoTIFF probe: grid size, bands and CRS without decoding pixels.
function readTiffHeader(path) {
  const fd = openSync(path, 'r')
  try {
    const size = statSync(path).size
    const read = (offset, length) => {
      if (offset < 0 || offset + length > size) throw new Error('TIFF 字段数据越界')
      const buffer = Buffer.alloc(length)
      readSync(fd, buffer, 0, length, offset)
      return buffer
    }
    const head = read(0, 8)
    const order = head.toString('ascii', 0, 2)
    if (order !== 'II' && order !== 'MM') throw new Error('不是 TIFF 文件')
    const little = order === 'II'
    const u16 = (buffer, offset) => little ? buffer.readUInt16LE(offset) : buffer.readUInt16BE(offset)
    const u32 = (buffer, offset) => little ? buffer.readUInt32LE(offset) : buffer.readUInt32BE(offset)
    if (u16(head, 2) !== 42) throw new Error('暂不支持 BigTIFF')
    const ifd = u32(head, 4)
    const count = u16(read(ifd, 2), 0)
    const entries = read(ifd + 2, count * 12)
    const tags = new Map()
    for (let index = 0; index < count; index += 1) {
      const at = index * 12
      const tag = u16(entries, at), type = u16(entries, at + 2), number = u32(entries, at + 4)
      const width = tiffTypeSize(type)
      if (!width || ![256, 257, 258, 277, 339, 33550, 33922, 34735, 34737, 42113, 270].includes(tag)) continue
      const total = width * number
      const data = total <= 4 ? entries.subarray(at + 8, at + 8 + total) : read(u32(entries, at + 8), total)
      if (type === 2) { tags.set(tag, data.toString('ascii').replace(/\0+$/, '')); continue }
      // DEFERRED[低] Bug: 除 3/4/11/12 以外的类型都按单字节 data[offset] 读取，SSHORT/SLONG(8/9)、RATIONAL(5/10) 字段会解析错误；与 server.mjs 中 readTiff 的解析逻辑重复且结果不一致；建议：复用 readTiff 的 readNumber 实现（抽成只读头部的共享函数）
      const values = []
      for (let item = 0; item < number; item += 1) {
        const offset = item * width
        values.push(type === 3 ? u16(data, offset) : type === 4 ? u32(data, offset)
          : type === 12 ? (little ? data.readDoubleLE(offset) : data.readDoubleBE(offset))
          : type === 11 ? (little ? data.readFloatLE(offset) : data.readFloatBE(offset)) : data[offset])
      }
      tags.set(tag, values)
    }
    // GeoKey 目录：前 4 个值是头，之后每 4 个值为一项 [键, 位置, 个数, 值]；
    // 这里只取位置为 0（值直接存在项内）的键。
    const geoKeys = tags.get(34735) || []
    const geoKey = (key) => { for (let i = 4; i + 3 < geoKeys.length; i += 4) if (geoKeys[i] === key && geoKeys[i + 1] === 0) return geoKeys[i + 3]; return null }
    const projected = geoKey(3072), geographic = geoKey(2048)
    const bits = (tags.get(258) || [8])[0], format = (tags.get(339) || [1])[0]
    const scale = tags.get(33550) || [], tie = tags.get(33922) || []
    const description = typeof tags.get(270) === 'string' ? tags.get(270) : ''
    const names = description.match(/band names\s*=\s*\{([^}]*)\}/i)?.[1]
    return {
      width: (tags.get(256) || [0])[0], height: (tags.get(257) || [0])[0], bands: (tags.get(277) || [1])[0],
      dataType: `${format === 3 ? 'Float' : format === 2 ? 'Int' : 'UInt'}${bits}`,
      crs: projected && projected !== 32767 ? `EPSG:${projected}` : geographic && geographic !== 32767 ? `EPSG:${geographic}` : '',
      pixelSizeX: scale[0] ?? null, pixelSizeY: scale[1] ?? null,
      originX: tie.length >= 6 && scale.length >= 2 ? tie[3] - tie[0] * scale[0] : tie[3] ?? null,
      originY: tie.length >= 6 && scale.length >= 2 ? tie[4] + tie[1] * scale[1] : tie[4] ?? null,
      noData: tags.has(42113) ? String(tags.get(42113)).trim() : null,
      bandNames: names ? names.split(',').map((value) => value.trim()) : null
    }
  } finally { closeSync(fd) }
}

// 模块：栅格工具工厂。readTiff(path, band, includeValues) 返回完整解码结果
// （width/height/bands/values/crs/unit/pixelSize/origin…）；normalizeHostPath 与 windowsPath
// 负责主机路径与 Windows 路径互换。
export function createRasterTools({ readTiff, normalizeHostPath, windowsPath }) {
  // 作用：逐个检查路径对应的文件（最多 32 个），不抛异常，把问题写进每项的 error。
  // 输出：{ path, exists, bytes, format, … }；.tif/.tiff 额外带 readTiffHeader 的头信息。
  function inspectRasters(paths) {
    return (Array.isArray(paths) ? paths.slice(0, 32) : []).map((value) => {
      const source = normalizeHostPath(String(value || '').trim().replace(/^"|"$/g, ''))
      if (!source) return { path: '', exists: false }
      if (!isAbsolute(source)) return { path: source, exists: false, error: '须为绝对路径' }
      const path = resolve(source)
      if (!existsSync(path) || !statSync(path).isFile()) return { path: windowsPath(path), exists: false, error: '文件不存在' }
      const info = { path: windowsPath(path), exists: true, bytes: statSync(path).size }
      if (!/\.tiff?$/i.test(path)) return { ...info, format: extname(path).slice(1).toUpperCase() }
      try { return { ...info, format: 'GeoTIFF', ...readTiffHeader(path) } }
      catch (error) { return { ...info, format: 'TIFF', error: error.message } }
    })
  }

  // 作用：取一个波段给查看器显示。输出尺寸、CRS、像元大小、原点、Float32 值（base64）、
  // 统计量与 96 档直方图。
  // 步骤：1) 解码；2) step = ceil(max(宽, 高) / 2048)，每 step 个像元取一个样本；
  // 3) 对样本求分位数（1/2/98/99 %）与直方图；4) step > 1 时，count/最值/均值/标准差改用
  // 全部像元重新计算（stats.sampled 标记分位数基于样本）。
  // Float32 band values for the viewer; large grids are block-sampled to keep the payload bounded.
  function rasterBand(value, band) {
    const path = resolve(normalizeHostPath(String(value || '').trim()))
    if (!/\.tiff?$/i.test(path) || !existsSync(path) || !statSync(path).isFile()) throw new Error(`找不到栅格：${windowsPath(path)}`)
    const tiff = readTiff(path, band, true)
    if (Number(band) >= tiff.bands) throw new Error(`波段序号 ${band} 超出范围：该栅格只有 ${tiff.bands} 个波段`)
    const step = Math.max(1, Math.ceil(Math.max(tiff.width, tiff.height) / 2048))
    const width = Math.ceil(tiff.width / step), height = Math.ceil(tiff.height / step)
    const output = new Float32Array(width * height)
    const finite = []
    let sum = 0, squares = 0
    for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
      const sample = tiff.values[Math.min(tiff.height - 1, y * step) * tiff.width + Math.min(tiff.width - 1, x * step)]
      output[y * width + x] = Number.isFinite(sample) ? sample : Number.NaN
      if (Number.isFinite(sample)) { finite.push(sample); sum += sample; squares += sample * sample }
    }
    const sorted = Float64Array.from(finite).sort()
    const quantile = (fraction) => sorted.length ? sorted[Math.min(sorted.length - 1, Math.max(0, Math.round((sorted.length - 1) * fraction)))] : null
    // FIX(2026-11-24 S11): count / minimum / maximum / mean / std come from every source pixel, not from the display sample
    // (a skipped extreme would otherwise be missing); the quantiles and the histogram stay on the sample (stats.sampled).
    let count = sorted.length
    let minimum = count ? sorted[0] : null, maximum = count ? sorted[count - 1] : null
    let mean = count ? sum / count : null, std = count ? Math.sqrt(Math.max(0, squares / count - (sum / count) ** 2)) : null
    if (step > 1) {
      let n = 0, lo = Infinity, hi = -Infinity, total = 0, totalSquares = 0, shift = null
      for (let index = 0; index < tiff.values.length; index += 1) {
        const sample = tiff.values[index]
        if (!Number.isFinite(sample)) continue
        // 数值处理：先减去首个有效值再累加平方和，避免大偏置（如开尔文温度）造成方差相消误差。
        if (shift === null) shift = sample   // shifted sums keep the variance stable for large offsets (e.g. Kelvin)
        n += 1
        if (sample < lo) lo = sample
        if (sample > hi) hi = sample
        total += sample - shift; totalSquares += (sample - shift) ** 2
      }
      if (n) {
        count = n; minimum = lo; maximum = hi
        mean = shift + total / n; std = Math.sqrt(Math.max(0, totalSquares / n - (total / n) ** 2))
      }
    }
    // 直方图：样本落在 [最小值, 最大值] 的 96 个等宽格内（最大值等于最小值时不统计）。
    const histogram = new Array(96).fill(0)
    if (sorted.length && maximum > minimum) for (const sample of sorted) histogram[Math.max(0, Math.min(95, Math.floor((sample - minimum) / (maximum - minimum) * 96)))] += 1
    return {
      path: windowsPath(path), band: tiff.band, bands: tiff.bands, bandNames: tiff.bandNames,
      width, height, sourceWidth: tiff.width, sourceHeight: tiff.height, step,
      crs: tiff.crs, unit: tiff.unit, pixelSizeX: tiff.pixelSizeX, pixelSizeY: tiff.pixelSizeY, originX: tiff.originX, originY: tiff.originY,
      values: Buffer.from(output.buffer).toString('base64'),
      stats: { count, minimum, maximum, mean, std, sampled: step > 1,
        p1: quantile(.01), p2: quantile(.02), p98: quantile(.98), p99: quantile(.99) },
      histogram
    }
  }

  // 作用：按地表类型栅格逐类统计本次运行各产品每个波段的像元数、均值、标准差（及面积 km2）。
  // 输入：输出目录、类型栅格路径与波段（1 起）；输出：{ pixelAreaKm2, classes, products }。
  // 说明：产品与类型栅格尺寸不一致时跳过；类型码非有限值的像元不计。
  // Per-IGBP-class mean and standard deviation of every band of the run's products.
  function classStatistics(outputDir, typeFile, typeBand = 1) {
    typeFile = typeFile && normalizeHostPath(typeFile)
    if (!typeFile || !existsSync(typeFile)) throw new Error('找不到本次运行的地表类型栅格')
    if (!Number.isInteger(typeBand) || typeBand < 1) throw new Error('本次运行的地表类型波段无效')
    const type = readTiff(resolve(typeFile), typeBand - 1, true)
    if (type.band !== typeBand - 1) throw new Error('本次运行的地表类型波段不存在')
    // 经纬度栅格的像元面积随纬度变化，逐行按球面计算；投影栅格按米制像元面积。
    // DEFERRED[中] Bug: 只有 unit === '°' 时才按球面面积计算；unit 为空（缺少 GTModelType/线性单位键，或经纬度 CRS 只写了引文）时把度数当成米，面积小 10 个数量级以上；投影单位是英尺或 EPSG:3857（面积放大 1/cos²φ）时也不正确；建议：只有确认线性单位为米(9001)时才用米制公式，其它情况返回 null 并在前端显示“—”，Web Mercator 按纬度修正
    const geographic = type.unit === '°' && type.pixelSizeX && type.pixelSizeY && Number.isFinite(type.originY)
    const area = geographic ? 0 : Math.abs((type.pixelSizeX || 0) * (type.pixelSizeY || 0)) / 1e6
    // 面积：经纬度栅格按球面逐行计算 R²·Δλ·Δφ·cos φ（φ 取该行中心纬度）；
    // 其他栅格每像元面积固定 = |dx·dy| / 1e6 km2。球面面积只在 unit 为 '°' 时启用。
    const radians = Math.PI / 180, earthRadiusKm = 6371.0088
    const rowArea = geographic
      ? Array.from({ length: type.height }, (_, row) => earthRadiusKm ** 2 * Math.abs(type.pixelSizeX * radians) * Math.abs(type.pixelSizeY * radians)
        * Math.abs(Math.cos((type.originY - (row + 0.5) * Math.abs(type.pixelSizeY)) * radians)))
      : null
    // 遍历运行输出里可能存在的产品文件；不存在的跳过。
    const products = {}
    for (const file of ['optical_reflectance.tif', 'tir_brightness_temperature.tif', 'tir_surface_temperature.tif', 'components.tif', 'eb_forcing.tif', 'eb_fluxes.tif', 'lst.tif', 'lsct.tif']) {
      const path = join(outputDir, file)
      if (!existsSync(path)) continue
      // DEFERRED[中] 性能: readTiff 每次都 readFileSync 整个文件并解码全幅像元（includeValues=false 时也照样逐像元循环），这里对每个波段各调用一次，eb_fluxes 这类 20 多个波段的产品会重复解码整个文件 20 多次（原始文件读盘由结果缓存共用；2026-11-23 起整体在后台计算线程执行，不再阻塞服务事件循环，但仍慢）；建议：一次解码后按波段分别取值（或交给引擎写出统计）
      const first = readTiff(path, 0, false)
      if (first.width !== type.width || first.height !== type.height) continue
      const bands = []
      for (let band = 0; band < first.bands; band += 1) {
        const values = readTiff(path, band, true).values
        // 按类型码累计 count/sum/平方和/面积，再换算成均值与标准差（方差取 max(0, …) 防负）。
        const accumulator = new Map()
        for (let index = 0; index < values.length; index += 1) {
          const value = values[index], code = type.values[index]
          if (!Number.isFinite(value) || !Number.isFinite(code)) continue
          const entry = accumulator.get(code) || { count: 0, sum: 0, squares: 0, areaKm2: 0 }
          entry.count += 1; entry.sum += value; entry.squares += value * value
          if (rowArea) entry.areaKm2 += rowArea[Math.floor(index / type.width)]
          accumulator.set(code, entry)
        }
        bands.push(Object.fromEntries([...accumulator].map(([code, entry]) => {
          const mean = entry.sum / entry.count
          return [code, { count: entry.count, mean, std: Math.sqrt(Math.max(0, entry.squares / entry.count - mean * mean)), ...(rowArea ? { areaKm2: entry.areaKm2 } : {}) }]
        })))
      }
      products[file] = { bandNames: first.bandNames, bands }
    }
    const classes = new Map()
    for (const code of type.values) if (Number.isFinite(code)) classes.set(code, (classes.get(code) || 0) + 1)
    return { outputDir: windowsPath(outputDir), pixelAreaKm2: area || null,
      classes: [...classes].sort((a, b) => a[0] - b[0]).map(([code, pixels]) => ({ code, pixels })), products }
  }

  // 对外接口。
  return { inspectRasters, rasterBand, classStatistics }
}
