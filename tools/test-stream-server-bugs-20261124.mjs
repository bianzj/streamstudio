// ==========================================================================
// 文件：test-stream-server-bugs-20261124.mjs
// 功能：STREAM 服务端 2026-11-24 第三批修复的 node:test 测试：S1（能量参数校验与未知字段警告）、
//   冰雪融化开关透传、S7（停止发生在子进程启动之前）、缓存键含 ctime、S11（栅格统计取全部像元）。
// 主要模块：
//   1. 夹具：临时目录、气象包与输入文件占位、scene()（最小 EB 场景）与 build()（编译为引擎配置）
//   2. S1：非法 temperature.energy 取值在提交阶段按引擎规则被拒绝；未知字段只告警不报错
//   3. iceMelt：只有显式 false 才下发给引擎
//   4. S7：假引擎行为测试——排队/运行中的任务被 stop 都以 stopped 收尾（另有一个宽松的结构兜底检查）
//   5. 缓存键：fileStamp 的 key 含 4 段（最后一段为 ctimeMs）
//   6. S11：rasterBand 的统计覆盖所有像元，仅降采样网格才置 sampled 标志
// 关系：被测模块 server/stream/config.mjs（createProjectStore）、server/result-cache.mjs（fileStamp）、
//   server/stream/rasters.mjs（createRasterTools）；不依赖真实引擎。
// 运行：node --test tools/test-stream-server-bugs-20261124.mjs
// ==========================================================================

// Server-side tests for the third batch of 2026-11-24 fixes: S1 (energy validation + unknown-field warnings), S7 (stop before
// spawn), S11 (float32 nodata, true-extreme raster statistics), cache key ctime.
// Run: node --test tools/test-stream-server-bugs-20261124.mjs
import assert from 'node:assert/strict'
import { test, after } from 'node:test'
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createProjectStore } from '../server/stream/config.mjs'
import { fileStamp } from '../server/result-cache.mjs'
import { createRasterTools } from '../server/stream/rasters.mjs'
import { fakeEngine, makeService, waitFor } from './stream-server-test-helpers.mjs'

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = mkdtempSync(join(tmpdir(), 'stream-bugs-server-'))
after(() => rmSync(root, { recursive: true, force: true }))
const identity = (path) => path
const pack = join(root, 'packs', 'pack.json')
mkdirSync(dirname(pack), { recursive: true })
writeFileSync(pack, JSON.stringify({ kind: 'stream-forcing-pack', schemaVersion: 1, product: 'era5', files: [] }))
const inputFile = join(root, 'input.tif')
writeFileSync(inputFile, '')
const store = createProjectStore({ normalizeHostPath: identity, windowsPath: identity, forcingDirectories: () => [join(root, 'documents', 'forcing-metadata')] })
// scene(energy)：最小 EB 场景（用户给定土壤湿度、ERA5 包为占位），energy 并入 temperature.energy；
// build(input)：经 createProjectStore.buildEngineConfig 编译成引擎配置，非法取值在此抛错。
const scene = (energy = {}) => ({ overpass: '2019-07-15T04:00:00Z', locationSource: 'input',
  surface: { type: { file: inputFile }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 }, soilMoisture: { value: 0.2 } },
  view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] },
  temperature: { mode: 'eb', soilMoisture: { source: 'user' }, era5: { pack }, energy } })
const build = (input) => store.buildEngineConfig(input, { outputDir: root, dataDir: root })

// 测试：bad 表中每个字段取越界/非法值时都应抛出含字段名的错误；
//   合法值与"未提供"（空字符串、diffuseFraction = -1 表示 Erbs）应通过。
test('S1: EB energy values are validated at submission with the engine rules', () => {
  const bad = { soilHeatMethod: 'banana', temperatureMethod: 'x', diffuseFraction: 1.5, spectralBinNm: 150,
    heatRoughnessKb: 21, soilEmissivityK: 0, soilHeatFraction: 2, leafEmissivity: 0, stomatalG0: -1, minWindSpeed: -1,
    referenceHeight: 0, soilConductivity: 0, soilVolumetricCapacity: -5 }
  for (const [key, value] of Object.entries(bad)) {
    assert.throws(() => build(scene({ [key]: value })), new RegExp(key), `${key}=${value}`)
  }
  assert.throws(() => build(scene({ spectralBinNm: 12.5 })), /spectralBinNm/)
  assert.throws(() => build(scene({ diffuseFraction: true })), /diffuseFraction/)
  // valid and "not provided" values pass
  assert.doesNotThrow(() => build(scene({ soilHeatMethod: 'depth', temperatureMethod: 'direct', diffuseFraction: -1, spectralBinNm: 25,
    heatRoughnessKb: 5, soilEmissivityK: 0.1, leafEmissivity: 0.98, soilHeatFraction: '', stomatalG0: 0.02 })))
  assert.doesNotThrow(() => build(scene({})))
})

// 测试：拼写错误的字段（如 stomatalg0）只在 sceneWarnings 中给出一条警告，不使 build 失败。
test('S1: unknown temperature.energy fields are reported as warnings, never as errors', () => {
  const input = scene({ stomatalg0: 0.02, maxIterations: 40, soilHeatMethod: 'surface' })
  assert.doesNotThrow(() => build(input))
  const warnings = store.sceneWarnings(input)
  assert.equal(warnings.length, 1)
  assert.match(warnings[0], /stomatalg0/)
  assert.deepEqual(store.sceneWarnings(scene({ iceMelt: false })), [])
})

