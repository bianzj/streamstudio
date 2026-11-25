// ==========================================================================
// 文件：prepare.mjs
// 功能：回归测试第 1 步——用 assets/examples 的内置示例按 cases.mjs 的算例定义生成“冻结输入”，
//       流程与 server.mjs 的 /api/run 预处理一致，使两版引擎在完全相同的输入上对比。
// 主要步骤（逐算例）：
//   1. 复制示例工程到 <workDir>/cases/<算例>/project，应用 positions / files 替换（位置文件、附加 OBJ）；
//   2. 读 project.json 并规范化（统一工程格式经 unifiedToHistreamProject 转换后 normalizeProject）；
//   3. 设置模式、气象时段 meteo、set 路径覆盖，追加 addSpectra/addMaterials/addObjects；
//   4. 调 prepareRuntimeSceneProject 做实例筛选与裁切，把运行时输入复制到 input/；
//   5. 把气象文件复制为 input/meteo.txt 并改写路径，写出冻结的 project.json 与 case.json。
// 数据流：assets/examples + tools/regression/cases.mjs → <workDir>/cases/<算例>/{project,input,case.json}。
// 与其他文件的关系：cases.mjs 提供算例定义；run.mjs 用 input/ 运行；compare.py 对比结果。
//       依赖 src/renderer/src/project-schema.js、project-unified.js 与 tools/runtime-scene.mjs。
// 用法：node tools/regression/prepare.mjs <workDir> [算例名 ...]；示例或本脚本规范化逻辑变化后需重做并重跑基线。
// ==========================================================================

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

// 作用：按点分路径写值（如 "configuration.control.samples"），途中缺失的对象自动创建。
const setPath = (object, path, value) => {
  const keys = path.split('.')
  let target = object
  for (const key of keys.slice(0, -1)) target = target[key] ??= {}
  target[keys.at(-1)] = value
}

// 逐算例生成冻结输入；已存在的算例目录会被删除重建；未知算例名只报错并置非零退出码。
for (const name of names) {
  const spec = CASES[name]
  if (!spec) { console.error('unknown case', name); process.exitCode = 1; continue }
  const caseDir = join(work, 'cases', name)
  if (existsSync(caseDir)) rmSync(caseDir, { recursive: true, force: true })
  const projectCopy = join(caseDir, 'project')
  // 步骤 1：复制示例工程，使用副本以免改动仓库里的示例。
  cpSync(join(STUDIO, 'assets', 'examples', spec.example), projectCopy, { recursive: true })
  // Optional replacement position files (rows: x y z scale rotation).
  for (const [file, rows] of Object.entries(spec.positions || {}))
    writeFileSync(join(projectCopy, file), rows.map((row) => row.join(' ')).join('\n') + '\n', 'utf8')
  // Optional extra text files (e.g. an added OBJ) relative to the project.
  for (const [file, text] of Object.entries(spec.files || {})) {
    mkdirSync(dirname(join(projectCopy, file)), { recursive: true })
    writeFileSync(join(projectCopy, file), text, 'utf8')
  }
  // 步骤 2：去 BOM 后读工程；统一工程格式先转成 HiStream 工程再规范化，旧格式直接规范化。
  const source = join(projectCopy, 'project.json')
  const doc = JSON.parse(readFileSync(source, 'utf8').replace(/^\uFEFF/, ''))
  const project = unified.isUnifiedProject(doc)
    ? schema.normalizeProject(unified.unifiedToHistreamProject(unified.normalizeUnified(doc)))
    : schema.normalizeProject(doc)
  // 步骤 3：套用算例定义（模式、气象时段、任意路径覆盖、附加光谱/材料/对象）。
  // outDir 先设为占位值，运行时由 run.mjs 改写。
  project.mode = spec.mode
  const c = project.configuration
  c.outDir = join(caseDir, 'out-placeholder')
  if (spec.meteo) { c.meteo.start = spec.meteo[0]; c.meteo.end = spec.meteo[1] }
  for (const [path, value] of Object.entries(spec.set || {})) setPath(project, path, value)
  // Optional extra spectra / materials appended after normalization (vehicles).
  for (const item of spec.addSpectra || []) c.spectra.push(structuredClone(item))
  for (const item of spec.addMaterials || []) c.materials.push(structuredClone(item))
  // Optional scene objects appended after normalization (e.g. a water surface).
  for (const item of spec.addObjects || []) {
    c.objects.items.push(structuredClone(item))
    if (Array.isArray(c.objects.names)) c.objects.names.push(item.name)
    if ('count' in c.objects) c.objects.count = c.objects.items.length
  }
  // 资源路径解析：绝对路径原样；相对路径优先在工程目录找，找不到且以 assets/ 开头时回退到仓库共享资源。
  const assetPath = (value) => {
    const text = String(value || '').trim()
    if (!text) return ''
    if (isAbsolute(text)) return resolve(text)
    const local = resolve(projectCopy, text)
    const shared = /^assets[\\/]/i.test(text) ? resolve(STUDIO, text) : ''
    return shared && !existsSync(local) && existsSync(shared) ? shared : local
  }
  // 步骤 4：与 /api/run 相同的运行时场景准备（实例筛选/裁切）；地形高度固定为 0，使结果不依赖 DEM。
  // 随后把临时目录内容复制到 input/ 并清理临时目录。
  const runtime = prepareRuntimeSceneProject(source, project, { resolveAssetPath: assetPath, terrainHeight: () => 0 })
  const inputDir = join(caseDir, 'input')
  mkdirSync(inputDir, { recursive: true })
  cpSync(dirname(runtime.inputPath), inputDir, { recursive: true })
  runtime.cleanup()
  // 步骤 5：冻结气象——把气象文件复制进输入目录并改写路径，使后续运行与原气象文件的变化无关。
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
