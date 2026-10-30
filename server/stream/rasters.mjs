// Raster inspection, display sampling and per-class statistics for the STREAM image workspace.
import { closeSync, existsSync, openSync, readSync, statSync } from 'node:fs'
import { extname, isAbsolute, join, resolve } from 'node:path'

const tiffTypeSize = (type) => ({ 1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8 })[type] || 0

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
    const geoKeys = tags.get(34735) || []
    const geoKey = (key) => { for (let i = 4; i + 3 < geoKeys.length; i += 4) if (geoKeys[i] === key && geoKeys[i + 1] === 0) return geoKeys[i + 3]; return null }
    const projected = geoKey(3072), geographic = geoKey(2048)
    const bits = (tags.get(258) || [8])[0], format = (tags.get(339) || [1])[0]
    const scale = tags.get(33550) || [], tie = tags.get(33922) || []
    const description = typeof tags.get(270) === 'string' ? tags.get(270) : ''
    const names = description.match(/band names\s*=\s*\{([^}]*)\}/i)?.[1]
    // FIXED[中] Bug: 坐标原点直接取 tie[3]/tie[4]，忽略了 tie[0]/tie[1]（对应的栅格行列不一定是 0,0）；已修复：originX = tie[3] - tie[0]*scaleX、originY = tie[4] + tie[1]*scaleY（PixelIsPoint 半像元约定与 GeoAscii 引文回退未改，仍需与 readTiff 统一）
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

export function createRasterTools({ readTiff, normalizeHostPath, windowsPath }) {
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

  // Float32 band values for the viewer; large grids are block-sampled to keep the payload bounded.
  function rasterBand(value, band) {
    const path = resolve(normalizeHostPath(String(value || '').trim()))
    if (!/\.tiff?$/i.test(path) || !existsSync(path) || !statSync(path).isFile()) throw new Error(`找不到栅格：${windowsPath(path)}`)
    // FIXED[低] Bug: readTiff 会把越界的波段序号静默钳到最后一个波段，前端按请求的 band 缓存，结果会显示成别的波段而没有报错；已修复：读回后用 tiff.bands 校验请求的 band，越界直接报错（nodata 的 float32 容差比较仍在 readTiff 中，未改）
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
    const count = sorted.length
    const minimum = count ? sorted[0] : null, maximum = count ? sorted[count - 1] : null
    const histogram = new Array(96).fill(0)
    if (count && maximum > minimum) for (const sample of sorted) histogram[Math.min(95, Math.floor((sample - minimum) / (maximum - minimum) * 96))] += 1
    return {
      path: windowsPath(path), band: tiff.band, bands: tiff.bands, bandNames: tiff.bandNames,
      width, height, sourceWidth: tiff.width, sourceHeight: tiff.height, step,
      crs: tiff.crs, unit: tiff.unit, pixelSizeX: tiff.pixelSizeX, pixelSizeY: tiff.pixelSizeY, originX: tiff.originX, originY: tiff.originY,
      values: Buffer.from(output.buffer).toString('base64'),
      stats: { count, minimum, maximum, mean: count ? sum / count : null, std: count ? Math.sqrt(Math.max(0, squares / count - (sum / count) ** 2)) : null,
        p1: quantile(.01), p2: quantile(.02), p98: quantile(.98), p99: quantile(.99) },
      histogram
    }
  }

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
    const radians = Math.PI / 180, earthRadiusKm = 6371.0088
    const rowArea = geographic
      ? Array.from({ length: type.height }, (_, row) => earthRadiusKm ** 2 * Math.abs(type.pixelSizeX * radians) * Math.abs(type.pixelSizeY * radians)
        * Math.abs(Math.cos((type.originY - (row + 0.5) * Math.abs(type.pixelSizeY)) * radians)))
      : null
    const products = {}
    for (const file of ['optical_reflectance.tif', 'tir_brightness_temperature.tif', 'tir_surface_temperature.tif', 'components.tif', 'eb_forcing.tif', 'eb_fluxes.tif', 'lst.tif', 'lsct.tif']) {
      const path = join(outputDir, file)
      if (!existsSync(path)) continue
      // DEFERRED[中] 性能: readTiff 每次都 readFileSync 整个文件并解码全幅像元（includeValues=false 时也照样逐像元循环），这里对每个波段各调用一次，eb_fluxes 这类 20 多个波段的产品会重复读取解码整个文件 20 多次，在请求线程中同步阻塞事件循环；建议：一次读入、解码后按波段分别取值（或交给引擎写出统计），并放到 worker 中执行
      const first = readTiff(path, 0, false)
      if (first.width !== type.width || first.height !== type.height) continue
      const bands = []
      for (let band = 0; band < first.bands; band += 1) {
        const values = readTiff(path, band, true).values
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

  return { inspectRasters, rasterBand, classStatistics }
}