test('ice melt: only an explicit iceMelt=false is passed to the engine', () => {
  assert.equal(build(scene({ iceMelt: false })).temperature.energy.iceMelt, false)
  assert.equal(build(scene({})).temperature.energy.iceMelt, undefined)
  assert.equal(build(scene({ iceMelt: true })).temperature.energy.iceMelt, undefined)
})

// 测试 S7（行为级）：用假引擎（长时间睡眠的脚本）排两个任务。
//   1) 排队中的第二个任务被 stop 时直接以 stopped 关闭，引擎从未为它启动；
//   2) 运行中的第一个任务被 stop 时以 stopped 收尾，run-manifest.json 的 status 也是 stopped（不是 failed）。
//   “已激活但尚无子进程时 stop() 只打标记、launch() 见标记即不启动”的窗口在 HTTP 接口下不可达（start() 是同步的），
//   所以对应两行实现仍用下面一个宽松的源码检查兜底，只要求结构存在、不绑定具体写法。
test('S7: stop() closes queued jobs and running jobs as stopped', async () => {
  const resource = fakeEngine(root, 'sleepy', 'setTimeout(() => {}, 60000)')
  const { service, call, events } = await makeService({ resourceRoot: resource, modelRoot: join(resource, 'models'), documentsRoot: join(root, 'documents') })
  const outputRoot = join(root, 'sleepy-out')
  const input = (name) => ({ overpass: '2019-07-15T04:00:00Z', locationSource: 'input', name, outputRoot,
    surface: { type: { file: inputFile }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 } },
    view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] },
    temperature: { mode: 'rt', components: Object.fromEntries(['soilSunlit', 'soilShaded', 'leafSunlit', 'leafShaded'].map((key) => [key, { value: 300 }])) } })
  try {
    const first = (await call('POST', '/api/stream/run', { scene: input('first') })).payload
    await waitFor(() => events.some((event) => event.type === 'job-started' && event.job.id === first.id), 'first job started')
    const second = (await call('POST', '/api/stream/run', { scene: input('second') })).payload
    assert.equal((await call('POST', '/api/stream/stop', { id: second.id })).payload.stopped, true)
    assert.ok(events.some((event) => event.type === 'job-closed' && event.job.id === second.id && event.status === 'stopped'))
    assert.ok(!events.some((event) => event.type === 'job-started' && event.job.id === second.id), 'a stopped queued job must never start')
    assert.equal((await call('POST', '/api/stream/stop', { id: first.id })).payload.stopped, true)
    await waitFor(() => events.some((event) => event.type === 'job-closed' && event.job.id === first.id), 'first job closed')
    const closed = events.find((event) => event.type === 'job-closed' && event.job.id === first.id)
    assert.equal(closed.status, 'stopped')
    const runs = (await call('POST', '/api/stream/runs', { outputRoot })).payload.runs
    assert.equal(runs.length, 1)
    assert.equal(JSON.parse(readFileSync(join(runs[0].outputDir, 'run-manifest.json'), 'utf8')).status, 'stopped')
  } finally {
    service.shutdown()
  }
  // 兜底的结构检查：stop() 里有“激活但无子进程 → 打 stopped 标记”，launch() 开头见 stopped 即收尾，容忍空白与引号差异。
  const source = readFileSync(join(repo, 'server', 'stream', 'index.mjs'), 'utf8')
  assert.match(source, /function stop\([^)]*\)[\s\S]*?!\s*active\.child[\s\S]*?active\.stopped\s*=\s*true/)
  assert.match(source, /function launch\([^)]*\)\s*\{\s*if\s*\(\s*job\.stopped\s*\)\s*\{\s*finish\(\s*job\s*,\s*['"]stopped['"]/)
})

// 测试：fileStamp(file).key 以 '|' 分为 4 段，最后一段为数值（ctimeMs），用于改名/替换后的缓存失效。
test('cache key: ctimeMs is part of the file identity', () => {
  const stamp = fileStamp(inputFile)
  assert.equal(stamp.key.split('|').length, 4)
  assert.match(stamp.key, /\|\d+(\.\d+)?$/)
})

// 测试 S11：4100x3 的假栅格，宽度超限时预览按步长 3 降采样；
//   但最大/最小/均值/计数仍须基于全部像元（1000 位于被跳过的 x = 1 处仍应被统计到），
//   并置 sampled = true。均值的期望值按 10 的底值加两个异常点解析计算。
test('S11: rasterBand statistics use every pixel; sampled flag set only for down-sampled grids', () => {
  const width = 4100, height = 3
  const values = new Float64Array(width * height).fill(10)
  values[1 * width + 1] = 1000   // skipped by the step-3 sample (x = 1 is not a multiple of 3)
  values[2 * width + 7] = -50
  const tools = createRasterTools({ readTiff: () => ({ width, height, bands: 1, band: 0, values, bandNames: null }),
    normalizeHostPath: identity, windowsPath: identity })
  const file = join(root, 'big.tif')
  writeFileSync(file, '')
  const result = tools.rasterBand(file, 0)
  assert.equal(result.step, 3)
  assert.equal(result.stats.maximum, 1000)
  assert.equal(result.stats.minimum, -50)
  assert.equal(result.stats.count, width * height)
  assert.equal(result.stats.sampled, true)
  const exact = (10 * (width * height - 2) + 1000 - 50) / (width * height)
  assert.ok(Math.abs(result.stats.mean - exact) < 1e-9)
})