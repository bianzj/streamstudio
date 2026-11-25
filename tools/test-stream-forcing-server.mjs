// ==========================================================================
// 文件：test-stream-forcing-server.mjs
// 功能：STREAM 服务端（server/stream/）的气象强迫（forcing）元数据 API 与工程字段的测试，
//   以及评审修复 S6（未完成的运行不得列出写了一半的产物）。使用 node:test，不依赖浏览器。
// 主要模块：
//   1. 测试夹具：临时目录 root、makeService（不经 socket 直接驱动路由表）、气象包/CSV 样例、
//      ebScene/compile（把 EB 场景编译成引擎配置的 temperature 段）
//   2. API 测试：预设列表与预设 JSON、forcing-check 成功/失败/解析摘要（依赖真实 stream.exe）
//   3. 安全校验：路径穿越（..）、相对路径、未知预设、超大内联对象均被拒绝
//   4. 工程字段透传：era5 / forcing / forcingMetadata 到引擎配置，站点形式与数据包形式的必填项
//   5. 元数据保存/读取往返，以及工程保存/打开时的路径映射（预设名不被改写）
//   6. register：通用格点经 forcing-register 注册并出现在数据包列表
//   7. S6：用"假引擎"（node.exe 副本冒充 stream.exe）模拟崩溃与成功的运行，检查产物清单
// 关系：被测对象为 server/stream/index.mjs（createStreamService）与 server/stream/config.mjs
//   （createProjectStore、mapStreamScenePaths）。真实引擎缺失时依赖它的用例会被跳过（skip）。
// 运行：node --test tools/test-stream-forcing-server.mjs
// ==========================================================================

// Server-side tests for the weather-forcing metadata API and project fields (2026-11-24), plus review fix S6
// (a run that did not complete must not list half-written products).
// Run: node --test tools/test-stream-forcing-server.mjs   (engine-dependent cases are skipped when stream.exe is missing)
import assert from 'node:assert/strict'
import { test, after } from 'node:test'
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createStreamService } from '../server/stream/index.mjs'
import { createProjectStore, mapStreamScenePaths } from '../server/stream/config.mjs'

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = mkdtempSync(join(tmpdir(), 'stream-forcing-server-'))
after(() => rmSync(root, { recursive: true, force: true }))
const identity = (path) => path
const realEngine = existsSync(join(repo, 'models', 'bin_x64', 'Release', 'stream.exe')) && existsSync(join(repo, 'models', 'bin_x64', 'Release', 'stream-defined', 'classes.json'))

// 模块：测试夹具。makeService 返回 { service, call }；call(method, path, data) 把 data 作为请求体，
//   直接调用 service.handle 并返回 json() 回调捕获的 { status, payload }。
//   json/body 被替换为内存实现，readTiff 被替换为抛错（这些用例不应读栅格）。
// Drive the HTTP route table without a socket.
function makeService({ resourceRoot = repo, modelRoot = join(repo, 'models'), documentsRoot = join(root, 'documents') } = {}) {
  let next = {}, reply = null
  const service = createStreamService({ resourceRoot, modelRoot, normalizeHostPath: identity, windowsPath: identity, documentsRoot,
    json: (_response, status, payload) => { reply = { status, payload }; return true }, body: async () => next, readTiff: () => { throw new Error('unused') } })
  const call = async (method, path, data = {}) => {
    next = data; reply = null
    await service.handle({ method, on() {} }, {}, new URL(`http://localhost${path}`))
    return reply
  }
  return { service, call }
}

