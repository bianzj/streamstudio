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
const scene = (energy = {}) => ({ overpass: '2019-07-15T04:00:00Z', locationSource: 'input',
  surface: { type: { file: inputFile }, lai: { value: 2 }, latitude: { value: 40 }, longitude: { value: 116 }, soilMoisture: { value: 0.2 } },
  view: { vza: { value: 0 }, vaa: { value: 0 } }, optical: { enabled: false }, thermal: { enabled: true, bands: [{ name: 'T', center: 10.5, fwhm: 0.5 }] },
  temperature: { mode: 'eb', soilMoisture: { source: 'user' }, era5: { pack }, energy } })
const build = (input) => store.buildEngineConfig(input, { outputDir: root, dataDir: root })

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

test('S7: stop() marks an active job that has no engine process yet, and launch() honours it', () => {
  const source = readFileSync(join(repo, 'server', 'stream', 'index.mjs'), 'utf8')
  assert.match(source, /active && active\.id === target && !active\.child/)
  assert.match(source, /if \(job\.stopped\) \{ finish\(job, 'stopped', null\); return \}/)
})

test('cache key: ctimeMs is part of the file identity', () => {
  const stamp = fileStamp(inputFile)
  assert.equal(stamp.key.split('|').length, 4)
  assert.match(stamp.key, /\|\d+(\.\d+)?$/)
})

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