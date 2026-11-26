// ==========================================================================
// 文件：server/facet-result.mjs
// 功能：FacetRT / FacetEB 结果文件（facetrt.json / faceteb.json）读取器，同时支持
//       control.facetOutputFormat = json（旧格式，整文件内联数组）与 binary（元数据 JSON + float32 旁路 .bin，
//       规范见 docs/facet-binary-output.md）。两种格式返回完全相同的结果形状。
// 导出：readFacetResult（完整类型化数组）、sampleFacetResult（三维显示抽样）、isFacetResultFile、
//       facetResultFormat、facetResultCount、facetSidecarFiles / facetResultBytes / copyFacetSidecars /
//       removeFacetResult（旁路文件管理）、readLargeFileSync、MAX_RESULT_BYTES。
// 说明：JSON 格式的读取代码从 server.mjs 原样迁入（逐位行为不变）。
// ==========================================================================

import { closeSync, copyFileSync, existsSync, openSync, readFileSync, readSync, statSync, unlinkSync } from 'node:fs'
import { basename, dirname, join, resolve } from 'node:path'

// 单个结果文件（JSON，或二进制格式中的单个数组）超过该字节数时拒绝读取。
export const MAX_RESULT_BYTES = Number(process.env.STREAM_STUDIO_MAX_RESULT_BYTES) || 8 * 1024 * 1024 * 1024
// 三维显示的面元/体元采样上限（仅用于前端显示；磁盘上的结果保留全部面元）。
export const THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT = 1000000