const pack = join(root, 'packs', 'pack.json')
mkdirSync(dirname(pack), { recursive: true })
writeFileSync(pack, JSON.stringify({ kind: 'stream-forcing-pack', schemaVersion: 1, product: 'gldas-noah-0.25', metadata: 'gldas-noah-0.25', files: [] }))
const csv = join(root, 'st.csv')
writeFileSync(csv, 'time,temp\n2019-07-15T00:00:00Z,20\n')
const inputFile = join(root, 'input.tif')
writeFileSync(inputFile, '')
const store = createProjectStore({ normalizeHostPath: identity, windowsPath: identity, forcingDirectories: () => [join(root, 'documents', 'forcing-metadata')] })
// ebScene：最小的 EB 场景（单像元式输入）；temperature 覆盖/追加 temperature 段，extra 追加顶层字段。
// compile：用 createProjectStore 的 buildEngineConfig 编译，返回引擎配置的 temperature 段；
//   校验失败时会抛出（中文）错误，供 assert.throws 匹配。
const ebScene = (temperature, extra = {}) => ({ overpass: '2019-07-15T04:00:00Z', locationSource: 'input',
  surface: { type: { file: inputFile }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 }, soilMoisture: { value: 0.2 } },
  view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] },
  temperature: { mode: 'eb', soilMoisture: { source: 'user' }, ...temperature }, ...extra })
const compile = (temperature, extra) => store.buildEngineConfig(ebScene(temperature, extra), { outputDir: root, dataDir: root }).temperature

// 测试：预设列表与预设 JSON 由真实引擎提供；预设名含 ../ 时返回 400。
test('API: preset list and preset JSON come from the engine', { skip: !realEngine }, async () => {
  const { call } = makeService()
  const list = await call('GET', '/api/stream/forcing/presets')
  assert.equal(list.status, 200)
  assert.deepEqual(list.payload.presets, ['era5', 'era5-land', 'gldas-noah-0.25', 'gldas-noah-1.0', 'station-csv'])
  const gldas = await call('POST', '/api/stream/forcing/preset', { name: 'gldas-noah-0.25' })
  assert.equal(gldas.payload.metadata.form, 'gridded')
  assert.equal(gldas.payload.metadata.variables.humidity.type, 'specific')
  assert.equal((await call('POST', '/api/stream/forcing/preset', { name: '../etc' })).status, 400)
})

test('API: forcing-check success, failure and resolved summary', { skip: !realEngine }, async () => {
  const { call } = makeService()
  const ok = await call('POST', '/api/stream/forcing/check', { metadata: 'era5' })
  assert.equal(ok.status, 200)
  assert.equal(ok.payload.ok, true)
  assert.equal(ok.payload.resolved.variables.airTemperature.height_m, 2)
  const bad = await call('POST', '/api/stream/forcing/check', { metadata: { preset: 'era5', variables: { wind: { type: 'bogus' } } } })
  assert.equal(bad.status, 200)
  assert.equal(bad.payload.ok, false)
  assert.ok(bad.payload.errors.length > 0)
  const corrected = await call('POST', '/api/stream/forcing/check', { metadata: { preset: 'gldas-noah-0.25', variables: { wind: { type: 'speed', height_m: 2, speed: { source: 'Wind_f_inst', unit: 'm s-1' } } } } })
  assert.equal(corrected.payload.ok, true)
  assert.equal(corrected.payload.resolved.heightCorrection.wind.from_m, 2)
  assert.equal(corrected.payload.resolved.heightCorrection.wind.to_m, 10)
  assert.equal((await call('POST', '/api/stream/forcing/check', {})).status, 400)
})

