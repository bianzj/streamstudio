// ==========================================================================
// 文件：test-stream-server-tools-20261124.mjs
// 功能：2026-11-24 评审（第 6 块）发现的 STREAM 服务端测试缺口的补充测试（node:test，不需要真实引擎）。
// 主要模块：
//   1. EB 配置映射：服务端发出的每个 energy 键必须被引擎 config.cpp 读取，且服务端缺省值
//      与引擎缺省值一致（engineDefault 用正则从 config.cpp 源码提取缺省）；起转小时数向上取整
//   2. rasters.mjs：tiffBytes 手工拼装最小 GeoTIFF，测试 inspectRasters（头信息探测）、
//      rasterBand（采样/统计/直方图/波段越界）、classStatistics（按类别的均值/标准差与像元数）
//   3. index.mjs 路由：不依赖引擎的行为（status、inspect、缺引擎时 run 被拒绝、run-info 报错）
// 关系：被测模块 server/stream/{index,config,rasters}.mjs；用正则读取 models/stream/app/config.cpp
//   的源码文本做对照（config.cpp 里这些调用的写法改变时需同步修改本文件的正则）。
// 约定：tiffBytes 生成小端 TIFF，坐标系 EPSG:32650，像元 30 m，原点 (500000, 4400000)。
// 运行：node --test tools/test-stream-server-tools-20261124.mjs
// ==========================================================================

// Server-side test gaps found by the 2026-11-24 review (block 6):
//  - EB configuration mapping / default table: server JS field -> engine config key -> engine default (read from config.cpp);
//  - rasters.mjs: header probe of a hand-built GeoTIFF, band sampling, per-class statistics (with an injected readTiff);
//  - index.mjs: route-level behaviour that needs no engine (status, inspect, missing engine, run-info errors).
// Run: node --test tools/test-stream-server-tools-20261124.mjs
import assert from 'node:assert/strict'
import { test, after } from 'node:test'
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createStreamService } from '../server/stream/index.mjs'
import { createProjectStore } from '../server/stream/config.mjs'
import { createRasterTools } from '../server/stream/rasters.mjs'

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = mkdtempSync(join(tmpdir(), 'stream-server-tools-'))
after(() => rmSync(root, { recursive: true, force: true }))
const identity = (path) => path

// ---------- EB configuration mapping / defaults ----------
const pack = join(root, 'packs', 'pack.json')
mkdirSync(dirname(pack), { recursive: true })
writeFileSync(pack, JSON.stringify({ kind: 'stream-forcing-pack', schemaVersion: 1, product: 'era5', files: [] }))
const inputFile = join(root, 'input.tif')
writeFileSync(inputFile, '')
const store = createProjectStore({ normalizeHostPath: identity, windowsPath: identity, forcingDirectories: () => [join(root, 'documents', 'forcing-metadata')] })
// scene(energy, temperatureExtra)：最小 EB 场景；energy 并入 temperature.energy，
//   temperatureExtra 并入 temperature（如 spinupHours）。build(input) 编译为引擎配置。
// engineSource：引擎配置解析源码文本，供下面的缺省值/键名对照使用。
const scene = (energy = {}, temperatureExtra = {}) => ({ overpass: '2019-07-15T04:00:00Z', locationSource: 'input',
  surface: { type: { file: inputFile }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 }, soilMoisture: { value: 0.2 } },
  view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] },
  temperature: { mode: 'eb', soilMoisture: { source: 'user' }, era5: { pack }, energy, ...temperatureExtra } })
const build = (input) => store.buildEngineConfig(input, { outputDir: root, dataDir: root })
const engineSource = readFileSync(join(repo, 'models', 'stream', 'app', 'config.cpp'), 'utf8')

