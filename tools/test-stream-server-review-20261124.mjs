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

test('S5: start() validates the configuration before it creates the output folder', () => {
  const source = readFileSync(join(repo, 'server', 'stream', 'index.mjs'), 'utf8')
  const start = source.slice(source.indexOf('start() {', source.indexOf('function submitRun')))
  const build = start.indexOf('projects.buildEngineConfig(scene, { outputDir: job.outputDir')
  const mkdir = start.indexOf('mkdirSync(job.outputDir')
  assert.ok(build > 0 && mkdir > build, 'buildEngineConfig must precede mkdirSync in start()')
})

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
