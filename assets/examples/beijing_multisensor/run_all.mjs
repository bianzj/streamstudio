#!/usr/bin/env node
// =====================================================================================
// 北京市 · 光学与热红外多传感器观测模拟：一键运行脚本
//
// 功能
//   ① 定位 STREAM 引擎（stream.exe）与数据目录；
//   ② 运行母工程（project.json，ERA5 能量平衡）一次，得到组分温度 components.tif；
//   ③ 依次运行 variants/<传感器>.json 与 variants/angles_*.json，它们复用母工程的 components.tif，
//      只做光学/热红外成像（RT 模式 + 组分温度文件）；
//   ④ 调用同目录 raster_tools.py（Python，需要 numpy + GDAL(osgeo)）统计各波段并写出
//      results/summary.csv、results/angles.csv、results/aggregation.csv（300 m → 1 km 聚合对比），
//      可选写出 reference/（精简参考结果）；
//   ⑤ 在终端打印一屏摘要，任何一次引擎运行退出码非 0 时脚本以非 0 退出。
//
// 数据流
//   project.json / variants/*.json ──(buildRunConfig)──> <out>/<id>/run-config.json
//     ──(stream.exe run)──> <out>/<id>/{optical_reflectance,tir_brightness_temperature,components,...}.tif
//   <out>/parent/components.tif ──(写入各变体的 temperature.components.file)──> 变体运行
//   <out>/run-manifest.json（各次运行的目录、退出码、耗时）──> raster_tools.py ──> results/*.csv
//
// 用法
//   node run_all.mjs [--engine <stream.exe>] [--data <stream-defined 目录>] [--out <输出目录>]
//                    [--python <python.exe>] [--only <id,id,...>] [--reuse-parent]
//                    [--write-reference] [--no-analysis]
//   --engine           引擎路径，缺省 <仓库>/models/bin_x64/Release/stream.exe（再退回环境变量 STREAM_EXE）
//   --data             引擎数据目录，缺省 <引擎目录>/stream-defined
//   --out              输出根目录，缺省 <案例目录>/output（该目录不入库）。不使用用户 AppData
//   --python           Python 解释器，缺省 C:\work\miniconda\python.exe，其次 PATH 里的 python
//   --only             只运行列出的运行 id（母工程 id 为 parent，传感器 id 见 sensors.json，角度变体 id 为文件名）
//   --reuse-parent     <out>/parent/components.tif 已存在时跳过母工程
//   --write-reference  同时重写案例目录的 reference/（默认不改动已有参考结果）
//   --no-analysis      只跑引擎，不做统计与聚合
// =====================================================================================
import { spawn, spawnSync } from 'node:child_process'
import { createWriteStream, existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve, basename } from 'node:path'
import { fileURLToPath } from 'node:url'

const caseDir = dirname(fileURLToPath(import.meta.url))

// 作用：解析命令行参数（--key value 与布尔开关）。
function parseArgs(argv) {
  const flags = new Set(['reuse-parent', 'write-reference', 'no-analysis'])
  const options = {}
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index]
    if (!arg.startsWith('--')) throw new Error(`无法识别的参数：${arg}`)
    const key = arg.slice(2)
    if (flags.has(key)) options[key] = true
    else {
      if (index + 1 >= argv.length) throw new Error(`参数 ${arg} 缺少取值`)
      options[key] = argv[++index]
    }
  }
  return options
}

// 作用：读取 JSON 文件（容忍 UTF-8 BOM）。
function readJson(file) {
  return JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, ''))
}