// 作用：从 config.cpp 源码按几种固定写法（numberOr/integerOr、GetString、GetBool，
//   以及 resistance 子对象）提取某键的引擎缺省值，数字转 Number、true/false 转布尔；找不到返回 undefined。
// Engine default of a key read from the `energy` object in config.cpp: numberOr/integerOr(energy, "k", D, ...), GetString("k", "D"), GetBool("k", D).
function engineDefault(key) {
  const escaped = key.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  const patterns = [
    new RegExp(`(?:numberOr|integerOr)\\(energy, "${escaped}", ([-+0-9.eE]+)`),
    new RegExp(`energy\\.GetString\\("${escaped}", "([^"]*)"\\)`),
    new RegExp(`energy\\.GetBool\\("${escaped}", (true|false)\\)`),
    new RegExp(`numberOr\\(resistance, "${escaped}", ([-+0-9.eE]+)`),
    new RegExp(`resistance\\.GetString\\("${escaped}", "([^"]*)"\\)`)
  ]
  for (const pattern of patterns) {
    const match = pattern.exec(engineSource)
    if (match) return match[1] === 'true' ? true : match[1] === 'false' ? false : Number.isNaN(Number(match[1])) || match[1] === '' ? match[1] : Number(match[1])
  }
  return undefined
}

// 测试：(1) 服务端缺省与引擎缺省逐键相等；(2) 仅在给出时才转发的键，转发后键名必须出现在
//   config.cpp 中（防止服务端发出引擎不认识的名字）；(3) 未给出的可选键不会被服务端凭空生成。
test('EB mapping: every emitted energy key is read by the engine and the JS defaults equal the engine defaults', () => {
  const { energy } = build(scene()).temperature
  const defaults = { soilHeatMethod: 'surface', temperatureMethod: 'robust', diffuseFraction: 0.2, enforceSolarLimit: true, spectralBinNm: 50,
    soilThermal: 'constant', soilEmissivityModel: 'hapke', surfaceTemperatureNormalisation: 'retrieval' }
  for (const [key, value] of Object.entries(defaults)) {
    assert.deepEqual(energy[key], value, `server default of ${key}`)
    assert.deepEqual(engineDefault(key), value, `engine default of ${key} (config.cpp)`)
  }
  assert.equal(energy.scheme, 'tirt')
  assert.equal(energy.soilResistance.method, 'constant')
  assert.equal(energy.soilResistance.value, engineDefault('value'))
  assert.equal(engineDefault('method'), 'constant')
  // keys that are only forwarded when given must exist in the engine under the same name
  const forwarded = build(scene({ stomatalG0: 0.02, minWindSpeed: 0.1, leafEmissivity: 0.98, soilConductivity: 1.2, soilVolumetricCapacity: 1.1e6,
    soilHeatFraction: 0.3, maxStabilityIterations: 40, heatRoughnessKb: 4, referenceHeight: 10, maxAerodynamicResistance: 300, maxIterations: 70,
    soilEmissivityK: 0.8, soilEmissivity: 0.96, soilEmissivityWet: 0.98 })).temperature.energy
  for (const key of ['stomatalG0', 'minWindSpeed', 'leafEmissivity', 'soilConductivity', 'soilVolumetricCapacity', 'soilHeatFraction', 'maxStabilityIterations',
    'heatRoughnessKb', 'referenceHeight', 'maxAerodynamicResistance', 'maxIterations', 'soilEmissivityK', 'soilEmissivity', 'soilEmissivityWet'])
    assert.ok(forwarded[key] !== undefined, `${key} forwarded`)
  for (const key of Object.keys(forwarded).filter((name) => name !== 'soilResistance'))
    assert.ok(engineSource.includes(`"${key}"`), `engine config.cpp reads "${key}"`)
  assert.equal(forwarded.maxIterations, 70)
  assert.equal(forwarded.referenceHeight, 10)
  // omitted optional keys are not invented by the server
  for (const key of ['stomatalG0', 'minWindSpeed', 'maxIterations', 'heatRoughnessKb', 'referenceHeight', 'maxAerodynamicResistance'])
    assert.equal(energy[key], undefined, `${key} omitted`)
})

