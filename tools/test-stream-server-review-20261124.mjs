// ==========================================================================
// 文件：test-stream-server-review-20261124.mjs
// 功能：STREAM 服务端 2026-11-24 评审修复的 node:test 测试：S2（过境年份范围）、S3（soilBrightness
//   校验）、S4（maxIterations 取正整数）、S5（先校验再创建输出目录）、E4（非 ASCII 的 ERA5 路径预检）。
// 主要模块：
//   1. 夹具：临时目录、气象包与输入文件占位、scene()（可覆盖 overpass/surface/energy）与 build()
//   2. S2：过境年份必须在 1900–2200（与引擎一致）
//   3. S3：soilBrightness 给出时必须有效，不得静默丢弃；空串视为未提供
//   4. S4：maxIterations 给出时必须为正整数（1e9 被拒绝，具体上限待核对）
//   5. S5：以源码文本断言 start() 中 buildEngineConfig 先于 mkdirSync
//   6. E4：unreachableNonAsciiPackFile 对没有可用 8.3 短名的非 ASCII 数据包文件路径报告
// 关系：被测模块 server/stream/config.mjs（createProjectStore）与 server/stream/index.mjs
//   （unreachableNonAsciiPackFile）；不依赖真实引擎。
// 运行：node --test tools/test-stream-server-review-20261124.mjs
// ==========================================================================

// Server-side tests for the 2026-11-24 review fixes S2 (year range), S3 (soilBrightness), S4 (maxIterations), S5 (validate before
// mkdir) and E4 (non-ASCII ERA5 path preflight).
// Run: node --test tools/test-stream-server-review-20261124.mjs
import assert from 'node:assert/strict'
import { test, after } from 'node:test'
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { unreachableNonAsciiPackFile } from '../server/stream/index.mjs'
import { createProjectStore } from '../server/stream/config.mjs'

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = mkdtempSync(join(tmpdir(), 'stream-review-server-'))
after(() => rmSync(root, { recursive: true, force: true }))
const identity = (path) => path
const pack = join(root, 'packs', 'pack.json')
mkdirSync(dirname(pack), { recursive: true })
writeFileSync(pack, JSON.stringify({ kind: 'stream-forcing-pack', schemaVersion: 1, product: 'era5', files: [] }))
const inputFile = join(root, 'input.tif')
writeFileSync(inputFile, '')
const store = createProjectStore({ normalizeHostPath: identity, windowsPath: identity, forcingDirectories: () => [join(root, 'documents', 'forcing-metadata')] })
// scene({ overpass, surface, energy })：最小 EB 场景，surface 并入地表段，energy 并入 temperature.energy；
// build(input)：编译为引擎配置，校验失败时抛错。
const scene = ({ overpass = '2019-07-15T04:00:00Z', surface = {}, energy = {} } = {}) => ({ overpass, locationSource: 'input',
  surface: { type: { file: inputFile }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 }, soilMoisture: { value: 0.2 }, ...surface },
  view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] },
  temperature: { mode: 'eb', soilMoisture: { source: 'user' }, era5: { pack }, energy } })
const build = (input) => store.buildEngineConfig(input, { outputDir: root, dataDir: root })

test('S2: overpass year must lie in 1900-2200 like the engine', () => {
  assert.doesNotThrow(() => build(scene({ overpass: '1900-01-01T00:00:00Z' })))
  assert.doesNotThrow(() => build(scene({ overpass: '2200-12-31T23:00:00Z' })))
  for (const overpass of ['1899-12-31T23:00:00Z', '0001-01-01T00:00:00Z', '2201-01-01T00:00:00Z', '9999-01-01T00:00:00Z'])
    assert.throws(() => build(scene({ overpass })), /1900–2200/, overpass)
})

// 测试：0、负数、非数值、0.01、1.5 均被拒绝（有效范围的具体边界待核对）；
//   0.3 与 { value: 0.4 } 两种写法透传，未给或空串则输出中没有该字段。
test('S3: soilBrightness must be valid when given (no silent drop)', () => {
  for (const value of [0, -0.2, 'abc', 0.01, 1.5]) assert.throws(() => build(scene({ surface: { soilBrightness: value } })), /soilBrightness/, String(value))
  assert.equal(build(scene({ surface: { soilBrightness: 0.3 } })).surface.soilBrightness, 0.3)
  assert.equal(build(scene({ surface: { soilBrightness: { value: 0.4 } } })).surface.soilBrightness, 0.4)
  assert.equal(build(scene()).surface.soilBrightness, undefined)
  assert.equal(build(scene({ surface: { soilBrightness: '' } })).surface.soilBrightness, undefined)
})

test('S4: maxIterations must be a positive integer when given', () => {
  for (const value of ['x', 0, -3, 2.5, 1e9]) assert.throws(() => build(scene({ energy: { maxIterations: value } })), /maxIterations/, String(value))
  assert.equal(build(scene({ energy: { maxIterations: 80 } })).temperature.energy.maxIterations, 80)
  assert.equal(build(scene({ energy: { maxIterations: '' } })).temperature.energy.maxIterations, undefined)
})

// 测试 S5：对 index.mjs 源码取 submitRun 之后的 start() 片段，断言 buildEngineConfig 的位置
//   早于 mkdirSync(job.outputDir)，即校验失败时不会留下空输出目录。（文本断言，较脆弱。）
test('S5: start() validates the configuration before it creates the output folder', () => {
  const source = readFileSync(join(repo, 'server', 'stream', 'index.mjs'), 'utf8')
  const start = source.slice(source.indexOf('start() {', source.indexOf('function submitRun')))
  const build = start.indexOf('projects.buildEngineConfig(scene, { outputDir: job.outputDir')
  const mkdir = start.indexOf('mkdirSync(job.outputDir')
  assert.ok(build > 0 && mkdir > build, 'buildEngineConfig must precede mkdirSync in start()')
})

// 测试 E4：netCDF 经 ANSI 代码页打开文件，引擎只能靠 Windows 8.3 短名读取非 ASCII 路径（见 index.mjs 的 FIX E4）；
//   shortName 注入为假实现：空串或仍含非 ASCII（无可用短名）时返回该数据文件路径，
//   返回纯 ASCII 短名则返回空串；ASCII 路径、无数据文件、索引不存在时同样返回空串。
test('E4: non-ASCII ERA5 file path without a usable short name is caught at submission', () => {
  const folder = join(root, '数据包')
  mkdirSync(folder, { recursive: true })
  writeFileSync(join(folder, 'a.nc'), 'x')
  writeFileSync(join(folder, 'era5-pack.json'), JSON.stringify({ files: [{ path: 'a.nc' }] }))
  const index = join(folder, 'era5-pack.json')
  assert.equal(unreachableNonAsciiPackFile(index, { shortName: () => '' }), join(folder, 'a.nc'))
  assert.equal(unreachableNonAsciiPackFile(index, { shortName: (file) => file }), join(folder, 'a.nc'))
  assert.equal(unreachableNonAsciiPackFile(index, { shortName: () => 'C:\\PACKS\\A.NC' }), '')
  assert.equal(unreachableNonAsciiPackFile(pack, { shortName: () => '' }), '')  // ASCII path / no data files
  assert.equal(unreachableNonAsciiPackFile(join(root, 'missing.json')), '')
})