// 作用：写 JSON 文件（2 空格缩进）。
function writeJson(file, value) {
  writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`, 'utf8')
}

// 作用：定位引擎与数据目录，找不到时给出明确错误。
// 步骤 1：命令行 --engine；步骤 2：环境变量 STREAM_EXE；步骤 3：仓库默认位置（案例目录上溯三级）。
function locateEngine(options) {
  const repoRoot = resolve(caseDir, '..', '..', '..')
  const candidates = [options.engine, process.env.STREAM_EXE, join(repoRoot, 'models', 'bin_x64', 'Release', 'stream.exe')].filter(Boolean)
  const engine = candidates.map((path) => resolve(path)).find((path) => existsSync(path))
  if (!engine) throw new Error(`找不到 stream.exe（已尝试：${candidates.join('；') || '无'}）；请用 --engine 指定`)
  const data = resolve(options.data || join(dirname(engine), 'stream-defined'))
  if (!existsSync(data)) throw new Error(`找不到引擎数据目录：${data}；请用 --data 指定`)
  return { engine, data }
}

// 作用：把工程里相对工程文件的路径换成绝对路径。
function abs(projectDir, file) {
  return resolve(projectDir, String(file))
}

// 作用：把工程文件（stream-image-project v2）换成引擎配置 stream-image-config v2，
//       规则与 server/stream/config.mjs 的 buildEngineConfig 一致（本案例用到的字段子集）。
// 步骤 1：地表输入与观测角；步骤 2：光学/热红外波段；步骤 3：温度来源（EB 或 RT+组分温度文件）；
// 步骤 4：汇总输出目录、数据目录与线程。
function buildRunConfig(projectFile, { outputDir, dataDir, componentsFile }) {
  const project = readJson(projectFile)
  const scene = project.scene
  const base = dirname(projectFile)
  // 步骤 1
  const surface = {}
  for (const key of ['type', 'lai', 'dem']) if (scene.surface?.[key]?.file) surface[key] = { file: abs(base, scene.surface[key].file) }
  const view = { vza: { value: Number(scene.view.vza.value) }, vaa: { value: Number(scene.view.vaa.value) } }
  // 步骤 2
  const bandList = (block) => (block?.enabled ? block.bands.map((band) => ({ name: band.name, center: band.center, fwhm: band.fwhm })) : [])
  const optical = { enabled: Boolean(scene.optical?.enabled), bands: bandList(scene.optical) }
  const thermal = { enabled: Boolean(scene.thermal?.enabled), bands: bandList(scene.thermal) }
  // 步骤 3
  const input = scene.temperature
  const emission = (energy = {}) => ({
    soilEmissivityModel: energy.soilEmissivityModel === 'constant' ? 'constant' : 'hapke',
    surfaceTemperatureNormalisation: energy.surfaceTemperatureNormalisation === 'directional' ? 'directional' : 'retrieval'
  })
  let temperature
  if (input.mode === 'eb') {
    const energy = input.energy || {}
    temperature = {
      mode: 'eb',
      era5: { pack: abs(base, input.era5.pack) },
      soilMoisture: { source: input.soilMoisture?.source === 'user' ? 'user' : 'era5' },
      elevationCorrection: input.elevationCorrection !== false,
      energy: {
        scheme: 'tirt', soilHeatMethod: energy.soilHeatMethod || 'surface', temperatureMethod: energy.temperatureMethod || 'robust',
        diffuseFraction: energy.diffuseFraction ?? 0.2, enforceSolarLimit: energy.enforceSolarLimit !== false,
        spectralBinNm: energy.spectralBinNm ?? 50, soilThermal: energy.soilThermal === 'moisture' ? 'moisture' : 'constant',
        ...emission(energy),
        soilResistance: { method: energy.soilResistance?.method === 'sellers' ? 'sellers' : 'constant', value: energy.soilResistance?.value ?? 200 }
      }
    }
  } else {
    const file = componentsFile || abs(base, input.components.file)
    if (!existsSync(file)) throw new Error(`组分温度文件不存在：${file}`)
    temperature = { mode: 'rt', components: { file }, energy: emission(input.energy) }
  }
  // 步骤 4
  return {
    kind: 'stream-image-config', schemaVersion: 2, outputDir, dataDir,
    overpass: scene.overpass, surface, view, optical, thermal, temperature, threads: Number(scene.threads) || 0
  }
}

// 作用：以子进程运行 `stream.exe run <config>`，stdout/stderr 写入 stream.log，返回退出码与耗时。
function runEngine(engine, configFile, logFile) {
  return new Promise((resolvePromise) => {
    const started = Date.now()
    const log = createWriteStream(logFile)
    const child = spawn(engine, ['run', configFile], { windowsHide: true, cwd: dirname(engine) })
    child.stdout.pipe(log, { end: false })
    child.stderr.pipe(log, { end: false })
    child.on('error', (error) => { log.end(`spawn 失败：${error.message}\n`); resolvePromise({ exitCode: -1, seconds: 0 }) })
    child.on('close', (code) => { log.end(); resolvePromise({ exitCode: code ?? -1, seconds: (Date.now() - started) / 1000 }) })
  })
}

// 作用：列出本次要运行的条目：母工程、sensors.json 中的各传感器、variants/angles_*.json。
function planRuns(only) {
  const sensors = readJson(join(caseDir, 'sensors.json')).sensors
  const runs = [{ id: 'parent', kind: 'parent', project: join(caseDir, 'project.json') }]
  for (const sensor of sensors) runs.push({ id: sensor.id, kind: 'sensor', project: join(caseDir, sensor.variant) })
  const variantsDir = join(caseDir, 'variants')
  for (const name of readdirSync(variantsDir).filter((file) => /^angles_.*\.json$/.test(file)).sort()) {
    runs.push({ id: basename(name, '.json'), kind: 'angle', project: join(variantsDir, name) })
  }
  return only ? runs.filter((run) => only.has(run.id) || run.id === 'parent') : runs
}

// 作用：选 Python 解释器（--python，其次 miniconda，其次 PATH 里的 python）。
function pickPython(options) {
  const candidates = [options.python, 'C:\\work\\miniconda\\python.exe', 'python'].filter(Boolean)
  for (const python of candidates) {
    if (python !== 'python' && !existsSync(python)) continue
    const probe = spawnSync(python, ['-I', '-c', 'import numpy, osgeo'], { encoding: 'utf8' })
    if (probe.status === 0) return python
  }
  throw new Error('找不到带 numpy 与 GDAL(osgeo) 的 Python；请用 --python 指定（GDAL 可用 conda install gdal 或 pip 安装 GDAL 轮子）')
}

// 作用：主流程。
// 步骤 1：解析参数、定位引擎；步骤 2：准备输出目录与运行计划；步骤 3：母工程；
// 步骤 4：各传感器与角度变体；步骤 5：写运行清单并调用 Python 统计；步骤 6：打印摘要。
async function main() {
  const options = parseArgs(process.argv.slice(2))
  // 步骤 1
  const { engine, data } = locateEngine(options)
  const outRoot = resolve(options.out || join(caseDir, 'output'))
  mkdirSync(outRoot, { recursive: true })
  console.log(`引擎：${engine}\n数据：${data}\n输出：${outRoot}`)
  // 步骤 2
  const only = options.only ? new Set(String(options.only).split(',').map((id) => id.trim()).filter(Boolean)) : null
  const runs = planRuns(only)
  const manifest = { engine, caseDir, outRoot, startedAt: new Date().toISOString(), runs: [] }
  const parentDir = join(outRoot, 'parent')
  const componentsFile = join(parentDir, 'components.tif')
  // 步骤 3 与 4：逐个运行；变体都使用母工程的 components.tif
  for (const run of runs) {
    const outputDir = join(outRoot, run.id)
    const entry = { id: run.id, kind: run.kind, project: run.project, dir: outputDir }
    manifest.runs.push(entry)
    if (run.kind === 'parent' && options['reuse-parent'] && existsSync(componentsFile)) {
      Object.assign(entry, { exitCode: 0, seconds: 0, reused: true })
      console.log(`[${run.id}] 复用已有母工程输出`)
      continue
    }
    mkdirSync(outputDir, { recursive: true })
    const configFile = join(outputDir, 'run-config.json')
    try {
      writeJson(configFile, buildRunConfig(run.project, { outputDir, dataDir: data, componentsFile: run.kind === 'parent' ? undefined : componentsFile }))
    } catch (error) {
      Object.assign(entry, { exitCode: -2, seconds: 0, error: error.message })
      console.log(`[${run.id}] 配置失败：${error.message}`)
      if (run.kind === 'parent') break
      continue
    }
    const result = await runEngine(engine, configFile, join(outputDir, 'stream.log'))
    Object.assign(entry, result)
    console.log(`[${run.id}] 退出码 ${result.exitCode}，${result.seconds.toFixed(1)} s`)
    if (run.kind === 'parent' && result.exitCode !== 0) break
  }
  // 步骤 5
  manifest.finishedAt = new Date().toISOString()
  writeJson(join(outRoot, 'run-manifest.json'), manifest)
  if (!options['no-analysis'] && manifest.runs.some((run) => run.exitCode === 0)) {
    const python = pickPython(options)
    const args = ['-I', join(caseDir, 'raster_tools.py'), 'analyze', join(outRoot, 'run-manifest.json'), '--sensors', join(caseDir, 'sensors.json'), '--results', join(caseDir, 'results')]
    if (options['write-reference']) args.push('--reference', join(caseDir, 'reference'))
    const analysis = spawnSync(python, args, { encoding: 'utf8', maxBuffer: 1 << 26 })
    if (analysis.status !== 0) {
      console.log(`统计失败（退出码 ${analysis.status}）：\n${String(analysis.stderr).split('\n').slice(-15).join('\n')}`)
      process.exitCode = 1
    } else console.log(analysis.stdout)
  }
  // 步骤 6
  const failed = manifest.runs.filter((run) => run.exitCode !== 0)
  const total = manifest.runs.reduce((sum, run) => sum + (run.seconds || 0), 0)
  console.log(`共 ${manifest.runs.length} 次运行，失败 ${failed.length} 次${failed.length ? `（${failed.map((run) => run.id).join('、')}）` : ''}，引擎总耗时 ${total.toFixed(1)} s`)
  if (failed.length) process.exitCode = 1
}

main().catch((error) => { console.error(error.message); process.exitCode = 1 })
