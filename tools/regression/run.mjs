// Run prepared regression cases with one engine build.
// usage: node tools/regression/run.mjs <workDir> <variant> [--engine <binDir>] [case ...]
//   default engine: models/bin_x64/Release of this checkout
// env: REGRESSION_SET="a.b=1;c.d=x" (extra JSON overrides), REGRESSION_TIMEOUT (s, default 900)
// Output: <workDir>/runs/<variant>/<case>/{project.json,out/,stdout.txt,stderr.txt,result.json}
import { spawn } from 'node:child_process'
import { closeSync, cpSync, existsSync, mkdirSync, openSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { CASES, FACET_OUTPUT } from './cases.mjs'

const STUDIO = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')
const args = process.argv.slice(2)
let engineDir = join(STUDIO, 'models', 'bin_x64', 'Release')
const engineIndex = args.indexOf('--engine')
if (engineIndex >= 0) { engineDir = resolve(args[engineIndex + 1]); args.splice(engineIndex, 2) }
const [workArg, variant, ...only] = args
if (!workArg || !variant) { console.error('usage: node tools/regression/run.mjs <workDir> <variant> [--engine <binDir>] [case ...]'); process.exit(64) }
const work = resolve(workArg)
const exe = join(engineDir, 'histream.exe')
const timeoutSec = Number(process.env.REGRESSION_TIMEOUT || 900)
const names = only.length ? only : Object.keys(CASES).filter((name) => existsSync(join(work, 'cases', name, 'input')))
const summary = []

for (const name of names) {
  const spec = CASES[name]
  const inputDir = join(work, 'cases', name, 'input')
  if (!spec || !existsSync(inputDir)) { console.error('case not prepared:', name); continue }
  const runDir = join(work, 'runs', variant, name)
  if (existsSync(runDir)) rmSync(runDir, { recursive: true, force: true })
  cpSync(inputDir, runDir, { recursive: true })
  const out = join(runDir, 'out')
  mkdirSync(out, { recursive: true })
  const doc = JSON.parse(readFileSync(join(inputDir, 'project.json'), 'utf8'))
  doc.configuration.outDir = out
  if (doc.configuration.meteo?.path) doc.configuration.meteo.path = join(runDir, 'meteo.txt')
  for (const pair of (process.env.REGRESSION_SET || '').split(';').filter(Boolean)) {
    const [path, raw] = pair.split('=')
    const keys = path.split('.'); let target = doc
    for (const key of keys.slice(0, -1)) target = target[key]
    let value = raw; try { value = JSON.parse(raw) } catch {}
    target[keys.at(-1)] = value
  }
  const project = join(runDir, 'project.json')
  writeFileSync(project, JSON.stringify(doc, null, 2), 'utf8')
  const argv = [spec.mode, project]
  if (FACET_OUTPUT[spec.mode]) argv.push(join(out, FACET_OUTPUT[spec.mode]))
  const stdout = openSync(join(runDir, 'stdout.txt'), 'w')
  const stderr = openSync(join(runDir, 'stderr.txt'), 'w')
  const started = Date.now()
  const exitCode = await new Promise((done) => {
    const child = spawn(exe, argv, { cwd: engineDir, stdio: ['ignore', stdout, stderr], env: { ...process.env, HISTREAM_FACET_STEP_SYNC: '0' } })
    const timer = setTimeout(() => { child.kill(); done('timeout') }, timeoutSec * 1000)
    child.on('exit', (code) => { clearTimeout(timer); done(code) })
    child.on('error', (error) => { clearTimeout(timer); done('spawn-error:' + error.message) })
  })
  closeSync(stdout); closeSync(stderr)
  const result = { case: name, mode: spec.mode, variant, exe, exitCode, elapsedSec: (Date.now() - started) / 1000 }
  writeFileSync(join(runDir, 'result.json'), JSON.stringify(result, null, 1))
  summary.push(result)
  console.log(JSON.stringify(result))
}
writeFileSync(join(work, 'runs', variant, `_summary_${Date.now()}.json`), JSON.stringify(summary, null, 1))