// 模块：大文件读取与辐射结果（面元/体元 JSON）解析。
// readFileSync 对超过 2 GiB 的文件会抛 ERR_FS_FILE_TOO_LARGE（与 Buffer 上限无关）。
// 面元结果和三维过程数据可能达到数 GB，因此分块读入自行分配的 Buffer。
export function readLargeFileSync(path) {
  const { size } = statSync(path)
  if (!(size > 0)) return Buffer.alloc(0)
  if (size > MAX_RESULT_BYTES) {
    const label = (bytes) => (bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB`
      : bytes >= 1024 ** 2 ? `${(bytes / 1024 ** 2).toFixed(0)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`)
    throw new Error(`结果文件 ${label(size)} 超过上限 ${label(MAX_RESULT_BYTES)}，请减少面元数量或简化 OBJ`)
  }
  const buffer = Buffer.allocUnsafe(size)
  const descriptor = openSync(path, 'r')
  let offset = 0
  try {
    while (offset < size) {
      const read = readSync(descriptor, buffer, offset, Math.min(size - offset, 1 << 30), offset)
      if (read <= 0) break
      offset += read
    }
  } finally {
    closeSync(descriptor)
  }
  return offset === size ? buffer : buffer.subarray(0, offset)
}

// 作用：读取 FacetRT/FacetEB 引擎写出的面元结果 JSON（可达数 GB），返回类型化数组形式的完整结果。
// 输入：jsonPath；geometryOnly=true 时只读几何（vertexPositions）和 wavelengths，用于逐步导出的几何文件。
// 输出：{ facetCount, metrics:[[id,name,Float32Array]], vertexPositions(facetCount×9), backend, iterations,
//       maxDelta, wavelengths, bandRadiosity, bandStates, quantity, units }。
// 说明：不把整个文件转成 JS 字符串（受 V8 字符串长度上限限制），而是在 Buffer 上手工扫描键并逐个数字解析。
//       每面元有正反两个面（surfaceCount = 2·facetCount），指标数组按 (面元, 面) 交错。
function readJsonFacetResult(jsonPath, geometryOnly = false) {
  if (!existsSync(jsonPath)) throw new Error(`找不到面元结果：${jsonPath}`)
  // 大场景/多波段面元结果 JSON 会超过 512 MB。若用 readFileSync(..., 'utf8') + JSON.parse，
  // 会触发 V8 单字符串长度上限（0x1fffffe8 字符）：
  //   Cannot create a string longer than 0x1fffffe8 characters
  // 这里改为直接基于 Buffer 逐数字解析（与 sampleRadiosityResult 同一思路），
  // 不把整个文件转换成 JS 字符串，因此不再受该上限限制。
  // readFileSync 对超过 2 GiB 的文件会抛 ERR_FS_FILE_TOO_LARGE，而面元结果可达数 GB
  // （约 750 万面元 ≈ 3.3 GB），因此统一用 readLargeFileSync 分块读入自行分配的 Buffer。
  const source = readLargeFileSync(jsonPath)
  const header = source.subarray(0, Math.min(source.length, 1 << 16)).toString('utf8')
  const facetCount = Number(header.match(/"facetCount"\s*:\s*(\d+)/)?.[1])
  if (!Number.isInteger(facetCount) || facetCount < 1) throw new Error('面元结果缺少有效的 facetCount')
  const quantity = String(header.match(/"quantity"\s*:\s*"([^"]*)"/)?.[1] || '')
  const units = String(header.match(/"units"\s*:\s*"([^"]*)"/)?.[1] || '')
  const backend = String(header.match(/"backend"\s*:\s*"([^"]*)"/)?.[1] || '')
  const surfaceCount = facetCount * 2

  // 读取顶层标量（如 iterations、maxDelta）：找到键后读到下一个分隔符（, 换行 }）为止；缺失返回 undefined。
  const readScalar = (key) => {
    const keyOffset = source.indexOf('"' + key + '"')
    if (keyOffset < 0) return undefined
    const colon = source.indexOf(58, keyOffset)
    if (colon < 0) return undefined
    let end = colon + 1
    while (end < source.length && ![44, 10, 13, 125].includes(source[end])) end += 1
    const value = Number(source.toString('ascii', colon + 1, end).trim())
    return Number.isFinite(value) ? value : undefined
  }
  const iterations = readScalar('iterations')
  const maxDelta = readScalar('maxDelta')

  // 顶层数值数组按固定顺序写入；用递增的 searchFrom 避免对超大文件反复全量扫描。
  // findArray/readFixed/readDynamic：按引擎写出顺序依次定位数组；readFixed 要求元素个数恰为 count（Float32 存储），
  // readDynamic 读到 ] 为止（用于元素个数不定的 wavelengths）。
  let searchFrom = 0
  const findArray = (key) => {
    const keyOffset = source.indexOf('"' + key + '"', searchFrom)
    if (keyOffset < 0) return -1
    const arrayOffset = source.indexOf(91, keyOffset)
    if (arrayOffset < 0) throw new Error('面元结果缺少 ' + key + ' 数组')
    searchFrom = keyOffset + key.length + 2
    return arrayOffset
  }
  const readFixed = (key, count) => {
    const arrayOffset = findArray(key)
    if (arrayOffset < 0) return null
    const values = new Float32Array(count)
    let position = arrayOffset + 1
    for (let index = 0; index < count; index += 1) {
      while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13 || source[position] === 44)) position += 1
      if (position >= source.length) throw new Error(key + ' 数据不完整')
      const numberStart = position
      while (position < source.length && source[position] !== 44 && source[position] !== 93) position += 1
      if (position === numberStart) throw new Error(key + ' 数据不完整')
      const value = Number(source.toString('ascii', numberStart, position))
      if (!Number.isFinite(value)) throw new Error(key + ' 包含无效数值')
      values[index] = value
    }
    while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13)) position += 1
    if (position >= source.length || source[position] !== 93) throw new Error(key + ' 数据不完整')
    return values
  }
  const readDynamic = (key) => {
    const arrayOffset = findArray(key)
    if (arrayOffset < 0) return null
    const values = []
    let position = arrayOffset + 1
    while (true) {
      while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13 || source[position] === 44)) position += 1
      if (position >= source.length || source[position] === 93) break
      const numberStart = position
      while (position < source.length && source[position] !== 44 && source[position] !== 93) position += 1
      const value = Number(source.toString('ascii', numberStart, position))
      if (!Number.isFinite(value)) throw new Error(key + ' 包含无效数值')
      values.push(value)
    }
    return values
  }

  // 步骤 1：先读三角形顶点坐标（必须有）；geometryOnly 时顺带读写在其后的 wavelengths 就返回。
  // 步骤 2：按文件内写入顺序依次读各指标数组（顺序错误会因 searchFrom 单向前进而找不到）。
  const arrays = {}
  arrays.vertexPositions = readFixed('vertexPositions', facetCount * 9)
  if (!arrays.vertexPositions) throw new Error('面元结果缺少完整的三角形坐标')
  if (geometryOnly) {
    // The FacetEB step-sync geometry file may also carry the engine's actual
    // band list ("wavelengths", written after vertexPositions); use it when present.
    const geometryWavelengths = (readDynamic('wavelengths') || []).filter((value) => Number.isFinite(value) && value > 0)
    return { facetCount, vertexPositions: arrays.vertexPositions, backend, metrics: [], bandRadiosity: new Float32Array(0), wavelengths: geometryWavelengths }
  }
  arrays.wavelengths = readDynamic('wavelengths')
  // 文件内的写入顺序：sunlit, radiosity, lightEnhancement, temperature, ...
  const metricKeys = ['sunlit', 'radiosity', 'lightEnhancement', 'temperature', 'netRadiation', 'sensibleHeat', 'latentHeat', 'storageHeat', 'photovoltaicPower']
  for (const key of metricKeys) arrays[key] = readFixed(key, surfaceCount)
  arrays.bandRadiosity = readFixed('bandRadiosity', surfaceCount * Math.max(1, (arrays.wavelengths || []).length))
  // Per band and facet: front lit, front shaded, back lit, back shaded.
  arrays.bandStates = readFixed('bandStates', facetCount * 4 * Math.max(1, (arrays.wavelengths || []).length))

  const wavelengths = (arrays.wavelengths || []).filter((value) => Number.isFinite(value) && value > 0)
  const bandRadiosity = arrays.bandRadiosity || new Float32Array(0)
  if (bandRadiosity.length && bandRadiosity.length < surfaceCount * Math.max(1, wavelengths.length)) {
    throw new Error('面元多波段辐亮度数据不完整')
  }

  // 步骤 3：整理指标清单 [id, 显示名, 是否必需]。quantity 为 spectral radiance 时 radiosity 的含义是光谱辐亮度。
  // 必需项缺失即报错；能量平衡类指标（温度、热通量、光伏）仅在文件含有时输出。
  const radianceResult = quantity.toLowerCase() === 'spectral radiance'
  const metricDefinitions = [
    ['radiosity', radianceResult ? '光谱辐亮度 [W m⁻² sr⁻¹ μm⁻¹]' : '辐射度', true],
    ['lightEnhancement', '光照增强', true],
    ['sunlit', '光照比例', true],
    ['temperature', '温度 [K]', false],
    ['netRadiation', '净辐射 [W m⁻²]', false],
    ['sensibleHeat', '显热 [W m⁻²]', false],
    ['latentHeat', '潜热 [W m⁻²]', false],
    ['storageHeat', '储热 [W m⁻²]', false],
    ['photovoltaicPower', '光伏功率 [W m⁻²]', false]
  ]
  const metrics = []
  for (const [id, name, required] of metricDefinitions) {
    const values = arrays[id]
    if (required && !values) throw new Error('面元结果缺少 ' + id + ' 数组')
    if (values) metrics.push([id, name, values])
  }
  return {
    facetCount,
    metrics,
    vertexPositions: arrays.vertexPositions,
    backend,
    iterations,
    maxDelta,
    wavelengths,
    bandRadiosity,
    bandStates: arrays.bandStates || new Float32Array(0),
    quantity,
    units
  }
}

// 作用：为三维显示读取面元结果的等间隔抽样（面元数超过 maximumFacets 时抽样），返回普通数组。
// 输出：{ kind:'facet', facetCount(抽样后), sourceFacetCount, sampled, metrics:[{id,label,values}],
//       vertexPositions, backend, iterations, maxDelta }。
// 说明：单遍扫描数组，只解析落在被选面元范围内的数值，其余仅跳过，避免一次性解析数 GB 数据。
//       抽样序号 = floor(i·N/n)（保持在 [0,N-1]）；每面元的指标有正反 2 个值。
function sampleJsonFacetResult(jsonPath, maximumFacets = THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT, geometryOnly = false) {
  if (!existsSync(jsonPath)) throw new Error('找不到面元结果：' + jsonPath)
  const source = readLargeFileSync(jsonPath)
  const header = source.subarray(0, Math.min(source.length, 65536)).toString('utf8')
  const sourceFacetCount = Number(header.match(/"facetCount"\s*:\s*(\d+)/)?.[1])
  if (!Number.isInteger(sourceFacetCount) || sourceFacetCount < 1) throw new Error('面元结果缺少有效的 facetCount')
  const facetCount = Math.min(sourceFacetCount, maximumFacets)
  const sourceFacets = new Uint32Array(facetCount)
  for (let sampledFacet = 0; sampledFacet < facetCount; sampledFacet += 1) {
    sourceFacets[sampledFacet] = facetCount === sourceFacetCount
      ? sampledFacet
      : Math.min(sourceFacetCount - 1, Math.floor(sampledFacet * sourceFacetCount / facetCount))
  }

  // sampleArray：顺序扫描数组，sourceValue 为当前元素序号，仅取 [targetStart,targetEnd) 内的值写入抽样结果。
  const sampleArray = (key, valuesPerFacet) => {
    const keyOffset = source.indexOf('"' + key + '"')
    const arrayOffset = keyOffset < 0 ? -1 : source.indexOf(91, keyOffset)
    if (arrayOffset < 0) throw new Error('面元结果缺少 ' + key + ' 数组')
    const values = new Array(facetCount * valuesPerFacet)
    let position = arrayOffset + 1
    let sourceValue = 0
    let sampledFacet = 0
    let targetStart = sourceFacets[0] * valuesPerFacet
    let targetEnd = targetStart + valuesPerFacet
    while (position < source.length && sampledFacet < facetCount) {
      while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13 || source[position] === 44)) position += 1
      if (position >= source.length || source[position] === 93) break
      const numberStart = position
      while (position < source.length && source[position] !== 44 && source[position] !== 93) position += 1
      if (sourceValue >= targetStart && sourceValue < targetEnd) {
        const value = Number(source.toString('ascii', numberStart, position).trim())
        if (!Number.isFinite(value)) throw new Error(key + ' 包含无效数值')
        values[sampledFacet * valuesPerFacet + sourceValue - targetStart] = value
      }
      sourceValue += 1
      if (sourceValue === targetEnd) {
        sampledFacet += 1
        if (sampledFacet < facetCount) {
          targetStart = sourceFacets[sampledFacet] * valuesPerFacet
          targetEnd = targetStart + valuesPerFacet
        }
      }
    }
    if (sampledFacet !== facetCount) throw new Error(key + ' 数据不完整')
    return values
  }
  const scalar = (key) => {
    const keyOffset = source.indexOf('"' + key + '"')
    if (keyOffset < 0) return undefined
    const colon = source.indexOf(58, keyOffset)
    if (colon < 0) return undefined
    let end = colon + 1
    while (end < source.length && ![44, 10, 13, 125].includes(source[end])) end += 1
    const value = Number(source.toString('ascii', colon + 1, end).trim())
    return Number.isFinite(value) ? value : undefined
  }
  const backend = header.match(/"backend"\s*:\s*"([^"]*)"/)?.[1]
  const quantity = header.match(/"quantity"\s*:\s*"([^"]*)"/)?.[1] || ''
  const units = header.match(/"units"\s*:\s*"([^"]*)"/)?.[1] || ''
  const radiosityLabel = quantity.toLowerCase() === 'spectral radiance'
    ? '光谱辐亮度 [W m⁻² sr⁻¹ μm⁻¹]'
    : `辐射度${units ? ` [${units.replaceAll('-2', '⁻²')}]` : ' [W m⁻²]'}`
  const vertexPositions = sampleArray('vertexPositions', 9)
  // 指标清单（第三项为必需）；geometryOnly 时不读任何指标。此处没有光伏功率项（与 readRadiosityResult 不同）。
  const metricDefinitions = [
    ['radiosity', radiosityLabel, true],
    ['lightEnhancement', '光照增强 [-]', true],
    ['sunlit', '光照比例 [-]', true],
    ['temperature', '温度 [K]'],
    ['netRadiation', '净辐射 [W m⁻²]'],
    ['sensibleHeat', '显热 [W m⁻²]'],
    ['latentHeat', '潜热 [W m⁻²]'],
    ['storageHeat', '储热 [W m⁻²]']
  ]
  const metrics = metricDefinitions.filter(([key, , required]) => {
    const exists = source.indexOf('"' + key + '"') >= 0
    if (required && !exists && !geometryOnly) throw new Error('面元结果缺少 ' + key + ' 数组')
    return exists && !geometryOnly
  }).map(([id, label]) => ({ id, label, values: sampleArray(id, 2) }))

  return {
    kind: 'facet',
    path: jsonPath,
    facetCount,
    sourceFacetCount,
    sampled: facetCount < sourceFacetCount,
    metrics,
    vertexPositions,
    backend,
    iterations: scalar('iterations'),
    maxDelta: scalar('maxDelta')
  }
}

// 通过读前 64 KB 头部判断一个 JSON 是不是面元结果（含 backend、facetCount、vertexPositions）；出错返回 false。
function isJsonFacetResultFile(path) {
  let descriptor
  try {
    descriptor = openSync(path, 'r')
    const sample = Buffer.alloc(Math.min(65536, statSync(path).size))
    readSync(descriptor, sample, 0, sample.length, 0)
    const header = sample.toString('utf8')
    return /"backend"\s*:/.test(header) &&
      /"facetCount"\s*:\s*\d+/.test(header) &&
      /"vertexPositions"\s*:\s*\[/.test(header)
  } catch {
    return false
  } finally {
    if (descriptor != null) closeSync(descriptor)
  }
}


// ==========================================================================
// 二进制格式（control.facetOutputFormat = "binary"，规范见 docs/facet-binary-output.md）。
// JSON 只含元数据、小数组和描述符 { file, offset, length, dtype:"float32", shape }；大数组在同目录 .bin 中
// （小端 float32，起点 16 字节对齐）。下面的读取器返回与 JSON 路径完全相同的结果形状，
// 并且只读取调用方需要的数组（几何模式只读 vertexPositions，抽样只读被选面元所在的数据块），
// 因此多 GB 结果不会整体进入内存。
// ==========================================================================

const BINARY_META_MAX_BYTES = 64 * 1024 * 1024
const SAMPLE_WINDOW_BYTES = 8 * 1024 * 1024
const SAMPLE_DENSE_BYTES = 64 * 1024
const HEAD_BYTES = 65536
const isLittleEndianHost = new Uint8Array(new Uint16Array([1]).buffer)[0] === 1

function readHead(path, bytes = HEAD_BYTES) {
  const fd = openSync(path, 'r')
  try {
    const buffer = Buffer.alloc(Math.min(bytes, statSync(path).size))
    const read = readSync(fd, buffer, 0, buffer.length, 0)
    return buffer.toString('utf8', 0, read)
  } finally { closeSync(fd) }
}

const isDescriptor = (value) => Boolean(value) && typeof value === 'object' && !Array.isArray(value) && typeof value.dtype === 'string'
const BINARY_HEAD_PATTERN = /"format"\s*:\s*"binary"/
const INLINE_GEOMETRY_PATTERN = /"vertexPositions"\s*:\s*\[/

// 作用：判定面元结果 JSON 的格式（'binary' | 'json'）。不整体读取大文件：
// 先看前 64 KB（format 字段或内联的 vertexPositions 数组），不能判定且文件不大时才解析整个 JSON。
export function facetResultFormat(path) {
  const head = readHead(path)
  if (BINARY_HEAD_PATTERN.test(head)) return 'binary'
  if (INLINE_GEOMETRY_PATTERN.test(head)) return 'json'
  const size = statSync(path).size
  if (size > BINARY_META_MAX_BYTES) return 'json'
  try {
    const meta = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
    return meta?.format === 'binary' || isDescriptor(meta?.vertexPositions) ? 'binary' : 'json'
  } catch { return 'json' }
}

// 二进制结果的元数据（小 JSON），按 (size, mtime) 缓存最近几个。
const binaryMetaCache = new Map()
function readBinaryMeta(path) {
  const info = statSync(path)
  const cached = binaryMetaCache.get(path)
  if (cached && cached.size === info.size && cached.mtimeMs === info.mtimeMs) return cached.meta
  if (info.size > BINARY_META_MAX_BYTES) throw new Error(`面元结果元数据 ${(info.size / 1048576).toFixed(0)} MB 过大，不像二进制格式的 JSON：${basename(path)}`)
  const meta = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
  if (!meta || typeof meta !== 'object') throw new Error('面元结果元数据格式错误')
  if (meta.format !== undefined && meta.format !== 'binary') throw new Error(`不支持的面元结果格式：${meta.format}`)
  if (Number(meta.formatVersion) > 1) throw new Error(`面元结果二进制格式版本 ${meta.formatVersion} 高于本程序支持的版本 1，请升级 Stream Studio`)
  if (binaryMetaCache.size >= 8) binaryMetaCache.delete(binaryMetaCache.keys().next().value)
  binaryMetaCache.set(path, { size: info.size, mtimeMs: info.mtimeMs, meta })
  return meta
}

// 旁路文件名必须是与 JSON 同目录的纯文件名（拒绝路径分隔符，防止元数据指向任意文件）。
function sidecarName(value) {
  const name = String(value || '')
  if (!name || name !== basename(name) || name === '.' || name === '..' || /[\\/:]/.test(name)) throw new Error('面元结果引用了无效的旁路文件名：' + name)
  return name
}

// 元数据中所有被引用的旁路文件（绝对路径，去重，只含存在的文件名合法者）。JSON 格式返回空数组。
export function facetSidecarFiles(jsonPath) {
  try {
    if (!existsSync(jsonPath) || facetResultFormat(jsonPath) !== 'binary') return []
    const meta = readBinaryMeta(jsonPath)
    const names = new Set()
    const add = (value) => { try { names.add(sidecarName(value)) } catch {} }
    if (meta.binaryFile) add(meta.binaryFile)
    for (const value of Object.values(meta)) if (isDescriptor(value) && value.file) add(value.file)
    return [...names].map((name) => join(dirname(jsonPath), name))
  } catch { return [] }
}

// 结果在磁盘上的总字节数（JSON + 旁路文件）。
export function facetResultBytes(jsonPath) {
  let total = statSync(jsonPath).size
  for (const path of facetSidecarFiles(jsonPath)) { try { total += statSync(path).size } catch {} }
  return total
}

// 把旁路文件复制到 destinationDirectory（保持文件名，JSON 用文件名引用）；目标已存在且大小相同则跳过。
export function copyFacetSidecars(jsonPath, destinationDirectory) {
  const copied = []
  for (const source of facetSidecarFiles(jsonPath)) {
    if (!existsSync(source)) continue
    const target = join(destinationDirectory, basename(source))
    if (resolve(target) === resolve(source)) continue
    if (existsSync(target) && statSync(target).size === statSync(source).size) continue
    copyFileSync(source, target)
    copied.push(target)
  }
  return copied
}

// 删除结果 JSON 及其旁路文件。旁路文件先于 JSON 删除（需要读 JSON 才知道名字）；同主名的 .bin 也一并清理。
export function removeFacetResult(jsonPath) {
  const targets = new Set(facetSidecarFiles(jsonPath))
  targets.add(jsonPath.replace(/\.json$/i, '.bin'))
  let removed = 0
  for (const path of targets) {
    if (path !== jsonPath && existsSync(path)) { try { unlinkSync(path); removed += 1 } catch {} }
  }
  if (existsSync(jsonPath)) { unlinkSync(jsonPath); removed += 1 }
  return removed
}

// 只读元数据得到面元数；JSON 格式读前 4 KB；失败返回 0。
export function facetResultCount(jsonPath) {
  try {
    const head = readHead(jsonPath, 4096)
    const count = Number(head.match(/"facetCount"\s*:\s*(\d+)/)?.[1])
    if (count > 0) return count
    if (facetResultFormat(jsonPath) === 'binary') return Number(readBinaryMeta(jsonPath).facetCount) || 0
    return 0
  } catch { return 0 }
}

// 旁路文件读取器：按需打开、同一次读取内复用文件句柄。
class SidecarReader {
  constructor(directory) { this.directory = directory; this.files = new Map() }
  open(file) {
    const name = sidecarName(file)
    let entry = this.files.get(name)
    if (!entry) {
      const path = join(this.directory, name)
      if (!existsSync(path)) throw new Error('缺少面元结果旁路文件：' + name)
      entry = { fd: openSync(path, 'r'), size: statSync(path).size, name }
      this.files.set(name, entry)
    }
    return entry
  }
  close() { for (const { fd } of this.files.values()) { try { closeSync(fd) } catch {} } this.files.clear() }
}

// 校验描述符并返回 { entry, offset, length }。
function resolveDescriptor(reader, descriptor, key) {
  if (descriptor.dtype !== 'float32') throw new Error(`${key} 的数据类型 ${descriptor.dtype} 不受支持（仅 float32）`)
  if (!isLittleEndianHost) throw new Error('二进制面元结果为小端存储，本机不是小端')
  const offset = Number(descriptor.offset)
  const length = Number(descriptor.length)
  if (!Number.isSafeInteger(offset) || offset < 0 || !Number.isSafeInteger(length) || length < 0) throw new Error(`${key} 的描述符无效`)
  if (Array.isArray(descriptor.shape) && descriptor.shape.reduce((product, value) => product * Number(value), 1) !== length) {
    throw new Error(`${key} 的 shape 与 length 不一致`)
  }
  const entry = reader.open(descriptor.file)
  if (offset + length * 4 > entry.size) throw new Error(`${key} 数据不完整（${entry.name} 比元数据短）`)
  return { entry, offset, length }
}

function readFully(fd, buffer, bytes, position) {
  let done = 0
  while (done < bytes) {
    const read = readSync(fd, buffer, done, Math.min(bytes - done, 1 << 30), position + done)
    if (read <= 0) throw new Error('二进制面元结果读取到文件末尾，数据不完整')
    done += read
  }
}

// 读取一个字段的前 expected 个 float（描述符或内联数组），严格要求元素数相等；字段缺失返回 null。
function readFloatField(reader, meta, key, expected) {
  const field = meta[key]
  if (field === undefined || field === null) return null
  if (Array.isArray(field)) {
    if (field.length !== expected) throw new Error(key + ' 数据不完整')
    const values = new Float32Array(expected)
    for (let index = 0; index < expected; index += 1) {
      const value = Number(field[index])
      if (!Number.isFinite(value)) throw new Error(key + ' 包含无效数值')
      values[index] = value
    }
    return values
  }
  if (!isDescriptor(field)) throw new Error(key + ' 字段格式无效')
  const { entry, offset, length } = resolveDescriptor(reader, field, key)
  if (length !== expected) throw new Error(key + ' 数据不完整')
  if (length * 4 > MAX_RESULT_BYTES) throw new Error(`${key} 数组 ${(length * 4 / 1024 ** 3).toFixed(1)} GB 超过上限 ${(MAX_RESULT_BYTES / 1024 ** 3).toFixed(0)} GB，请减少面元数量或简化 OBJ`)
  const values = new Float32Array(expected)
  readFully(entry.fd, Buffer.from(values.buffer, values.byteOffset, values.byteLength), values.byteLength, offset)
  return values
}

// 读取小数组（wavelengths 等，通常内联）为普通数组；描述符形式也支持。
function readSmallArray(reader, meta, key) {
  const field = meta[key]
  if (field === undefined || field === null) return null
  if (Array.isArray(field)) return field.map(Number)
  if (isDescriptor(field)) return Array.from(readFloatField(reader, meta, key, Number(field.length)))
  return null
}

const binaryHeader = (meta) => {
  const facetCount = Number(meta.facetCount)
  if (!Number.isInteger(facetCount) || facetCount < 1) throw new Error('面元结果缺少有效的 facetCount')
  const text = (value) => (typeof value === 'string' ? value : '')
  return { facetCount, quantity: text(meta.quantity), units: text(meta.units), backend: text(meta.backend) }
}
const finiteOrUndefined = (value) => (Number.isFinite(Number(value)) && value !== null && value !== '' ? Number(value) : undefined)

function readBinaryFacetResult(jsonPath, geometryOnly) {
  const meta = readBinaryMeta(jsonPath)
  const { facetCount, quantity, units, backend } = binaryHeader(meta)
  const surfaceCount = facetCount * 2
  const reader = new SidecarReader(dirname(jsonPath))
  try {
    const vertexPositions = readFloatField(reader, meta, 'vertexPositions', facetCount * 9)
    if (!vertexPositions) throw new Error('面元结果缺少完整的三角形坐标')
    if (geometryOnly) {
      const geometryWavelengths = (readSmallArray(reader, meta, 'wavelengths') || []).filter((value) => Number.isFinite(value) && value > 0)
      return { facetCount, vertexPositions, backend, metrics: [], bandRadiosity: new Float32Array(0), wavelengths: geometryWavelengths }
    }
    const rawWavelengths = readSmallArray(reader, meta, 'wavelengths')
    const bandCount = Math.max(1, (rawWavelengths || []).length)
    const arrays = {}
    for (const key of ['sunlit', 'radiosity', 'lightEnhancement', 'temperature', 'netRadiation', 'sensibleHeat', 'latentHeat', 'storageHeat', 'photovoltaicPower']) {
      arrays[key] = readFloatField(reader, meta, key, surfaceCount)
    }
    arrays.bandRadiosity = readFloatField(reader, meta, 'bandRadiosity', surfaceCount * bandCount)
    arrays.bandStates = readFloatField(reader, meta, 'bandStates', facetCount * 4 * bandCount)
    const wavelengths = (rawWavelengths || []).filter((value) => Number.isFinite(value) && value > 0)
    return assembleFacetResult({ facetCount, surfaceCount, quantity, units, backend, arrays, vertexPositions, wavelengths,
      iterations: finiteOrUndefined(meta.iterations), maxDelta: finiteOrUndefined(meta.maxDelta) })
  } finally { reader.close() }
}

// 与 JSON 路径共用的指标清单与结果整形（readJsonFacetResult 里的同名步骤）。
function assembleFacetResult({ facetCount, surfaceCount, quantity, units, backend, arrays, vertexPositions, wavelengths, iterations, maxDelta }) {
  const bandRadiosity = arrays.bandRadiosity || new Float32Array(0)
  if (bandRadiosity.length && bandRadiosity.length < surfaceCount * Math.max(1, wavelengths.length)) {
    throw new Error('面元多波段辐亮度数据不完整')
  }
  const radianceResult = quantity.toLowerCase() === 'spectral radiance'
  const metricDefinitions = [
    ['radiosity', radianceResult ? '光谱辐亮度 [W m⁻² sr⁻¹ μm⁻¹]' : '辐射度', true],
    ['lightEnhancement', '光照增强', true],
    ['sunlit', '光照比例', true],
    ['temperature', '温度 [K]', false],
    ['netRadiation', '净辐射 [W m⁻²]', false],
    ['sensibleHeat', '显热 [W m⁻²]', false],
    ['latentHeat', '潜热 [W m⁻²]', false],
    ['storageHeat', '储热 [W m⁻²]', false],
    ['photovoltaicPower', '光伏功率 [W m⁻²]', false]
  ]
  const metrics = []
  for (const [id, name, required] of metricDefinitions) {
    const values = arrays[id]
    if (required && !values) throw new Error('面元结果缺少 ' + id + ' 数组')
    if (values) metrics.push([id, name, values])
  }
  return { facetCount, metrics, vertexPositions, backend, iterations, maxDelta, wavelengths, bandRadiosity,
    bandStates: arrays.bandStates || new Float32Array(0), quantity, units }
}

// 按升序的面元序号 sourceFacets 从描述符数组中取每面元 perFacet 个值，返回普通数组。
// 密集抽样（相邻被选记录间隔 ≤ 64 KB）一次读入最多 8 MB 的连续窗口；稀疏抽样每条记录单独读取。
function sampleFloatDescriptor(reader, descriptor, key, perFacet, sourceFacets, sourceFacetCount) {
  const { entry, offset, length } = resolveDescriptor(reader, descriptor, key)
  if (length < sourceFacetCount * perFacet) throw new Error(key + ' 数据不完整')
  const recordBytes = perFacet * 4
  const buffer = Buffer.allocUnsafe(Math.max(SAMPLE_WINDOW_BYTES, recordBytes))
  const maximumRecords = Math.floor(buffer.length / recordBytes)
  const values = new Array(sourceFacets.length * perFacet)
  let firstRecord = 0
  let endRecord = 0
  for (let sample = 0; sample < sourceFacets.length; sample += 1) {
    const record = sourceFacets[sample]
    if (record < firstRecord || record >= endRecord) {
      const next = sample + 1 < sourceFacets.length ? sourceFacets[sample + 1] : -1
      endRecord = next > record && (next - record) * recordBytes <= SAMPLE_DENSE_BYTES
        ? Math.min(record + maximumRecords, sourceFacetCount) : record + 1
      firstRecord = record
      readFully(entry.fd, buffer, (endRecord - firstRecord) * recordBytes, offset + record * recordBytes)
    }
    const base = (record - firstRecord) * recordBytes
    for (let value = 0; value < perFacet; value += 1) values[sample * perFacet + value] = buffer.readFloatLE(base + value * 4)
  }
  return values
}

function sampleBinaryFacetResult(jsonPath, maximumFacets, geometryOnly) {
  const meta = readBinaryMeta(jsonPath)
  const { facetCount: sourceFacetCount, quantity, units, backend } = binaryHeader(meta)
  const facetCount = Math.min(sourceFacetCount, maximumFacets)
  const sourceFacets = new Uint32Array(facetCount)
  for (let sampled = 0; sampled < facetCount; sampled += 1) {
    sourceFacets[sampled] = facetCount === sourceFacetCount
      ? sampled : Math.min(sourceFacetCount - 1, Math.floor(sampled * sourceFacetCount / facetCount))
  }
  const reader = new SidecarReader(dirname(jsonPath))
  try {
    const sampleField = (key, perFacet) => {
      const field = meta[key]
      if (field === undefined || field === null) throw new Error('面元结果缺少 ' + key + ' 数组')
      if (isDescriptor(field)) return sampleFloatDescriptor(reader, field, key, perFacet, sourceFacets, sourceFacetCount)
      if (!Array.isArray(field) || field.length < sourceFacetCount * perFacet) throw new Error(key + ' 数据不完整')
      const values = new Array(facetCount * perFacet)
      for (let sampled = 0; sampled < facetCount; sampled += 1) {
        for (let value = 0; value < perFacet; value += 1) {
          const number = Number(field[sourceFacets[sampled] * perFacet + value])
          if (!Number.isFinite(number)) throw new Error(key + ' 包含无效数值')
          values[sampled * perFacet + value] = number
        }
      }
      return values
    }
    const radiosityLabel = quantity.toLowerCase() === 'spectral radiance'
      ? '光谱辐亮度 [W m⁻² sr⁻¹ μm⁻¹]'
      : `辐射度${units ? ` [${units.replaceAll('-2', '⁻²')}]` : ' [W m⁻²]'}`
    const vertexPositions = sampleField('vertexPositions', 9)
    const metricDefinitions = [
      ['radiosity', radiosityLabel, true],
      ['lightEnhancement', '光照增强 [-]', true],
      ['sunlit', '光照比例 [-]', true],
      ['temperature', '温度 [K]'],
      ['netRadiation', '净辐射 [W m⁻²]'],
      ['sensibleHeat', '显热 [W m⁻²]'],
      ['latentHeat', '潜热 [W m⁻²]'],
      ['storageHeat', '储热 [W m⁻²]']
    ]
    const metrics = metricDefinitions.filter(([key, , required]) => {
      const exists = meta[key] !== undefined && meta[key] !== null
      if (required && !exists && !geometryOnly) throw new Error('面元结果缺少 ' + key + ' 数组')
      return exists && !geometryOnly
    }).map(([id, label]) => ({ id, label, values: sampleField(id, 2) }))
    return {
      kind: 'facet', path: jsonPath, facetCount, sourceFacetCount, sampled: facetCount < sourceFacetCount,
      metrics, vertexPositions, backend: backend || undefined,
      iterations: finiteOrUndefined(meta.iterations), maxDelta: finiteOrUndefined(meta.maxDelta)
    }
  } finally { reader.close() }
}

// ==========================================================================
// 对外入口：自动识别 JSON / 二进制格式，返回与旧函数相同的结果形状。
// ==========================================================================

// 作用：读取完整面元结果（类型化数组）。geometryOnly=true 时只读几何与 wavelengths。
export function readFacetResult(jsonPath, geometryOnly = false) {
  if (!existsSync(jsonPath)) throw new Error(`找不到面元结果：${jsonPath}`)
  return facetResultFormat(jsonPath) === 'binary'
    ? readBinaryFacetResult(jsonPath, geometryOnly) : readJsonFacetResult(jsonPath, geometryOnly)
}

// 作用：三维显示用的等间隔抽样（普通数组）。
export function sampleFacetResult(jsonPath, maximumFacets = THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT, geometryOnly = false) {
  if (!existsSync(jsonPath)) throw new Error('找不到面元结果：' + jsonPath)
  return facetResultFormat(jsonPath) === 'binary'
    ? sampleBinaryFacetResult(jsonPath, maximumFacets, geometryOnly) : sampleJsonFacetResult(jsonPath, maximumFacets, geometryOnly)
}

// 作用：判断一个 .json 是否为面元结果（JSON 或二进制格式）。读取失败返回 false。
export function isFacetResultFile(path) {
  try {
    if (isJsonFacetResultFile(path)) return true
    const size = statSync(path).size
    if (size > BINARY_META_MAX_BYTES) return false
    const head = readHead(path)
    if (!BINARY_HEAD_PATTERN.test(head) && !/"vertexPositions"\s*:\s*\{/.test(head) && size > HEAD_BYTES) return false
    const meta = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
    return Boolean(meta) && typeof meta === 'object' && meta.format === 'binary' &&
      Number.isInteger(Number(meta.facetCount)) && Number(meta.facetCount) >= 1 &&
      (isDescriptor(meta.vertexPositions) || Array.isArray(meta.vertexPositions))
  } catch { return false }
}
