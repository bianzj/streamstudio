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
const rtScene = extra => ({ ...scene({}), assimilation: undefined, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] }, ...extra })
const compileRt = extra => store.buildEngineConfig(rtScene(extra), { outputDir: dir, dataDir: dir })
test('RT mode writes the soil emissivity settings', () => {
  const temperature = { ...rtScene({}).temperature, energy: { soilEmissivityModel: 'constant', soilEmissivityK: 2, surfaceTemperatureNormalisation: 'directional', soilEmissivity: 0.96 } }
  const energy = compileRt({ temperature }).temperature.energy
  assert.deepEqual(energy, { soilEmissivityModel: 'constant', soilEmissivityK: 2, surfaceTemperatureNormalisation: 'directional', soilEmissivity: 0.96 })
  assert.equal(compileRt({}).temperature.energy.soilEmissivityModel, 'hapke')
})
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