// 测试：spinupHours 向上取整为整点节点数（7.2 → 8）；未给出则不下发（引擎缺省 7）；
//   范围 0–240 以外被拒绝。
test('EB mapping: spin-up hours are whole nodes rounded up, like the engine', () => {
  assert.equal(build(scene({}, { spinupHours: 7.2 })).temperature.spinupHours, 8)
  assert.equal(build(scene({}, { spinupHours: 0 })).temperature.spinupHours + 0, 0)
  assert.equal(build(scene()).temperature.spinupHours, undefined)  // engine default 7
  assert.match(engineSource, /GetDouble\("spinupHours", 7\.0\)/)
  assert.throws(() => build(scene({}, { spinupHours: 241 })), /0–240/)
  assert.throws(() => build(scene({}, { spinupHours: -1 })), /0–240/)
})

// ---------- rasters.mjs ----------
// 作用：拼装最小 TIFF 字节：IFD 标签按升序排列；256/257 宽高，258 位深 32，259 无压缩，
//   273/279 单条带的偏移与字节数，339 = 3 表示浮点样本，270 为含 band names 的描述；
//   33550 像元尺度 (30, 30, 0)，33922 连接点（原点 500000, 4400000），
//   34735 GeoKey（3072 投影坐标系 = 32650，即 EPSG:32650）。大于 4 字节的值写在 IFD 之后，
//   像元数据放在最后，并回填条带偏移。
// Minimal little-endian TIFF: one Float32 strip, GeoTIFF scale / tie point / EPSG key, band-name description.
function tiffBytes({ width, height, values }) {
  const entries = []
  const data = []
  const description = Buffer.from('band names = {alpha}\0', 'ascii')
  const doubles = (list) => { const b = Buffer.alloc(list.length * 8); list.forEach((v, i) => b.writeDoubleLE(v, i * 8)); return b }
  const shorts = (list) => { const b = Buffer.alloc(list.length * 2); list.forEach((v, i) => b.writeUInt16LE(v, i * 2)); return b }
  const pixels = Buffer.alloc(width * height * 4)
  values.forEach((v, i) => pixels.writeFloatLE(v, i * 4))
  const tags = [
    [256, 3, 1, width], [257, 3, 1, height], [258, 3, 1, 32], [259, 3, 1, 1], [262, 3, 1, 1], [273, 4, 1, 'strip'], [277, 3, 1, 1],
    [278, 3, 1, height], [279, 4, 1, pixels.length], [339, 3, 1, 3],
    [270, 2, description.length, description], [33550, 12, 3, doubles([30, 30, 0])], [33922, 12, 6, doubles([0, 0, 0, 500000, 4400000, 0])],
    [34735, 3, 12, shorts([1, 1, 0, 2, 3072, 0, 1, 32650, 1025, 0, 1, 1])]
  ].sort((a, b) => a[0] - b[0])
  const ifdSize = 2 + tags.length * 12 + 4
  let cursor = 8 + ifdSize
  const head = Buffer.alloc(8 + ifdSize)
  head.write('II', 0, 'ascii'); head.writeUInt16LE(42, 2); head.writeUInt32LE(8, 4); head.writeUInt16LE(tags.length, 8)
  const stripOffsetPlaceholder = []
  tags.forEach(([tag, type, count, value], index) => {
    const at = 10 + index * 12
    head.writeUInt16LE(tag, at); head.writeUInt16LE(type, at + 2); head.writeUInt32LE(count, at + 4)
    if (Buffer.isBuffer(value)) {
      if (value.length <= 4) value.copy(head, at + 8)
      else { head.writeUInt32LE(cursor, at + 8); data.push(value); cursor += value.length }
    } else if (value === 'strip') stripOffsetPlaceholder.push(at + 8)
    else if (type === 3) head.writeUInt16LE(value, at + 8)
    else head.writeUInt32LE(value, at + 8)
  })
  stripOffsetPlaceholder.forEach((at) => head.writeUInt32LE(cursor, at))
  return Buffer.concat([head, ...data, pixels])
}

