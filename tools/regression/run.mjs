// ==========================================================================
// 文件：run.mjs
// 功能：回归测试第 2 步——用指定引擎目录运行已准备好的算例，记录退出码、耗时与日志。
// 主要步骤（逐算例）：
//   1. 把 <workDir>/cases/<算例>/input 复制到 runs/<variant>/<算例>（已有则先删除）；
//   2. 改写 outDir 与气象路径，按 REGRESSION_SET 环境变量覆盖参数，写 project.json；
//   3. 启动 histream.exe（模式 + 工程路径；FacetEB/FacetRT 还要给输出文件名），
//      stdout/stderr 落盘，超时则杀进程；
//   4. 写 result.json，最后汇总为 runs/<variant>/_summary_<时间戳>.json。
// 用法：node tools/regression/run.mjs <workDir> <variant> [--engine <binDir>] [算例名 ...]
//       缺省引擎：本仓库 models/bin_x64/Release；变体名通常取 base / new 以便 compare.py 对比。
// 环境变量：REGRESSION_SET="a.b=1;c.d=x"（值按 JSON 解析，失败则按字符串）；REGRESSION_TIMEOUT（秒，缺省 900）。
// 与其他文件的关系：cases.mjs 提供 CASES 与 FACET_OUTPUT；prepare.mjs 生成输入；compare.py 读取 out/。
// 注意：HISTREAM_FACET_STEP_SYNC=0 关闭 FacetEB 逐节点与服务端握手（该握手仅在 server.mjs 中开启）。
// ==========================================================================

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
// 要运行的算例：命令行指定的，或所有已完成 prepare 的算例。
const names = only.length ? only : Object.keys(CASES).filter((name) => existsSync(join(work, 'cases', name, 'input')))
const summary = []

// 逐算例运行；未 prepare 的算例只提示并跳过。
for (const name of names) {
  const spec = CASES[name]
  const inputDir = join(work, 'cases', name, 'input')
  if (!spec || !existsSync(inputDir)) { console.error('case not prepared:', name); continue }
  const runDir = join(work, 'runs', variant, name)
  if (existsSync(runDir)) rmSync(runDir, { recursive: true, force: true })
  cpSync(inputDir, runDir, { recursive: true })
  const out = join(runDir, 'out')
  mkdirSync(out, { recursive: true })
  // 步骤 2：输出目录指向本次运行目录；气象路径指向复制过来的 meteo.txt。
  const doc = JSON.parse(readFileSync(join(inputDir, 'project.json'), 'utf8'))
  doc.configuration.outDir = out
  if (doc.configuration.meteo?.path) doc.configuration.meteo.path = join(runDir, 'meteo.txt')
  // REGRESSION_SET 的临时覆盖（如采样数）；路径中间节点必须已存在。覆盖会写进运行目录的 project.json。
  for (const pair of (process.env.REGRESSION_SET || '').split(';').filter(Boolean)) {
    const [path, raw] = pair.split('=')
    const keys = path.split('.'); let target = doc
    for (const key of keys.slice(0, -1)) target = target[key]
    let value = raw; try { value = JSON.parse(raw) } catch {}
    target[keys.at(-1)] = value
  }
  const project = join(runDir, 'project.json')
  writeFileSync(project, JSON.stringify(doc, null, 2), 'utf8')
  // 步骤 3：命令行 = 模式 + 工程；FacetEB/FacetRT 额外传结果索引 JSON 的输出路径。
  const argv = [spec.mode, project]
  if (FACET_OUTPUT[spec.mode]) argv.push(join(out, FACET_OUTPUT[spec.mode]))
  const stdout = openSync(join(runDir, 'stdout.txt'), 'w')
  const stderr = openSync(join(runDir, 'stderr.txt'), 'w')
  const started = Date.now()
  // 等待进程结束；超时杀进程并以 'timeout' 作为退出码；启动失败记为 spawn-error。
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