// 测试：路径穿越与超大输入被拒绝。覆盖：工程/元数据/数据包目录之外的元数据文件、
//   路径中的 ".."、站点文件相对路径、未知预设、超大的内联元数据对象（此处 7 万字符，具体上限待核对），
//   以及保存/读取元数据 API 的文件名与请求级检查（均应返回 400）。
test('path traversal and oversize input are rejected', async () => {
  const { call } = makeService()
  const documents = join(root, 'documents')
  mkdirSync(join(documents, 'forcing-metadata'), { recursive: true })
  // metadata file outside project / metadata / pack directories
  const outside = join(root, 'outside', 'meta.json')
  mkdirSync(dirname(outside), { recursive: true })
  writeFileSync(outside, JSON.stringify({ preset: 'era5' }))
  assert.throws(() => compile({ era5: { pack }, forcing: { metadata: outside } }), /只能位于/)
  // ".." segments in metadata path, station files and inline station registry
  assert.throws(() => compile({ forcing: { metadata: 'station-csv', files: [`${root}/a/../st.csv`] } }), /\.\./)
  assert.throws(() => compile({ forcing: { metadata: { preset: 'station-csv', station: { stations: [{ id: 'a', file: '../../secret.csv' }] } } } }), /\.\./)
  assert.throws(() => compile({ forcing: { metadata: `${root}/x/../outside/meta.json` }, era5: { pack } }), /\.\./)
  // relative paths, unknown preset, oversize inline object
  assert.throws(() => compile({ forcing: { metadata: 'station-csv', files: ['st.csv'] } }), /绝对路径/)
  assert.throws(() => compile({ era5: { pack }, forcing: { metadata: 'no-such-preset' } }), /未知的气象强迫预设/)
  assert.throws(() => compile({ era5: { pack }, forcing: { metadata: { preset: 'era5', description: 'x'.repeat(70000) } } }), /过大/)
  assert.throws(() => compile({ era5: { pack: outside } }), /不是气象数据包索引/)
  // API: file name and request-level checks
  const project = join(root, 'proj', 'stream-project.json')
  mkdirSync(dirname(project), { recursive: true })
  writeFileSync(project, '{}')
  for (const fileName of ['../evil.json', 'stream-project.json', 'a/forcing-metadata.json'])
    assert.equal((await call('POST', '/api/stream/forcing/metadata/save', { projectPath: project, metadata: { preset: 'era5' }, fileName })).status, 400)
  assert.equal((await call('POST', '/api/stream/forcing/metadata/save', { projectPath: '', metadata: { preset: 'era5' } })).status, 400)
  assert.equal((await call('POST', '/api/stream/forcing/check', { metadata: outside })).status, 400)
  assert.equal((await call('POST', '/api/stream/forcing/metadata/read', { projectPath: project, path: outside })).status, 400)
})

// 测试：工程字段透传到引擎配置。仅有旧版 era5 字段时输出不变（无 forcing 块）；
//   站点形式（station-csv）不需要数据包但需要站点文件，非站点预设需要气象数据包；
//   forcingMetadata 简写等价于 forcing.metadata。
test('project fields pass through to the engine config', () => {
  // Legacy ERA5 field only: unchanged output (no forcing block).
  assert.deepEqual(compile({ era5: { pack } }).era5, { pack })
  assert.equal(compile({ era5: { pack } }).forcing, undefined)
  const gldas = compile({ era5: { pack }, forcing: { metadata: 'gldas-noah-0.25' } })
  assert.deepEqual(gldas.forcing, { metadata: 'gldas-noah-0.25' })
  const inline = compile({ forcing: { pack, metadata: { preset: 'gldas-noah-0.25', variables: { wind: { height_m: 2 } } } } })
  assert.equal(inline.forcing.pack, pack)
  assert.equal(inline.forcing.metadata.variables.wind.height_m, 2)
  // Station form needs no pack but needs files (or registry files).
  const station = compile({ forcing: { metadata: 'station-csv', files: [csv] } })
  assert.deepEqual(station.forcing.files, [csv])
  assert.equal(station.era5, undefined)
  assert.throws(() => compile({ forcing: { metadata: 'station-csv' } }), /站点/)
  assert.throws(() => compile({ forcing: { metadata: 'gldas-noah-0.25' } }), /气象数据包/)
  // forcingMetadata shorthand
  assert.equal(compile({ era5: { pack }, forcingMetadata: 'era5-land' }).forcing.metadata, 'era5-land')
  // forcing.pack 与旧字段 era5.pack 同时存在：引擎以 forcing.pack 为准，所以只输出 forcing 块（不再重复输出 era5 块）。
  const other = join(root, 'packs', 'other.json')
  writeFileSync(other, JSON.stringify({ kind: 'stream-forcing-pack', schemaVersion: 1, product: 'era5', files: [] }))
  const both = compile({ era5: { pack: other }, forcing: { pack } })
  assert.equal(both.era5, undefined)
  assert.equal(both.forcing.pack, pack)
})

