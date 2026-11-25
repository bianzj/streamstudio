// ==========================================================================
// 文件：test-assimilation-config.mjs
// 功能：STREAM 同化/热红外配置编译的单元测试（Node 内置 test runner）。验证 server/stream/config.mjs
//       的 buildEngineConfig 对同化选项的转发与校验，以及 mapStreamScenePaths 的路径改写
// 运行：node --test tools/test-assimilation-config.mjs（仅检查配置，不运行原生引擎，输入文件为空文件）
// 主要用例：
//   1. 显式误差、QA、波长与来源标识被原样转发到引擎配置
//   2. 非法或相互矛盾的同化参数被拒绝（抛错）
//   3. mapStreamScenePaths 改写同化块内所有文件路径（含 views 列表）
//   4. RT 模式：土壤发射率设置转发、常量几何范围检查（2026-11-20 新增）、反射天空长波选项
// 与其他文件：配套原生契约测试 test-assimilation-contract.py（见 docs/ls4t-interop-20261106.md）。
// ==========================================================================

import assert from 'node:assert/strict'
import { test, after } from 'node:test'
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { createProjectStore, mapStreamScenePaths } from '../server/stream/config.mjs'

const dir = mkdtempSync(join(tmpdir(), 'stream-assimilation-contract-'))
after(() => rmSync(dir, { recursive: true }))
const file = join(dir, 'input.tif')
writeFileSync(file, '') // Config validation only; native tests exercise real rasters.
const store = createProjectStore({ normalizeHostPath: p => p, windowsPath: p => p })
// 夹具：最小可编译的同化场景（4 个温度分量常数 300 K，光学/热红外关闭），extra 覆盖 assimilation 字段。
const scene = extra => ({ overpass: '2019-07-15T04:00:00Z', locationSource: 'input',
  surface: { type: { file }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 } },
  view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: false },
  temperature: { mode: 'rt', components: Object.fromEntries(['soilSunlit','soilShaded','leafSunlit','leafShaded'].map(k => [k, { value: 300 }])) },
  assimilation: { mode: 'assimilate', observation: 'components', data: file, ...extra } })
const compile = extra => store.buildEngineConfig(scene(extra), { outputDir: dir, dataDir: dir })

test('forward explicit errors, QA, wavelength and provenance', () => {
  const as = compile({ uncertainty: file, qa: file, sigmaComponentK: 3, rejectQaBits: 256,
    lstWavelengthUm: 12, observationLevel: 'surface', sourceIds: ['scene'], priorSourceIds: ['forecast'] }).assimilation
  assert.equal(as.uncertainty, file)
  assert.equal(as.qa, file)
  assert.equal(as.sigmaComponentK, 3)
  assert.equal(as.lstWavelengthUm, 12)
  assert.deepEqual(as.sourceIds, ['scene'])
})
// 各无效输入：分量误差为 0、QA 位掩码非整数、波长为 0、观测层级未知、来源与先验来源重复。
test('reject invalid or dependent errors', () => {
  for (const extra of [{ sigmaComponentK: 0 }, { rejectQaBits: 1.2 }, { lstWavelengthUm: 0 },
    { observationLevel: 'toa' }, { sourceIds: ['scene'], priorSourceIds: ['scene'] }])
    assert.throws(() => compile(extra))
})
test('rewrite every assimilation file path', () => {
  const input = { assimilation: { data: 'a', uncertainty: 'b', qa: 'c', optical: 'd',
    opticalUncertainty: 'e', opticalQa: 'f', views: [{ data: 'g', uncertainty: 'h', qa: 'i' }] } }
  mapStreamScenePaths(input, p => `mapped/${p}`)
  assert.equal(input.assimilation.uncertainty, 'mapped/b')
  assert.equal(input.assimilation.opticalQa, 'mapped/f')
  assert.equal(input.assimilation.views[0].uncertainty, 'mapped/h')
})
// 2026-11-20: RT runs forward the soil emissivity settings; constant geometry is range-checked; reflected sky.
// 夹具：RT（正向辐射传输）场景——去掉同化块，打开一个热红外通道（中心 10.5 µm，半宽 0.5 µm）。
const rtScene = extra => ({ ...scene({}), assimilation: undefined, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] }, ...extra })
const compileRt = extra => store.buildEngineConfig(rtScene(extra), { outputDir: dir, dataDir: dir })
test('RT mode writes the soil emissivity settings', () => {
  const temperature = { ...rtScene({}).temperature, energy: { soilEmissivityModel: 'constant', soilEmissivityK: 2, surfaceTemperatureNormalisation: 'directional', soilEmissivity: 0.96 } }
  const energy = compileRt({ temperature }).temperature.energy
  assert.deepEqual(energy, { soilEmissivityModel: 'constant', soilEmissivityK: 2, surfaceTemperatureNormalisation: 'directional', soilEmissivity: 0.96 })
  assert.equal(compileRt({}).temperature.energy.soilEmissivityModel, 'hapke')
})
// 常量几何的范围：vza 120、vaa 400、纬度 200 被拒绝；经度 300 被接受（经度不做范围拒绝的原因待核对）。
test('reject out-of-range constant geometry', () => {
  assert.throws(() => compileRt({ view: { vza: { value: 120 }, vaa: { value: 0 } } }))
  assert.throws(() => compileRt({ view: { vza: { value: 0 }, vaa: { value: 400 } } }))
  assert.throws(() => compileRt({ surface: { ...rtScene({}).surface, latitude: { value: 200 } } }))
  assert.doesNotThrow(() => compileRt({ surface: { ...rtScene({}).surface, longitude: { value: 300 } } }))
})
test('reflected sky options', () => {
  assert.equal(compileRt({}).skyLongwave, undefined)
  assert.deepEqual(compileRt({ skyLongwave: { mode: 'none' } }).skyLongwave, { mode: 'none' })
  assert.deepEqual(compileRt({ skyLongwave: { value: 350 } }).skyLongwave, { value: 350 })
  assert.throws(() => compileRt({ skyLongwave: { value: 900 } }))
})