// 测试：inspectRasters 对 6 种输入的返回：正常 TIFF（尺寸、类型、CRS、像元大小、原点、波段名）、
//   不存在、相对路径、非 TIFF 的 .txt、损坏的 .tif、空串；参数不是数组时返回空数组。
test('rasters.inspectRasters: header probe, missing file, relative path, non-TIFF', () => {
  const tool = createRasterTools({ readTiff: () => { throw new Error('unused') }, normalizeHostPath: identity, windowsPath: identity })
  const file = join(root, 'probe.tif')
  writeFileSync(file, tiffBytes({ width: 3, height: 2, values: [1, 2, 3, 4, 5, 6] }))
  writeFileSync(join(root, 'notes.txt'), 'x')
  writeFileSync(join(root, 'broken.tif'), 'not a tiff at all')
  const [ok, missing, relative, text, broken, empty] = tool.inspectRasters([file, join(root, 'nope.tif'), 'relative.tif', join(root, 'notes.txt'), join(root, 'broken.tif'), ''])
  assert.equal(ok.exists, true)
  assert.equal(ok.width, 3); assert.equal(ok.height, 2); assert.equal(ok.bands, 1)
  assert.equal(ok.dataType, 'Float32')
  assert.equal(ok.crs, 'EPSG:32650')
  assert.equal(ok.pixelSizeX, 30)
  assert.equal(ok.originX, 500000); assert.equal(ok.originY, 4400000)
  assert.deepEqual(ok.bandNames, ['alpha'])
  assert.equal(missing.exists, false); assert.match(missing.error, /不存在/)
  assert.match(relative.error, /绝对路径/)
  assert.equal(text.format, 'TXT')
  assert.equal(broken.format, 'TIFF'); assert.ok(broken.error)
  assert.equal(empty.exists, false)
  assert.equal(tool.inspectRasters('not an array').length, 0)
})

// 测试：rasterBand 对含 NaN 的 2x2 数据：有效像元数 3、最小/最大/均值（7/3）、96 格直方图、
//   base64 的 Float32 样本保留 NaN；波段号越界与文件缺失分别报错。
test('rasters.rasterBand: samples, statistics, band range and missing file', () => {
  const file = join(root, 'band.tif')
  writeFileSync(file, tiffBytes({ width: 2, height: 2, values: [1, 2, 3, 4] }))
  const readTiff = (path, band) => ({ width: 2, height: 2, bands: 1, band, values: Float32Array.from([1, 2, Number.NaN, 4]), bandNames: null, crs: '', unit: '', pixelSizeX: 1, pixelSizeY: 1, originX: 0, originY: 0 })
  const tool = createRasterTools({ readTiff, normalizeHostPath: identity, windowsPath: identity })
  const band = tool.rasterBand(file, 0)
  assert.equal(band.width, 2); assert.equal(band.step, 1)
  assert.equal(band.stats.count, 3)
  assert.equal(band.stats.minimum, 1); assert.equal(band.stats.maximum, 4)
  assert.ok(Math.abs(band.stats.mean - 7 / 3) < 1e-12)
  assert.equal(band.histogram.length, 96)
  assert.equal(band.histogram.reduce((a, b) => a + b, 0), 3)
  const raw = Buffer.from(band.values, 'base64')
  assert.equal(raw.readFloatLE(0), 1); assert.ok(Number.isNaN(raw.readFloatLE(8)))
  assert.throws(() => tool.rasterBand(file, 3), /超出范围/)
  assert.throws(() => tool.rasterBand(join(root, 'nope.tif'), 0), /找不到栅格/)
})