// 测试：元数据保存/读取往返，保存的文件只允许被所属工程引用（其他工程报"只能位于"）；
//   import 模式可读任意位置但必须是元数据形状的 JSON；工程保存/打开的路径映射只改写文件路径，
//   不改写预设名。
test('metadata save / read round trip and project path mapping', async () => {
  const { call } = makeService()
  const project = join(root, 'proj2', 'stream-project.json')
  mkdirSync(dirname(project), { recursive: true })
  writeFileSync(project, '{}')
  const saved = await call('POST', '/api/stream/forcing/metadata/save', { projectPath: project, metadata: { name: 'mine', preset: 'gldas-noah-0.25', variables: { wind: { height_m: 2 } } } })
  assert.equal(saved.status, 200)
  assert.equal(saved.payload.path, join(dirname(project), 'forcing-metadata.json'))
  const read = await call('POST', '/api/stream/forcing/metadata/read', { projectPath: project })
  assert.equal(read.payload.metadata.name, 'mine')
  assert.equal(read.payload.metadata.kind, 'stream-forcing-metadata')
  assert.equal((await call('POST', '/api/stream/forcing/metadata/read', { projectPath: join(root, 'proj2', 'stream-project.json'), fileName: 'forcing-metadata-2.json' })).payload.exists, false)
  // the saved file is accepted as temperature.forcing.metadata for that project, and rejected for another project
  const scene = ebScene({ era5: { pack }, forcing: { metadata: saved.payload.path } }, { projectPath: project })
  assert.equal(store.buildEngineConfig(scene, { outputDir: root, dataDir: root }).temperature.forcing.metadata, saved.payload.path)
  assert.throws(() => store.buildEngineConfig(ebScene({ era5: { pack }, forcing: { metadata: saved.payload.path } }, { projectPath: join(root, 'proj', 'stream-project.json') }), { outputDir: root, dataDir: root }), /只能位于/)
  // import reads a metadata document picked anywhere, but only metadata-shaped JSON
  const picked = join(root, 'outside', 'picked.json')
  writeFileSync(picked, JSON.stringify({ kind: 'stream-forcing-metadata', name: 'picked', preset: 'era5' }))
  assert.equal((await call('POST', '/api/stream/forcing/metadata/read', { import: true, path: picked })).payload.metadata.name, 'picked')
  writeFileSync(picked, JSON.stringify({ hello: 1 }))
  assert.equal((await call('POST', '/api/stream/forcing/metadata/read', { import: true, path: picked })).status, 400)
  // project save/open mapping rewrites paths but never a preset name
  const mapped = { temperature: { forcing: { pack: 'p.json', files: ['a.csv'], metadata: 'forcing-metadata.json' } } }
  mapStreamScenePaths(mapped, (file) => `M/${file}`)
  assert.deepEqual(mapped.temperature.forcing, { pack: 'M/p.json', files: ['M/a.csv'], metadata: 'M/forcing-metadata.json' })
  const preset = { temperature: { forcing: { metadata: 'gldas-noah-0.25' }, forcingMetadata: 'era5' } }
  mapStreamScenePaths(preset, (file) => `M/${file}`)
  assert.equal(preset.temperature.forcing.metadata, 'gldas-noah-0.25')
  assert.equal(preset.temperature.forcingMetadata, 'era5')
})

// 测试：通用格点 NetCDF 经 forcing-register 注册为数据包并出现在 era5/list 中（最多轮询 10 s）。
//   样例 NetCDF 放在本机 scratch 目录（不在仓库内），文件不存在时直接返回（相当于跳过）。
test('register: generic grids go through forcing-register and show in the pack list', { skip: !realEngine }, async () => {
  const { call } = makeService()
  const syntheticDirectory = 'C:\\work\\streamsim\\scratch\\items_20261122\\forcingmeta\\synth'
  const nc = join(syntheticDirectory, 'gldas3h.nc')
  if (!existsSync(nc)) return // synthetic NetCDF lives in the scratch area, not in the repository
  const job = await call('POST', '/api/stream/forcing/register', { files: [nc], metadata: 'gldas-noah-0.25', label: '测试 GLDAS' })
  assert.equal(job.status, 200)
  for (let i = 0; i < 100; i += 1) {
    await new Promise((done) => setTimeout(done, 100))
    const list = (await call('POST', '/api/stream/era5/list', {})).payload.packs
    if (list.length) { assert.equal(list[0].kind, 'stream-forcing-pack'); assert.equal(list[0].label, '测试 GLDAS'); assert.equal(list[0].metadata, 'gldas-noah-0.25'); return }
  }
  assert.fail('registered pack did not appear')
})

