// Prepare frozen regression inputs from the bundled examples, mirroring the
// /api/run preparation in server.mjs (normalizeProject, runtime scene
// filtering/clipping, frozen meteorology).
// usage: node tools/regression/prepare.mjs <workDir> [case ...]
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { dirname, isAbsolute, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { CASES } from './cases.mjs'

const STUDIO = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')
const ROOT = pathToFileURL(STUDIO + '/').href
const schema = await import(ROOT + 'src/renderer/src/project-schema.js')
const unified = await import(ROOT + 'src/renderer/src/project-unified.js')
const { prepareRuntimeSceneProject } = await import(ROOT + 'tools/runtime-scene.mjs')

const [workArg, ...only] = process.argv.slice(2)
if (!workArg) { console.error('usage: node tools/regression/prepare.mjs <workDir> [case ...]'); process.exit(64) }
const work = resolve(workArg)
const names = only.length ? only : Object.keys(CASES)

const setPath = (object, path, value) => {
  const keys = path.split('.')
  let target = object
  for (const key of keys.slice(0, -1)) target = target[key] ??= {}
  target[keys.at(-1)] = value
}

for (const name of names) {
  const spec = CASES[name]
  if (!spec) { console.error('unknown case', name); process.exitCode = 1; continue }
  const caseDir = join(work, 'cases', name)
  if (existsSync(caseDir)) rmSync(caseDir, { recursive: true, force: true })
  const projectCopy = join(caseDir, 'project')
  cpSync(join(STUDIO, 'assets', 'examples', spec.example), projectCopy, { recursive: true })
  // Optional replacement position files (rows: x y z scale rotation).
  for (const [file, rows] of Object.entries(spec.positions || {}))
    writeFileSync(join(projectCopy, file), rows.map((row) => row.join(' ')).join('\n') + '\n', 'utf8')
  const source = join(projectCopy, 'project.json')
  const doc = JSON.parse(readFileSync(source, 'utf8').replace(/^\uFEFF/, ''))
  const project = unified.isUnifiedProject(doc)
    ? schema.normalizeProject(unified.unifiedToHistreamProject(unified.normalizeUnified(doc)))
    : schema.normalizeProject(doc)
  project.mode = spec.mode
  const c = project.configuration
  c.outDir = join(caseDir, 'out-placeholder')
  if (spec.meteo) { c.meteo.start = spec.meteo[0]; c.meteo.end = spec.meteo[1] }
  for (const [path, value] of Object.entries(spec.set || {})) setPath(project, path, value)
  const assetPath = (value) => {
    const text = String(value || '').trim()
    if (!text) return ''
    if (isAbsolute(text)) return resolve(text)
    const local = resolve(projectCopy, text)
    const shared = /^assets[\\/]/i.test(text) ? resolve(STUDIO, text) : ''
    return shared && !existsSync(local) && existsSync(shared) ? shared : local
  }
  const runtime = prepareRuntimeSceneProject(source, project, { resolveAssetPath: assetPath, terrainHeight: () => 0 })
  const inputDir = join(caseDir, 'input')
  mkdirSync(inputDir, { recursive: true })
  cpSync(dirname(runtime.inputPath), inputDir, { recursive: true })
  runtime.cleanup()
  const runInput = join(inputDir, 'project.json')
  const frozen = JSON.parse(readFileSync(runInput, 'utf8'))
  const meteo = frozen.configuration.meteo?.path
  if (meteo && existsSync(meteo)) {
    copyFileSync(meteo, join(inputDir, 'meteo.txt'))
    frozen.configuration.meteo.path = join(inputDir, 'meteo.txt')
  }
  writeFileSync(runInput, schema.stringifyProject(frozen), 'utf8')
  writeFileSync(join(caseDir, 'case.json'), JSON.stringify({ name, ...spec }, null, 1))
  console.log(name, spec.mode, 'objects kept', runtime.kept, '/', runtime.total)
}