// 测试：按类别图（含一个 NaN 像元）统计产品波段：类别 10 有 2 个像元（值 1、3：均值 2、std 1，
//   即总体标准差），类别 12 有 1 个（均值 5）；像元面积 pixelAreaKm2 = 30 m × 30 m = 900e-6 km²。
test('rasters.classStatistics: per-class mean / std and class pixel counts', () => {
  const out = join(root, 'run-out')
  mkdirSync(out, { recursive: true })
  writeFileSync(join(out, 'components.tif'), 'x')
  const typeFile = join(root, 'type.tif')
  writeFileSync(typeFile, 'x')
  const readTiff = (path, band, includeValues) => {
    if (path.endsWith('type.tif')) return { width: 2, height: 2, bands: 1, band, values: Float32Array.from([10, 10, 12, Number.NaN]), unit: '', pixelSizeX: 30, pixelSizeY: 30, originY: 0 }
    return { width: 2, height: 2, bands: 1, band, values: Float32Array.from([1, 3, 5, 7]), bandNames: ['T'] }
  }
  const tool = createRasterTools({ readTiff, normalizeHostPath: identity, windowsPath: identity })
  const result = tool.classStatistics(out, typeFile, 1)
  assert.deepEqual(result.classes, [{ code: 10, pixels: 2 }, { code: 12, pixels: 1 }])
  const stats = result.products['components.tif'].bands[0]
  assert.equal(stats[10].count, 2); assert.equal(stats[10].mean, 2); assert.equal(stats[10].std, 1)
  assert.equal(stats[12].mean, 5)
  assert.equal(result.pixelAreaKm2, 900 / 1e6)
  assert.throws(() => tool.classStatistics(out, join(root, 'missing.tif')), /找不到/)
  assert.throws(() => tool.classStatistics(out, typeFile, 0), /波段无效/)
})

// 以下 makeService 构造资源/模型目录都不存在的服务（因此"没有引擎"），请求体用内存回调；
//   call(method, path, data) 返回 { status, payload }。
// ---------- index.mjs routes that need no engine ----------
function makeService() {
  let next = {}, reply = null
  const service = createStreamService({ resourceRoot: join(root, 'no-resources'), modelRoot: join(root, 'no-models'), normalizeHostPath: identity, windowsPath: identity,
    documentsRoot: join(root, 'docs-index'), json: (_response, status, payload) => { reply = { status, payload }; return true }, body: async () => next,
    readTiff: () => { throw new Error('unused') } })
  const call = async (method, path, data = {}) => {
    next = data; reply = null
    await service.handle({ method, on() {} }, {}, new URL(`http://localhost${path}`))
    return reply
  }
  return { call }
}

// 测试：无引擎时 status 报不可用；inspect 仍可读栅格头；run 被拒绝并提示 STREAM 引擎缺失；
//   run-info 对不存在的目录、没有 run-manifest.json 的目录、空路径都返回 >= 400。
test('index routes: status without engine, inspect, missing engine on run, run-info errors', async () => {
  const { call } = makeService()
  const status = await call('GET', '/api/stream/status')
  assert.equal(status.status, 200)
  assert.equal(status.payload.engine?.available ?? status.payload.available ?? false, false)
  const inspect = await call('POST', '/api/stream/inspect', { paths: [join(root, 'probe.tif')] })
  assert.equal(inspect.status, 200)
  assert.equal(inspect.payload.rasters[0].width, 3)
  const run = await call('POST', '/api/stream/run', { scene: scene() })
  assert.ok(run.status >= 400, 'run without an engine is rejected')
  assert.match(JSON.stringify(run.payload), /STREAM 引擎/)
  const missing = await call('POST', '/api/stream/run-info', { outputDir: join(root, 'does-not-exist') })
  assert.ok(missing.status >= 400); assert.match(JSON.stringify(missing.payload), /找不到运行输出目录/)
  const notRun = await call('POST', '/api/stream/run-info', { outputDir: root })
  assert.ok(notRun.status >= 400); assert.match(JSON.stringify(notRun.payload), /run-manifest\.json/)
  const empty = await call('POST', '/api/stream/run-info', { outputDir: '' })
  assert.ok(empty.status >= 400)
})