// 模块：S6 假引擎。fakeEngine 在临时资源目录下建 engine/（含 stream-defined/classes.json），
//   engineScript(exitCode) 生成的脚本会写出 partial_result.tif（内容仅 4 字节）与
//   scene_summary.json 后以 exitCode 退出，用来模拟引擎崩溃（1）或正常完成（0）。
// ---- S6: fake engine = a copy of node.exe named stream.exe running the extensionless script "run" ----
function fakeEngine(name, script) {
  const resource = join(root, name)
  mkdirSync(join(resource, 'engine', 'stream-defined'), { recursive: true })
  copyFileSync(process.execPath, join(resource, 'engine', 'stream.exe'))
  writeFileSync(join(resource, 'engine', 'stream-defined', 'classes.json'), '{}')
  writeFileSync(join(resource, 'engine', 'run'), script)
  return resource
}
const engineScript = (exitCode) => `
const { readFileSync, writeFileSync } = require('node:fs'); const { join } = require('node:path')
const config = JSON.parse(readFileSync(process.argv[2], 'utf8'))
writeFileSync(join(config.outputDir || require('node:path').dirname(process.argv[2]), 'partial_result.tif'), 'half')
writeFileSync(join(require('node:path').dirname(process.argv[2]), 'scene_summary.json'), '{}')
process.exit(${exitCode})`

// 作用：通过 /api/stream/run 提交一次假引擎运行，轮询 /runs 至多约 15 s 直到不再是 running，
//   再读 run-info 与 run-manifest.json；返回 { info, manifest } 并关闭服务。
async function runFake(name, exitCode) {
  const resource = fakeEngine(name, engineScript(exitCode))
  const { service, call } = makeService({ resourceRoot: resource, modelRoot: join(resource, 'models') })
  const outputRoot = join(root, `${name}-out`)
  const scene = { overpass: '2019-07-15T04:00:00Z', locationSource: 'input', name, outputRoot,
    surface: { type: { file: inputFile }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 } },
    view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] },
    temperature: { mode: 'rt', components: Object.fromEntries(['soilSunlit', 'soilShaded', 'leafSunlit', 'leafShaded'].map((key) => [key, { value: 300 }])) } }
  const submitted = await call('POST', '/api/stream/run', { scene })
  assert.equal(submitted.status, 200, JSON.stringify(submitted.payload))
  let runs = []
  for (let i = 0; i < 150 && !runs.some((run) => run.status !== 'running'); i += 1) {
    await new Promise((done) => setTimeout(done, 100))
    runs = (await call('POST', '/api/stream/runs', { outputRoot })).payload?.runs || []
  }
  assert.ok(runs.length && runs[0].status !== 'running', 'run did not finish')
  const info = (await call('POST', '/api/stream/run-info', { outputDir: runs[0].outputDir })).payload
  const manifest = JSON.parse(readFileSync(join(runs[0].outputDir, 'run-manifest.json'), 'utf8'))
  service.shutdown()
  return { info, manifest }
}

// 检查 S6：引擎退出码非 0 时 manifest.status 为 failed，且产物清单为空（不列出残缺的 .tif）。
test('S6: a crashed engine run does not list half-written .tif products', async () => {
  const { info, manifest } = await runFake('crash', 1)
  assert.equal(manifest.status, 'failed')
  assert.deepEqual(manifest.products, [])
  assert.deepEqual(info.products, [])
})

test('S6: a completed run still lists its products', async () => {
  const { info, manifest } = await runFake('good', 0)
  assert.equal(manifest.status, 'completed')
  assert.ok(manifest.products.includes('partial_result.tif'))
  assert.ok(info.products.some((path) => path.endsWith('partial_result.tif')))
})
