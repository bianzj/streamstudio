// ==========================================================================
// 文件：server.mjs
// 功能：StreamStudio 的 Node 服务端入口（HTTP + SSE + 后台计算线程），既被 Electron 主进程
//       加载，也可独立运行（--api-only 只提供 /api）。负责工程文件读写、调起 histream 引擎、
//       读取/解码结果（TIFF、ENVI、面元/体元 JSON）并回传给 Vite 前端（src/renderer）。
// 主要模块（按文件内出现顺序）：
//   1. 常量、环境变量与全局运行状态（超时、结果缓存、后台线程池 heavyTasks、STREAM 服务）
//   2. 引擎定位与白名单（executable/allowedEngine）、计算设备枚举、文件 SHA-256 缓存
//   3. 隔离运行目录与 run-manifest（createIsolatedRun/writeRunManifest/verifyFrozenRunInputs）
//   4. 进程重置（resetSimulationProcesses）、JSON 响应与带缓存的结果读取（sendCachedResult）
//   5. 路径安全与路径映射（pathWithin/realPathWithin、normalizeHostPath、windowsPath 等）
//   6. Stream3D 代理部署检查（histreamProxyDeployment/Health）与 SSE 事件广播（emit）
//   7. 面元规模预检（runPreflight、assertFacetScale）与大气查找表采样（LUT、Planck）
//   8. 输出命名、面元/体元辐射结果读取与 TIFF 导出（readRadiosityResult、
//      writeRadiosityTiffForAngle 等）、面元逐步导出的 worker 调度
//   9. 工程文件读写与迁移、USD 同步、新建/另存/示例工程
//   10. 结果列表/删除、ENVI→TIFF 转换、TIFF/ENVI/DEM 读取与拉伸、结果对比
//   11. api()：全部 /api/* 路由的分发（运行、停止、气象、结果、STREAM 等）
//   12. 后台线程入口（task-service/task-once worker）、HTTP 服务器启动与退出清理
// 数据流：前端 → /api → 读写工程 JSON → 创建隔离运行目录并 spawn histream.exe → 进度经 SSE
//       推送 → 运行结束后在后台线程把引擎输出转成 TIFF/影像 → 前端按需通过 /api 读取结果。
// 与其他文件：src/renderer/src/project-schema.js、project-unified.js（工程模型与校验）；
//       server/stream/*（STREAM 遥感模拟服务）；server/result-cache.mjs（结果 LRU 缓存与
//       二进制编码）；server/task-pool.mjs（线程池）；tools/*.mjs（运行时场景、USD、篷布）。
// 约定：温度 K；波长 nm；角度除特别说明外为度；路径在内部统一为宿主机路径（normalizeHostPath），
//       返回给前端时用 windowsPath 转成 Windows 形式。histream 退出码见 HISTREAM_EXIT。
// 同一文件在多个文件范围内被分段加注释，函数/段落边界由相应负责人处理。
// ==========================================================================

import { createServer } from 'node:http'
import { execFile, spawn, spawnSync } from 'node:child_process'
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads'
import { createInterface } from 'node:readline'
import { closeSync, copyFileSync, cpSync, createReadStream, existsSync, lstatSync, mkdirSync, openSync, readFileSync, readSync, realpathSync, readdirSync, renameSync, rmdirSync, rmSync, statSync, unlinkSync, writeFileSync, writeSync } from 'node:fs'
import { copyFile as copyFileAsync, cp as cpAsync } from 'node:fs/promises'
import { createHash, randomUUID } from 'node:crypto'
import { inflateSync } from 'node:zlib'
import { basename, delimiter, dirname, extname, isAbsolute, join, relative, resolve } from 'node:path'
import { homedir } from 'node:os'
import { fileURLToPath } from 'node:url'
import { createDefaultProject, DEFAULT_SCENE_MODEL, normalizeProject, normalizeRunTimeoutMinutes, PROJECT_MODES, sensorBandValues, sensorViewAngles, stringifyProject, validateProject } from './src/renderer/src/project-schema.js'
import { applyHistreamToUnified, histreamProjectToUnified, isUnifiedProject, normalizeUnified, stringifyUnifiedProject, unifiedToHistreamProject } from './src/renderer/src/project-unified.js'
import { prepareRuntimeSceneProject } from './tools/runtime-scene.mjs'
import { generateTarpCover } from './tools/tarp-cover.mjs'
import { exportProjectUsd } from './tools/usd-export.mjs'
import { buildProjectFromUsd } from './tools/usd-import.mjs'
import { createStreamService } from './server/stream/index.mjs'
import { mapStreamScenePaths } from './server/stream/config.mjs'
import { BINARY_RESULT_TYPE, ByteLru, encodeBinaryResult, fileStamp, resultCacheLimitBytes } from './server/result-cache.mjs'
import { createRasterTools } from './server/stream/rasters.mjs'
import { createTaskPool, ownedBytes, receivedBytes, runTaskOnce, serveTaskOnce, serveTasks } from './server/task-pool.mjs'

// 模块 1：路径根、版本号与运行模式开关。
// PROJECT_ROOT 为源码根；RESOURCE_ROOT 为打包后的资源根（可由环境变量覆盖），GUI 输出目录在其下。
const PROJECT_ROOT = dirname(fileURLToPath(import.meta.url))
const APP_VERSION = (() => { try { return JSON.parse(readFileSync(join(PROJECT_ROOT, 'package.json'), 'utf8')).version || '' } catch { return '' } })()
const HELP_DOCUMENTS = { user: 'Stream_Studio_User_Manual_CN.pdf', theory: 'Stream_Studio_Theory_Manual_CN.pdf' }
// 读取环境变量：按顺序取第一个非空值，兼容新旧前缀（STREAM_STUDIO_* / HISTREAM_STUDIO_*）。
const envSetting = (...names) => {
  for (const name of names) {
    const value = process.env[name]
    if (value !== undefined && value !== '') return value
  }
  return undefined
}
const RESOURCE_ROOT = resolve(envSetting('STREAM_STUDIO_RESOURCE_ROOT', 'HISTREAM_STUDIO_RESOURCE_ROOT') || PROJECT_ROOT)
const GUI_OUTPUT_DIR = join(RESOURCE_ROOT, 'gui')
// --api-only：只提供 /api，不托管前端静态文件。
const apiOnly = process.argv.includes('--api-only')
const modes = new Set(PROJECT_MODES)
const MODEL_ROOT = join(PROJECT_ROOT, 'models')
// Optional Stream3D integration. Paths are supplied by the deployment
// environment; developer-machine locations must never be compiled into the
// product or exposed in the shipped server.
const DEFAULT_HISTREAM_PROXY_MANIFEST = ''
const DEFAULT_HISTREAM_PROXY_RUNNER = ''
const DEFAULT_HISTREAM_PROXY_PYTHON = ''
const MAX_FACET_TRIANGLES = 5_000_000
// 单次三维运行的硬上限：超时即终止整棵进程树；结果体积超过上限则跳过读取与结果处理。
const RUN_TIMEOUT_MS = Number(process.env.STREAM_STUDIO_RUN_TIMEOUT_MS) || 15 * 60 * 1000
const MAX_RESULT_BYTES = Number(process.env.STREAM_STUDIO_MAX_RESULT_BYTES) || 8 * 1024 * 1024 * 1024
// 工程可用 control.runTimeoutMinutes（1–1440 分钟）覆盖；0/缺省用上面的服务端默认。
const projectRunTimeoutMs = (project) => {
  const minutes = normalizeRunTimeoutMinutes(project?.configuration?.control?.runTimeoutMinutes)
  return minutes ? minutes * 60000 : RUN_TIMEOUT_MS
}
const runTimeoutLabel = (ms = RUN_TIMEOUT_MS) => (ms >= 60000
  ? `${Math.round(ms / 60000)} 分钟`
  : `${Math.max(1, Math.round(ms / 1000))} 秒`)
// histream.exe exit codes (see models/histream/src/main.cpp).  Only FacetEB uses
// 2 = "finished, some nodes non-converged, results kept"; usage errors moved to 64
// so they are no longer mistaken for a non-converged run.
// 退出码表需与 models/histream/src/main.cpp 保持一致；histreamResultCodes 给出“可视为成功并保留结果”的码。
const HISTREAM_EXIT = Object.freeze({ ok: 0, failed: 1, facetNonConverged: 2, resources: 3, initialization: 4, usage: 64 })
const histreamResultCodes = (mode) => mode === 'eFacetEB'
  ? [HISTREAM_EXIT.ok, HISTREAM_EXIT.facetNonConverged] : [HISTREAM_EXIT.ok]
const defaultSceneAsset = join(RESOURCE_ROOT, 'assets', 'obj-library', 'building', 'house_a.obj')
// 引擎候选路径（按优先级）：打包的 engine/、再依次为开发机的 Release/Debug 构建目录。
const histreamCandidates = [
  join(RESOURCE_ROOT, 'engine', 'histream.exe'),
  join(MODEL_ROOT, 'bin_x64', 'Release', 'histream.exe'),
  join(MODEL_ROOT, 'bin_x64', 'Debug', 'histream.exe'),
  join(MODEL_ROOT, 'histream', 'bin', 'Debug', 'histream.exe'),
  join(MODEL_ROOT, 'histream', 'bin', 'Release', 'histream.exe')
]
// 静态文件 MIME 表（前端托管用）；resultImageMime 仅用于结果影像的回传。
const mime = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.webp': 'image/webp', '.gif': 'image/gif', '.bmp': 'image/bmp', '.wasm': 'application/wasm', '.json': 'application/json; charset=utf-8', '.ico': 'image/x-icon', '.woff2': 'font/woff2' }
const resultImageMime = { '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.webp': 'image/webp', '.gif': 'image/gif', '.bmp': 'image/bmp' }
// 全局运行状态：同一时刻只允许一个引擎子进程（child）；runPreparing 表示正在准备运行目录，
// resettingProcesses 防止并发重置，projectFile/projectDir 为当前打开的工程，clients 为 SSE 连接集合。
let child = null
let runPreparing = false
let childExecutable = ''
let resettingProcesses = false
let histreamProxyRequestInFlight = false
let projectFile = ''
let projectDir = ''
let projectFileChooser = null
const clients = new Set()
// 结果读取缓存。服务线程中缓存已编码的接口响应；后台计算线程各自缓存原始 TIFF/ENVI 数据与已解码分块
// （上限为总设置按线程数均分）。总字节上限默认 512 MB，STREAM_STUDIO_RESULT_CACHE_MB 调整，设为 0 关闭。
// 键含 (路径, mtime, size)，文件被覆盖即失效。
const resultCache = new ByteLru(resultCacheLimitBytes())
// 后台计算线程数：STREAM_STUDIO_TASK_WORKERS，默认 2；0 表示与旧版相同，在服务线程内同步执行。
const TASK_WORKERS = (() => {
  const raw = process.env.STREAM_STUDIO_TASK_WORKERS
  const count = raw === undefined || raw === '' ? 2 : Math.floor(Number(raw))
  return Number.isFinite(count) ? Math.max(0, Math.min(8, count)) : 2
})()
// 后台计算线程池：结果读取、TIFF 导出等重 CPU/IO 任务在 worker 中运行，避免阻塞 HTTP 事件循环；
// TASK_WORKERS=0 时退化为在服务线程内同步执行（runInline）。每线程缓存预算 = 总缓存 / 线程数。
const heavyTasks = createTaskPool({
  size: TASK_WORKERS,
  createWorker: () => new Worker(new URL(import.meta.url), facetStepWorkerOptions('task-service', {
    cacheBytes: TASK_WORKERS ? Math.floor(resultCacheLimitBytes() / TASK_WORKERS) : 0
  })),
  runInline: (task, args) => runHeavyTaskInline(task, args)
})
// 运行结束后的出图/ENVI 转换：一次性线程，结束后释放全部内存；“停止”可终止它。
function runPostprocessTask(running, task, args) {
  const inline = () => runHeavyTaskInline(task, args)
  if (!TASK_WORKERS) return Promise.resolve().then(inline)
  let current = null
  return runTaskOnce({
    task, args, runInline: (name, values) => runHeavyTaskInline(name, values),
    createWorker: () => new Worker(new URL(import.meta.url), facetStepWorkerOptions('task-once', { task, args, cacheBytes: 0 })),
    onWorker: (worker) => {
      if (worker) { current = worker; running.facetWorker = worker }
      else if (running.facetWorker === current) running.facetWorker = null
    }
  })
}
// STREAM remote-sensing image simulation: an independent module with its own API, queue and events.
// STREAM 遥感影像模拟服务：独立模块，把本文件的 json/body/readTiff 等工具函数注入，避免循环依赖。
const streamService = createStreamService({ resourceRoot: RESOURCE_ROOT, modelRoot: MODEL_ROOT, json, body, readTiff, normalizeHostPath, windowsPath,
  runTask: (task, args, options) => heavyTasks.run(task, args, options).then(toJsonBytes) })

// 返回第一个存在的引擎候选；都不存在时返回首选路径（供后续报“找不到引擎”）。
function executable() {
  return histreamCandidates.find(existsSync) || histreamCandidates[0]
}

// 作用：校验请求给出的引擎路径只能是内置候选或环境变量白名单中的路径，返回匹配到的规范路径。
// 输入：请求中的 engine 字符串（空则用默认引擎）；不在白名单时抛错。
// 说明：Windows 下路径比较忽略大小写；同时按 realpath 比较以处理 junction/符号链接。
// /api/run must never spawn an arbitrary program supplied by a request.
function allowedEngine(value) {
  if (!String(value || '').trim()) return executable()
  const requested = normalizeHostPath(value)
  const allowed = [
    ...histreamCandidates,
    ...String(envSetting('STREAM_STUDIO_ENGINE_ALLOWLIST', 'HISTREAM_STUDIO_ENGINE_ALLOWLIST') || '').split(delimiter)
      .map((item) => item.trim()).filter(Boolean).map((item) => resolve(normalizeHostPath(item)))
  ]
  const identity = (path) => {
    const normalized = resolve(path).replaceAll('\\', '/')
    return process.platform === 'win32' ? normalized.toLowerCase() : normalized
  }
  const match = allowed.find((candidate) => sameResolvedPath(candidate, requested) || identity(candidate) === identity(requested))
  if (!match) throw new Error(`不允许的 HiStream 引擎路径：${windowsPath(requested)}（仅允许内置引擎或 STREAM_STUDIO_ENGINE_ALLOWLIST 中列出的路径）`)
  return match
}

// 计算设备枚举：以引擎路径为键缓存 Promise，避免每次都创建 Vulkan 实例。
const deviceListCache = new Map()
// Asks the engine for its GPUs and CPU driver; cached per engine because a
// Vulkan instance takes a few seconds to create.
function listComputeDevices(engine, refresh = false) {
  if (!existsSync(engine)) return Promise.reject(new Error(`找不到 HiStream：${windowsPath(engine)}`))
  if (!refresh && deviceListCache.has(engine)) return deviceListCache.get(engine)
  const pending = new Promise((resolveDevices, reject) => {
    execFile(engine, ['--list-devices'], { cwd: dirname(engine), windowsHide: true, encoding: 'utf8', timeout: 60000, maxBuffer: 4 * 1024 * 1024 }, (error, stdout, stderr) => {
      const prefix = 'HISTREAM_DEVICES\t'
      const line = String(stdout || '').split(/\r?\n/).find((item) => item.startsWith(prefix))
      if (!line) return reject(new Error(`查询计算设备失败：${String(stderr || error?.message || '').trim() || '引擎无输出'}`))
      try { resolveDevices(JSON.parse(line.slice(prefix.length))) } catch (parseError) { reject(parseError) }
    })
  })
  deviceListCache.set(engine, pending)
  pending.catch(() => deviceListCache.delete(engine))
  return pending
}

// 模块：文件 SHA-256 身份（fileIdentity/fileIdentityAsync）。run-manifest 要记录引擎、工程、气象的哈希，
// 用于运行后校验输入是否被改动。
// SHA-256 按 (路径, mtime, size) 缓存：引擎 exe、气象文件等在每次运行准备时都要记录哈希，
// 内容未变时不再整文件重读。文件在哈希期间被改动（前后身份不一致）则不缓存。
const fileHashCache = new Map()
const rememberFileHash = (before, sha256) => {
  const after = fileStamp(before.path)
  if (after?.key === before.key) fileHashCache.set(before.path, { key: before.key, sha256 })
}

// 作用：同步计算文件的 {path, bytes, sha256}，命中缓存时不重读；文件不存在返回 null。
// 说明：分 1 MB 块读入；仅用于小文件或非关键路径，运行准备请用 fileIdentityAsync。
function fileIdentity(path) {
  const resolved = resolve(normalizeHostPath(path))
  const stamp = fileStamp(resolved)
  if (!stamp) return null
  const cached = fileHashCache.get(stamp.path)
  if (cached?.key === stamp.key) return { path: resolved, bytes: stamp.size, sha256: cached.sha256 }
  const hash = createHash('sha256')
  const descriptor = openSync(resolved, 'r')
  const buffer = Buffer.allocUnsafe(1024 * 1024)
  try {
    let count
    while ((count = readSync(descriptor, buffer, 0, buffer.length, null)) > 0) hash.update(buffer.subarray(0, count))
  } finally { closeSync(descriptor) }
  const sha256 = hash.digest('hex')
  rememberFileHash(stamp, sha256)
  return { path: resolved, bytes: statSync(resolved).size, sha256 }
}

// 运行准备阶段用的流式版本：大文件（引擎、气象、工程）分块读入，不阻塞事件循环。
async function fileIdentityAsync(path) {
  const resolved = resolve(normalizeHostPath(path))
  const stamp = fileStamp(resolved)
  if (!stamp) return null
  const cached = fileHashCache.get(stamp.path)
  if (cached?.key === stamp.key) return { path: resolved, bytes: stamp.size, sha256: cached.sha256 }
  const hash = createHash('sha256')
  await new Promise((resolveHash, reject) => {
    createReadStream(resolved, { highWaterMark: 1024 * 1024 })
      .on('data', (chunk) => hash.update(chunk))
      .on('error', reject)
      .on('end', resolveHash)
  })
  const sha256 = hash.digest('hex')
  rememberFileHash(stamp, sha256)
  return { path: resolved, bytes: statSync(resolved).size, sha256 }
}

// 作用：合并 patch 并原子写出 run-manifest.json（先写 .tmp 再 rename），同时刷新 updatedAt。
// manifest 记录本轮运行的输入哈希、请求的节点/视角、已完成产物，供运行后复核。
function writeRunManifest(run, patch = {}) {
  Object.assign(run.manifest, patch, { updatedAt: new Date().toISOString() })
  const path = join(run.outputDir, 'run-manifest.json')
  writeFileSync(path + '.tmp', JSON.stringify(run.manifest, null, 2) + '\n', 'utf8')
  // Native runs can enter Chinese project folders through an ASCII junction.
  // Resolve both sides before atomic publication; Windows may reject alias rename.
  renameSync(realpathSync(path + '.tmp'), join(realpathSync(dirname(path)), basename(path)))
}

// 作用：出图前复核“冻结工程”和“气象快照”的 SHA-256 与 manifest 一致，不一致就拒绝生成影像。
// Image export rereads the frozen project and meteorology; refuse to use them
// if either changed after the run recorded its hashes.
function verifyFrozenRunInputs(manifest, runInput) {
  const checks = [
    [runInput, manifest.runtimeInput?.sha256, '本轮冻结工程'],
    [manifest.meteorology?.snapshot?.path, manifest.meteorology?.snapshot?.sha256, '本轮气象快照']
  ]
  for (const [path, expected, label] of checks) {
    if (!expected) continue
    const actual = path ? fileIdentity(path) : null
    if (!actual || actual.sha256 !== expected) throw new Error(`${label}已被修改或丢失，拒绝生成结果影像：${windowsPath(path || '')}`)
  }
}

// 作用：列出运行目录（含 process、.facet_steps 子目录）中的产物文件，返回相对路径（正斜杠、已排序）。
// 排除 manifest、源工程副本和引擎的临时文件。
function runProductPaths(directory) {
  const files = []
  for (const subdirectory of [directory, join(directory, 'process'), join(directory, '.facet_steps')]) {
    if (!existsSync(subdirectory)) continue
    for (const entry of readdirSync(subdirectory, { withFileTypes: true })) {
      if (entry.isFile() && !['run-manifest.json', 'run-manifest.json.tmp', 'source-project.json'].includes(entry.name)
        && !entry.name.startsWith('.histream-transient-')) files.push(join(subdirectory, entry.name))
    }
  }
  return files.map(path => relative(directory, path).replaceAll('\\', '/')).sort()
}

// 作用：为一次运行创建隔离输出目录 <outDir>/runs/<runId>，冻结输入并写出初始 manifest。
// 输入：工程文件路径 input、规范化后的 project、运行模式 mode、引擎路径 engine。
// 输出：{ runId, outputDir, manifest, meteoPath }。
// 步骤 1：确定气象文件（缺省/内置名 → 引擎旁 defined/meteo.txt）并计算哈希；能量平衡模式必须有气象。
// 步骤 2：由 meteo.start/end 展开本轮要计算的节点（与引擎规则一致，至少一个节点）。
// 步骤 3：组装 manifest（请求的视角、观测几何、是否出图/过程输出），记录引擎 build-manifest。
// 步骤 4：创建目录，复制源工程为 source-project.json，写 manifest。
async function createIsolatedRun(input, project, mode, engine) {
  const baseDir = dirname(input)
  const root = resolve(baseDir, normalizeHostPath(project.configuration.outDir || 'output'))
  const runId = new Date().toISOString().replace(/[:.]/g, '-') + '-' + randomUUID().slice(0, 8)
  const outputDir = join(root, 'runs', runId)
  const configuredMeteo = String(project.configuration.meteo?.path || '')
  const meteoPath = !configuredMeteo || ['defined/meteo.txt', 'HiStream 内置气象数据'].includes(configuredMeteo)
    ? join(dirname(engine), 'defined', 'meteo.txt') : projectAssetPath(configuredMeteo, baseDir)
  const meteo = await fileIdentityAsync(meteoPath)
  if (isEnergyBalanceMode(mode) && !meteo) throw new Error('找不到本轮气象驱动：' + meteoPath)
  const [sourceInput, engineIdentity] = await Promise.all([fileIdentityAsync(input), fileIdentityAsync(engine)])
  const sensor = project.configuration.sensor
  const perspective = ['perspective', 'central', 'center'].includes(String(sensor.projection))
  // 节点范围的规整与引擎一致：start 非负，end 至少为 start+1。
  // Requested nodes follow the engine rule (fileio_json.cpp / facetrtcore.cpp):
  // start = max(0, start), end = max(start + 1, end) — end <= start still runs one node.
  const meteoStart = Math.max(0, Math.round(Number(project.configuration.meteo?.start) || 0))
  const meteoEnd = Math.max(meteoStart + 1, Math.round(Number(project.configuration.meteo?.end) || 0))
  const nodes = isEnergyBalanceMode(mode)
    ? Array.from({ length: meteoEnd - meteoStart }, (_, index) => meteoStart + index) : []
  const run = { runId, outputDir, manifest: {
    kind: 'histream-run', schemaVersion: 1, runId, mode, status: 'preparing', startedAt: new Date().toISOString(),
    outputDir: windowsPath(outputDir),
    sourceInput, engine: engineIdentity, meteorology: meteo,
    requested: { nodes, nodeCount: isEnergyBalanceMode(mode) ? nodes.length : 1,
      viewAngles: perspective ? [[sensor.vza, sensor.vaa]] : sensorViewAngles(sensor, project.configuration.light.azimuth),
      observation: {
        planeAzimuth: project.configuration.light.azimuth,
        principalPlane: !perspective && Boolean(sensor.principalPlane),
        hemisphere: !perspective && Boolean(sensor.hemisphere),
        projection: sensor.projection
      },
      image: Boolean(sensor.image), process: processOutputEnabled(JSON.stringify(project), mode) },
    completed: { nodes: [], nodeCount: 0, products: [] }
  } }
  const buildManifestPath = join(dirname(engine), 'build-manifest.json')
  if (existsSync(buildManifestPath)) {
    try {
      const build = JSON.parse(readFileSync(buildManifestPath, 'utf8'))
      run.manifest.build = { ...fileIdentity(buildManifestPath), applicationVersion: build.applicationVersion, sourceTreeSha256: build.sourceTreeSha256 }
    } catch { run.manifest.build = { ...fileIdentity(buildManifestPath), unreadable: true } }
  }
  mkdirSync(outputDir, { recursive: true })
  await copyFileAsync(input, join(outputDir, 'source-project.json'))
  writeRunManifest(run)
  return { ...run, meteoPath }
}

// 用信号 0 探测进程是否存在（不发送真实信号）。
function processExists(pid) {
  if (!pid) return false
  try { process.kill(pid, 0); return true }
  catch { return false }
}

// 封装 Windows taskkill.exe，返回 { label, killed, detail }；失败不抛出，由调用方判断。
function taskkill(args, label) {
  return new Promise((resolveTaskkill) => {
    execFile('taskkill.exe', args, { windowsHide: true, encoding: 'utf8', maxBuffer: 1024 * 1024 }, (error, stdout, stderr) => {
      resolveTaskkill({ label, killed: !error, detail: String(stdout || stderr || '').trim() })
    })
  })
}

// 作用：终止本服务启动的引擎进程树（“重置模拟环境”）。只杀自己的进程，不波及其他实例。
// 输出：{ activePid, terminated }；进程无法终止时抛错并恢复 child 状态。
async function resetSimulationProcesses() {
  if (resettingProcesses) throw new Error('模拟环境正在重置')
  resettingProcesses = true
  const running = child
  // 引擎已退出、只在做结果后处理时不按 PID 结束进程（PID 可能已被系统复用）。
  const runningPid = running && !running.histreamFinished ? running.pid || 0 : 0
  if (running) running.histreamReset = true
  const results = []
  try {
    // 步骤 1：先终止后处理 worker；histreamReset 标记让引擎退出回调不再按失败处理。
    if (running?.facetWorker) { try { await running.facetWorker.terminate() } catch {} }
    if (process.platform === 'win32') {
      // Only this server's own engine tree; other Stream Studio instances or
      // validation scripts may be running histream.exe concurrently.
      if (runningPid) results.push(await taskkill(['/PID', String(runningPid), '/T', '/F'], `PID ${runningPid}`))
    } else if (running) {
      running.kill('SIGKILL')
      results.push({ label: `PID ${runningPid}`, killed: true, detail: '' })
    }
    // 步骤 2：杀进程后复核；仍存在则回滚状态并报错，不清空 child。
    if (runningPid && processExists(runningPid)) {
      if (running) running.histreamReset = false
      child = running
      throw new Error(`无法终止模拟进程 PID ${runningPid}`)
    }
    child = null
    childExecutable = ''
    return { activePid: runningPid || null, terminated: results.filter((item) => item.killed).map((item) => item.label) }
  } finally {
    resettingProcesses = false
  }
}


// 模块 4：HTTP 响应工具。所有 JSON 响应都带防嵌入头（禁止被 iframe 嵌入）。
const FRAME_HEADERS = { 'X-Frame-Options': 'DENY', 'Content-Security-Policy': "frame-ancestors 'none'" }

// 后台线程已序列化好的 JSON 响应体（UTF-8 字节），json() 原样发送，与 JSON.stringify 结果逐字节相同。
class JsonBytes {
  constructor(bytes) { this.bytes = bytes }
}

// 统一的 JSON 响应；值为 JsonBytes 时直接发送后台线程已序列化的字节，避免主线程二次序列化。
function json(response, status, value) {
  response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', ...FRAME_HEADERS })
  response.end(value instanceof JsonBytes ? value.bytes : JSON.stringify(value))
}

// 编码结果读取响应：二进制块（客户端声明支持且有大数组时）或 JSON。在后台线程中执行。
function encodeResultPayload(result, binary) {
  const encoded = binary ? encodeBinaryResult(result) : null
  return encoded
    ? { bytes: encoded, contentType: BINARY_RESULT_TYPE }
    : { bytes: Buffer.from(JSON.stringify(result), 'utf8'), contentType: 'application/json; charset=utf-8' }
}

// 结果读取分发（在后台线程执行）：按 kind 选择 ENVI/风场 TIFF/TIFF/面元/过程结果读取器。
function produceResult(kind, path, band) {
  if (kind === 'envi') return readEnvi(path, band)
  if (kind === 'wind') return readWindTiff(path)
  if (kind === 'tiff') return readTiff(path, band)
  if (kind === 'facet') {
    const result = sampleRadiosityResult(path)
    return { ...result, path: windowsPath(result.path) }
  }
  if (kind === 'process') {
    const result = readProcessResult(path)
    return { ...result, path: windowsPath(result.path) }
  }
  throw new Error(`未知的结果类型：${kind}`)
}

// 正在计算的同一响应（同一缓存键）只算一次，后到的请求等待同一结果。
const resultInFlight = new Map()

// 结果读取接口：已编码的响应按依赖文件的 (路径, mtime, size) + 变体缓存，X-Cache 标明命中与否。
// 请求头 Accept 含 BINARY_RESULT_TYPE 时，大数值数组（几何、逐面元/体元指标）以二进制块回传；
// 否则保持原来的 JSON。读取、解析与编码在后台线程执行（job = { kind, path, band }）。
async function sendCachedResult(request, response, dependencies, variant, job) {
  const binary = String(request.headers.accept || '').includes(BINARY_RESULT_TYPE)
  // 步骤 1：由依赖文件的 (路径, mtime, size)、变体和编码方式拼出缓存键；任一依赖文件不存在则不缓存。
  const stamps = dependencies.map((path) => fileStamp(path))
  const key = stamps.every(Boolean) ? `result:${variant}|${binary ? 'binary' : 'json'}|${stamps.map((stamp) => stamp.key).join('|')}` : ''
  let cached = key ? resultCache.get(key) : undefined
  const hit = Boolean(cached)
  // 步骤 2：未命中时合并同键的并发请求（resultInFlight），交给后台线程读取并编码。
  if (!cached) {
    let pending = key ? resultInFlight.get(key) : undefined
    if (!pending) {
      pending = heavyTasks.run('result-read', { ...job, binary }, { key: job.path }).then(({ bytes, contentType }) => {
        const produced = { payload: receivedBytes(bytes), contentType }
        // 读取期间文件被改写（引擎仍在写出）时不缓存，下次重新读取。
        if (key && dependencies.every((path, index) => fileStamp(path)?.key === stamps[index].key)) resultCache.set(key, produced, produced.payload.length, stamps)
        return produced
      })
      if (key) {
        resultInFlight.set(key, pending)
        const forget = () => { if (resultInFlight.get(key) === pending) resultInFlight.delete(key) }
        pending.then(forget, forget)
      }
    }
    cached = await pending
  }
  const { payload, contentType } = cached
  if (process.env.STREAM_STUDIO_RESULT_CACHE_DEBUG === '1') {
    console.log(`STREAM STUDIO: 结果缓存 ${hit ? 'hit' : 'miss'} ${variant} ${(payload.length / 1048576).toFixed(2)} MB，占用 ${(resultCache.bytes / 1048576).toFixed(1)}/${(resultCache.limit / 1048576).toFixed(0)} MB，${windowsPath(dependencies[0])}`)
  }
  // 步骤 3：写出响应；X-Cache 标明 hit/miss，no-store 避免浏览器自行缓存。
  response.writeHead(200, {
    'Content-Type': contentType,
    'Content-Length': payload.length, 'Cache-Control': 'no-store', 'X-Cache': hit ? 'hit' : 'miss', ...FRAME_HEADERS
  })
  response.end(payload)
}

// 过程结果的缓存依赖：元数据 + 数据文件；面元几何另依赖 geometryFile（缺省 ../faceteb.json）。
function processResultDependencies(metadataPath, metadata) {
  const dependencies = [metadataPath, resolve(dirname(metadataPath), String(metadata.dataFile || ''))]
  if (metadata.geometry === 'facet') dependencies.push(resolve(dirname(metadataPath), String(metadata.geometryFile || '../faceteb.json')))
  return dependencies
}

// 模块 5：路径安全。pathWithin 做纯字符串层面的“是否在 root 之内”判断（root 本身也算）。
function pathWithin(root, candidate) {
  const base = resolve(root)
  const target = resolve(candidate)
  const rel = relative(base, target)
  return rel === '' || (rel !== '..' && !rel.startsWith(`..${process.platform === 'win32' ? '\\' : '/'}`) && !isAbsolute(rel))
}

// 在 pathWithin 基础上解析符号链接/junction 的真实路径，防止通过链接逃出 root。
// allowMissing=true 时对尚不存在的目标，改查其最近的已存在祖先。
function realPathWithin(root, candidate, allowMissing = false) {
  let realRoot
  try { realRoot = realpathSync(root) } catch { return false }
  let realTarget
  try { realTarget = realpathSync(candidate) }
  catch {
    if (!allowMissing || !pathWithin(realRoot, candidate)) return false
    let ancestor = resolve(candidate)
    while (!existsSync(ancestor)) {
      const parent = dirname(ancestor)
      if (parent === ancestor) return false
      ancestor = parent
    }
    try { return pathWithin(realRoot, realpathSync(ancestor)) } catch { return false }
  }
  return pathWithin(realRoot, realTarget)
}

function sameResolvedPath(left, right) {
  if (!existsSync(left) || !existsSync(right)) return false
  try {
    const a = resolve(realpathSync(left)).replaceAll('\\', '/')
    const b = resolve(realpathSync(right)).replaceAll('\\', '/')
    return process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b
  } catch { return false }
}

// 代理产物的哈希缓存：体量很大，仅在 size 或 mtime 变化时重算。
const sha256Cache = new Map()

// Proxy artifacts include large dense volumes; rehash only when size or mtime changes.
function sha256File(path) {
  const key = resolve(path)
  const stat = statSync(key)
  const cached = sha256Cache.get(key)
  if (cached && cached.size === stat.size && cached.mtimeMs === stat.mtimeMs) return cached.hash
  const hash = fileIdentity(key).sha256
  sha256Cache.set(key, { size: stat.size, mtimeMs: stat.mtimeMs, hash })
  return hash
}

// 模块 6：Stream3D（histream-proxy）代理部署检查。
// 作用：读取并严格校验代理部署清单（manifest），确认模型、体数据、抓取帧等产物齐全、路径受控、
//       SHA-256 与清单固定值一致；只有全部通过 ready 才为 true。
// 输入：环境变量 STREAM_STUDIO_PROXY_MANIFEST/RUNNER/PYTHON（可信的 runner 与 python 白名单）。
// 输出：{ manifestPath, deployment, paths, ready, reason, missing, issues }；清单不可用时 deployment=null。
// 说明：代理后端不调用原生引擎（native_engine_invoked 必须为 false），因此清单格式/路径检查偏严格。
function histreamProxyDeployment() {
  const configuredManifest = String(envSetting('STREAM_STUDIO_PROXY_MANIFEST', 'HISTREAM_STUDIO_PROXY_MANIFEST') || DEFAULT_HISTREAM_PROXY_MANIFEST).trim()
  if (!configuredManifest) {
    return { manifestPath: '', deployment: null,
      reason: 'histream-proxy is not configured; set STREAM_STUDIO_PROXY_MANIFEST, STREAM_STUDIO_PROXY_RUNNER and STREAM_STUDIO_PROXY_PYTHON before launching Stream Studio.' }
  }
  const manifestPath = resolve(normalizeHostPath(configuredManifest))
  if (!existsSync(manifestPath)) {
    return { manifestPath, deployment: null, reason: 'histream-proxy bundle is not installed; run the full-volume training --phase bundle command.' }
  }
  let deployment
  try { deployment = JSON.parse(readFileSync(manifestPath, 'utf8')) }
  catch (error) { return { manifestPath, deployment: null, reason: `Cannot read histream-proxy deployment manifest: ${error.message}` } }
  if (!deployment || typeof deployment !== 'object') return { manifestPath, deployment: null, reason: 'Invalid histream-proxy deployment manifest.' }
  if (deployment.format !== 'stream3d-histream-proxy-deployment-v1' || deployment.backend !== 'histream-proxy'
      || deployment.native_engine_invoked !== false) {
    return { manifestPath, deployment: null, reason: 'Unsupported or unsafe histream-proxy deployment manifest.' }
  }
  // 步骤 1：清单中所有模型/体数据/结果路径必须落在声明的 artifact_root 内，防止清单指向任意位置。
  const artifactRoot = resolve(normalizeHostPath(deployment.artifact_root || ''))
  const artifactPaths = [deployment.model_path, deployment.volume_schema_path,
    deployment.volume_join_audit_path, deployment.training_report_path, deployment.result_root,
    ...Object.values(deployment.volume_paths || {})]
  if (!artifactRoot || artifactPaths.some((item) => !item || !pathWithin(artifactRoot, normalizeHostPath(item)))) {
    return { manifestPath, deployment: null, reason: 'Proxy model/volume paths escape the declared artifact root.' }
  }
  const paths = {
    manifest: manifestPath,
    runner: resolve(normalizeHostPath(deployment.runner_path || '')),
    python: resolve(normalizeHostPath(deployment.python_executable || '')),
    capture: resolve(normalizeHostPath(deployment.capture_root || '')),
    model: resolve(normalizeHostPath(deployment.model_path || '')),
    artifactRoot,
    resultRoot: resolve(normalizeHostPath(deployment.result_root || '')),
  }
  // 步骤 2：清单里的 runner/python 必须与环境变量配置的受信任路径一致（防止清单替换可执行文件）。
  const trustedRunner = String(envSetting('STREAM_STUDIO_PROXY_RUNNER', 'HISTREAM_STUDIO_PROXY_RUNNER') || DEFAULT_HISTREAM_PROXY_RUNNER).trim()
  const trustedPython = String(envSetting('STREAM_STUDIO_PROXY_PYTHON', 'HISTREAM_STUDIO_PROXY_PYTHON') || DEFAULT_HISTREAM_PROXY_PYTHON).trim()
  const pathIssues = []
  const pathIdentity = (value) => {
    const normalized = resolve(normalizeHostPath(value)).replaceAll('\\', '/')
    return process.platform === 'win32' ? normalized.toLowerCase() : normalized
  }
  if (!trustedRunner) pathIssues.push('STREAM_STUDIO_PROXY_RUNNER is not configured.')
  else if (!sameResolvedPath(paths.runner, trustedRunner)) pathIssues.push('Manifest runner path is outside the configured histream-proxy runner allowlist.')
  if (!trustedPython) pathIssues.push('STREAM_STUDIO_PROXY_PYTHON is not configured.')
  else if (!sameResolvedPath(paths.python, trustedPython)) pathIssues.push('Manifest Python path is outside the configured histream-proxy Python allowlist.')
  // 步骤 3：固定要求 4 个热状态体数据（DOY173 的 12–15 时），并要求源工程输入文件的 SHA 清单与规范文件集合完全一致。
  const expectedStateKeys = ['DOY173_12-00', 'DOY173_13-00', 'DOY173_14-00', 'DOY173_15-00']
  if (JSON.stringify(Object.keys(deployment.volume_paths || {}).sort()) !== JSON.stringify(expectedStateKeys.slice().sort())
      || JSON.stringify(Object.keys(deployment.volume_sha256 || {}).sort()) !== JSON.stringify(expectedStateKeys.slice().sort())) {
    pathIssues.push('Deployment must pin exactly the four supported thermal-state volume files.')
  }
  const sourceRoot = resolve(normalizeHostPath(deployment.source_root || ''))
  const expectedSourcePaths = [
    join(sourceRoot, 'project.json'),
    join(sourceRoot, 'output', 'process', 'voxel_scene_links.bin'),
    join(sourceRoot, 'output', 'process', 'material_sensor_spectra.csv'),
    join(sourceRoot, 'meteorology', 'meteo_beijing_2019.txt'),
    join(sourceRoot, 'thermal_solver.log'),
  ]
  for (const key of expectedStateKeys) {
    const thermal = `voxelthermal_T=${key}.bin`
    const geometry = `voxelrt_T=${key}.bin`
    expectedSourcePaths.push(join(sourceRoot, 'output', 'process', thermal),
      join(sourceRoot, 'output', 'process', geometry), join(sourceRoot, 'output', 'process', geometry.replace(/\.bin$/i, '.json')))
  }
  const actualSourcePaths = Object.keys(deployment.required_source_inputs_sha256 || {})
  if (new Set(actualSourcePaths.map(pathIdentity)).size !== expectedSourcePaths.length
      || expectedSourcePaths.some((item) => !actualSourcePaths.some((candidate) => pathIdentity(candidate) === pathIdentity(item)))) {
    pathIssues.push('Deployment source-input SHA list must exactly cover all 17 canonical project/material/thermal/geometry/meteo files.')
  }
  // 步骤 4：校验抓取数据（capture）清单：源根一致、每个 episode 的 frames.jsonl 都在 capture 根内且被 SHA 清单覆盖。
  const expectedCapturePaths = [join(paths.capture, 'manifest.json')]
  try {
    const captureManifest = JSON.parse(readFileSync(expectedCapturePaths[0], 'utf8'))
    if (captureManifest.sourceRoot && pathIdentity(captureManifest.sourceRoot) !== pathIdentity(sourceRoot)) {
      pathIssues.push('Capture source root differs from the deployment source root.')
    }
    for (const episode of captureManifest.episodes || []) {
      if (!episode.path || episode.path.includes('..') || isAbsolute(normalizeHostPath(episode.path))) throw new Error('invalid episode path')
      const framePath = join(paths.capture, episode.path, 'frames.jsonl')
      if (!pathWithin(paths.capture, framePath)) throw new Error('episode path escapes capture root')
      expectedCapturePaths.push(framePath)
    }
    if (pathIdentity(deployment.capture_manifest_path || '') !== pathIdentity(expectedCapturePaths[0])) {
      pathIssues.push('Capture manifest path does not match the fixed capture root.')
    }
  } catch (error) { pathIssues.push(`Cannot validate capture hash list: ${error.message}`) }
  const actualCapturePaths = Object.keys(deployment.required_capture_inputs_sha256 || {})
  if (new Set(actualCapturePaths.map(pathIdentity)).size !== expectedCapturePaths.length
      || expectedCapturePaths.some((item) => !actualCapturePaths.some((candidate) => pathIdentity(candidate) === pathIdentity(item)))) {
    pathIssues.push('Deployment capture-input SHA list must exactly cover capture manifest and every episode frame file.')
  }
  // 步骤 5：解析符号链接后的真实路径也必须在 artifact_root 内；结果目录允许尚不存在。
  const containedArtifactFiles = [paths.model,
    resolve(normalizeHostPath(deployment.volume_schema_path || '')),
    resolve(normalizeHostPath(deployment.volume_join_audit_path || '')),
    resolve(normalizeHostPath(deployment.training_report_path || '')),
    ...Object.values(deployment.volume_paths || {}).map((item) => resolve(normalizeHostPath(item)))]
  if (containedArtifactFiles.some((item) => !realPathWithin(paths.artifactRoot, item))) {
    pathIssues.push('A model or dense volume resolves outside the artifact root.')
  }
  if (!realPathWithin(paths.artifactRoot, paths.resultRoot, true)) {
    pathIssues.push('Proxy result directory resolves outside the artifact root.')
  }
  // 步骤 6：检查所有必需文件存在，并逐个核对 SHA-256（缺少固定哈希本身也算问题）。
  const required = [paths.runner, paths.python, paths.capture, paths.model,
    resolve(normalizeHostPath(deployment.volume_schema_path || '')),
    resolve(normalizeHostPath(deployment.volume_join_audit_path || '')),
    resolve(normalizeHostPath(deployment.training_report_path || '')),
    ...Object.values(deployment.volume_paths || {}).map((item) => resolve(normalizeHostPath(item))),
    ...Object.keys(deployment.required_source_inputs_sha256 || {}).map((item) => resolve(normalizeHostPath(item))),
    ...Object.keys(deployment.required_capture_inputs_sha256 || {}).map((item) => resolve(normalizeHostPath(item)))]
  const missing = required.filter((item) => !existsSync(item))
  const hashIssues = []
  const hashChecks = [
    [paths.runner, deployment.runner_sha256, 'runner'],
    [paths.model, deployment.model_sha256, 'model'],
    [resolve(normalizeHostPath(deployment.volume_schema_path || '')), deployment.volume_schema_sha256, 'volume schema'],
    [resolve(normalizeHostPath(deployment.volume_join_audit_path || '')), deployment.volume_join_audit_sha256, 'volume join audit'],
    [resolve(normalizeHostPath(deployment.training_report_path || '')), deployment.training_report_sha256, 'training report'],
    ...Object.entries(deployment.volume_paths || {}).map(([key, path]) => [resolve(normalizeHostPath(path)), deployment.volume_sha256?.[key], `volume ${key}`]),
    ...Object.entries(deployment.required_source_inputs_sha256 || {}).map(([path, hash]) => [resolve(normalizeHostPath(path)), hash, `source ${basename(path)}`]),
    ...Object.entries(deployment.required_capture_inputs_sha256 || {}).map(([path, hash]) => [resolve(normalizeHostPath(path)), hash, `capture ${basename(path)}`]),
  ]
  for (const [path, expected, label] of hashChecks) {
    if (!expected) { hashIssues.push(`Missing pinned SHA256: ${label}`); continue }
    if (!existsSync(path)) continue
    try {
      if (sha256File(path) !== String(expected).toLowerCase()) hashIssues.push(`SHA256 mismatch: ${label}`)
    } catch (error) { hashIssues.push(`Cannot verify ${label}: ${error.message}`) }
  }
  const issues = [...pathIssues, ...hashIssues]
  const reason = missing.length ? `Missing proxy runtime inputs: ${missing.join(', ')}`
    : issues.length ? issues.join(' ') : null
  return { manifestPath, deployment, paths, ready: missing.length === 0 && issues.length === 0,
    reason, missing, issues }
}

// 作用：把部署检查结果整理成对前端展示的健康信息（路径转 Windows 形式）；includeFrames 且 ready 时附带支持的帧列表。
// nativeEngineInvoked 恒为 false：代理后端不运行原生 histream。
function histreamProxyHealth(includeFrames = false) {
  const active = histreamProxyDeployment()
  if (!active.deployment) return {
    backend: 'histream-proxy', ready: false, manifestPath: windowsPath(active.manifestPath),
    reason: active.reason, nativeEngineInvoked: false,
  }
  const d = active.deployment
  const info = {
    backend: 'histream-proxy', ready: active.ready, manifestPath: windowsPath(active.manifestPath),
    runnerPath: windowsPath(active.paths.runner), pythonExecutable: windowsPath(active.paths.python),
    captureRoot: windowsPath(active.paths.capture), artifactRoot: windowsPath(active.paths.artifactRoot),
    captureId: d.capture_id, captureScope: d.capture_scope, captureDate: d.capture_date,
    sourceProjectSha256: d.source_project_sha256, nativeEngineSha256: d.native_engine_sha256,
    shape: d.output_shape, bandNames: d.output_band_names, bandUnits: d.output_band_units,
    frameCount: d.supported_frames?.length || 0,
    portability: d.portability, reason: active.reason, nativeEngineInvoked: false,
  }
  if (includeFrames && active.ready) info.supportedFrames = d.supported_frames
  return info
}

// 向所有 SSE 客户端广播一条事件（data: <JSON>）；用于运行进度、日志、完成通知。
function emit(payload) {
  const line = `data: ${JSON.stringify(payload)}\n\n`
  for (const response of clients) response.write(line)
}

// 模块 5b：请求安全与请求体读取。仅接受环回地址的 Host/Origin。
const LOOPBACK_HOSTNAMES = new Set(['127.0.0.1', 'localhost', '[::1]'])
// Largest legitimate payload is a 64 MB DEM sent as base64 inside JSON.
const MAX_REQUEST_BYTES = 160 * 1024 * 1024
const LARGE_REQUEST_BYTES = 4 * 1024 * 1024

function isLoopbackHostname(value) {
  try { return LOOPBACK_HOSTNAMES.has(new URL(value).hostname) } catch { return false }
}

// 作用：判断请求是否来自外站/非本机，返回拒绝原因（空串表示放行）。
// 检查：Host 必须是环回地址；有 Origin 时必须是环回；非 GET/HEAD 必须是 application/json。
// The API can spawn executables and write files, so browsers on other sites
// (CSRF) or rebinding DNS names must never reach it. A JSON Content-Type forces
// a CORS preflight that this server never approves.
// DEFERRED[中] 安全: 只校验 Host/Origin/Content-Type，没有会话令牌。同机任何进程或其他 Windows 用户都能调用全部 API（启动引擎、写文件、读任意路径）；<img>/<script> 发起的 GET 不带 Origin 头，可以绕过 Origin 检查（例如用 GET /api/devices?refresh=1 反复拉起引擎）；Sec-Fetch-Site 只在个别接口判断，而且只拦 cross-site，同为 localhost 的其他端口属于 same-site 会放行；建议：每次启动生成随机令牌，经 preload 或启动参数注入页面，所有 /api 请求都校验自定义头；GET 统一拒绝 Sec-Fetch-Site 不是 same-origin/none 的请求。
function foreignRequestReason(request) {
  if (!isLoopbackHostname(`http://${request.headers.host || ''}`)) return 'Host 不是本机地址'
  const origin = request.headers.origin
  if (origin !== undefined && !isLoopbackHostname(origin)) return '拒绝跨站请求'
  if (request.method !== 'GET' && request.method !== 'HEAD'
      && !/^application\/json\b/i.test(request.headers['content-type'] || '')) return 'API 请求必须使用 application/json'
  return ''
}

// 作用：读取并解析 JSON 请求体（必须为对象），总量超过 MAX_REQUEST_BYTES 报错；
//       超过 LARGE_REQUEST_BYTES 的大请求在后台线程解析，避免阻塞事件循环。
async function body(request) {
  const chunks = []
  let size = 0
  for await (const chunk of request) {
    size += chunk.length
    if (size > MAX_REQUEST_BYTES) throw new Error(`请求体超过 ${MAX_REQUEST_BYTES / 1024 / 1024}MB`)
    chunks.push(chunk)
  }
  // 大请求体（导入 OBJ/DEM/纹理的 Base64 或文本内容）在后台线程解析，避免长时间 JSON.parse 阻塞。
  const data = size > LARGE_REQUEST_BYTES
    ? await heavyTasks.run('parse-json', { bytes: ownedBytes(Buffer.concat(chunks)) }, { transfer: true })
    : JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}')
  if (!data || typeof data !== 'object' || Array.isArray(data)) throw new Error('请求体须为 JSON 对象')
  return data
}

// 清理导入的 OBJ 文本：去掉 mtllib/usemtl 行，并统一换行、末尾只留一个换行（去掉的原因推测为材质改由工程波谱配置指定，待核对）。
function sanitizeObjContent(value) {
  const content = String(value || '')
    .split(/\r?\n/)
    .filter((line) => !/^\s*(mtllib|usemtl)\b/i.test(line))
    .join('\n')
    .trimEnd()
  return content ? content + '\n' : ''
}

// 作用：校验并规范化用户导入的波谱文件文本。格式：第 1 行波长，第 2 行反射率，可选第 3 行透射率；
//       以空白/逗号/分号分隔，# 之后为注释；缺省透射率为 0。
// 约束：波长 > 0 且不重复（按升序输出）；反射率、透射率在 [0,1]，且二者之和不超过 1（能量守恒）。
function normalizeSpectrumContent(value) {
  const rows = String(value || '').replace(/^\uFEFF/, '').split(/\r?\n/)
    .map((line) => line.replace(/#.*$/, '').trim()).filter(Boolean)
  if (rows.length < 2 || rows.length > 3) throw new Error('波谱文件必须为 2 行或 3 行：波长、反射率、可选透射率')
  const parseRow = (row, label) => {
    const values = row.split(/[\s,;]+/).filter(Boolean).map(Number)
    if (values.length < 2 || !values.every(Number.isFinite)) throw new Error(`${label}必须包含至少 2 个有效数值`)
    return values
  }
  const wavelengths = parseRow(rows[0], '第一行波长')
  const reflectance = parseRow(rows[1], '第二行反射率')
  const transmittance = rows[2] ? parseRow(rows[2], '第三行透射率') : wavelengths.map(() => 0)
  if (reflectance.length !== wavelengths.length || transmittance.length !== wavelengths.length) throw new Error('波长、反射率和透射率的数值数量必须相同')
  if (wavelengths.some((value) => value <= 0)) throw new Error('波长必须大于 0')
  const points = wavelengths.map((wavelength, index) => ({ wavelength, reflectance: reflectance[index], transmittance: transmittance[index] }))
    .sort((left, right) => left.wavelength - right.wavelength)
  if (points.some((point, index) => index > 0 && point.wavelength === points[index - 1].wavelength)) throw new Error('波长不能重复')
  if (points.some((point) => point.reflectance < 0 || point.transmittance < 0 || point.reflectance > 1 || point.transmittance > 1 || point.reflectance + point.transmittance > 1)) throw new Error('反射率、透射率必须在 0–1 内且二者之和不能超过 1')
  return [
    points.map((point) => point.wavelength).join(' '),
    points.map((point) => point.reflectance).join(' '),
    points.map((point) => point.transmittance).join(' ')
  ].join('\n') + '\n'
}

// 模块：宿主机路径换算。Linux 下把 Windows 盘符路径转成 /mnt/<盘符>/…；Windows 下把 WSL 路径转回盘符路径；
// 其余原样返回。注意函数内局部变量 windowsPath 遮蔽了同名全局函数（仅作正则匹配结果用）。
function normalizeHostPath(value) {
  const source = String(value || "").trim()
  const windowsPath = source.match(/^([A-Za-z]):(.*)$/)
  if (process.platform === "linux" && windowsPath) return "/mnt/" + windowsPath[1].toLowerCase() + "/" + windowsPath[2].replaceAll("\\", "/").replace(/^\/+/, '')
  const wslPath = source.match(/^\/mnt\/([A-Za-z])\/(.*)$/)
  if (process.platform === "win32" && wslPath) return wslPath[1].toUpperCase() + ":" + String.fromCharCode(92) + wslPath[2].replaceAll("/", String.fromCharCode(92))
  return source
}

// 把传给引擎的路径转为引擎可识别的形式：Linux 下通过 WSL 运行 .exe 引擎时，/mnt/x/… 要还原成 X:\…。
function histreamRuntimePath(value, enginePath = executable()) {
  const source = String(value || '').trim()
  if (process.platform !== 'linux' || extname(enginePath).toLowerCase() !== '.exe') return source
  const wslPath = source.match(/^\/mnt\/([A-Za-z])\/(.*)$/)
  return wslPath
    ? wslPath[1].toUpperCase() + ':\\' + wslPath[2].replaceAll('/', '\\')
    : source
}

// 把 WSL 风格路径（/mnt/x/…）转成 Windows 路径（X:\…）用于显示与回传前端；其他路径原样返回。
function windowsPath(value) {
  const source = String(value || '').trim()
  const wslPath = source.match(/^\/mnt\/([A-Za-z])\/(.*)$/)
  return wslPath
    ? wslPath[1].toUpperCase() + ':\\' + wslPath[2].replace(/^\/+/, '').replaceAll('/', '\\')
    : source
}

// 作用：决定工程文件中保存资源路径的形式。位于工程目录内的绝对路径存为相对路径（正斜杠，便于工程搬迁）；
//       其他绝对路径存为 Windows 绝对路径；本来就是相对路径的保持相对。
function projectStoredPath(value, baseDir) {
  const source = String(value || '').trim()
  if (!source) return ''
  const hostPath = normalizeHostPath(source)
  if (!isAbsolute(hostPath)) return hostPath.replaceAll('\\', '/')
  const absolutePath = resolve(hostPath)
  const local = relative(resolve(baseDir), absolutePath)
  if (local && local !== '..' && !local.startsWith('../') && !local.startsWith('..\\') && !isAbsolute(local))
    return local.replaceAll('\\', '/')
  return windowsPath(absolutePath)
}

// 对工程中所有会引用外部文件的字段（输出目录、气象、DEM、物体 OBJ/位置文件、波谱及其纹理）统一应用 projectStoredPath。
function normalizeStoredProjectPaths(project, baseDir) {
  const configuration = project?.configuration
  if (!configuration) return project
  if (configuration.outDir) configuration.outDir = projectStoredPath(configuration.outDir, baseDir)
  if (configuration.meteo?.path) configuration.meteo.path = projectStoredPath(configuration.meteo.path, baseDir)
  if (configuration.scene?.demFile) configuration.scene.demFile = projectStoredPath(configuration.scene.demFile, baseDir)
  for (const item of configuration.objects?.items || []) {
    if (item.fileName) item.fileName = projectStoredPath(item.fileName, baseDir)
    if (item.positionFile) item.positionFile = projectStoredPath(item.positionFile, baseDir)
  }
  for (const item of configuration.spectra || []) {
    if (item.fileName) item.fileName = projectStoredPath(item.fileName, baseDir)
    for (const texture of [item.physicalTexture, item.classTexture, item.opennessTexture]) {
      if (texture?.fileName) texture.fileName = projectStoredPath(texture.fileName, baseDir)
    }
  }
  return project
}

// 作用：对工程中所有“输入文件引用”字段原地应用 convert（如转成引擎可读路径）。
// 说明：不转换输出目录；内置气象标记（defined/meteo.txt 等）保持不变。
// Enumerate HiStream input references without changing output locations or built-in meteorology.
function mapHistreamInputPaths(configuration, convert) {
  if (!configuration) return
  const map = (object, key) => { if (object?.[key]) object[key] = convert(object[key]) }
  if (!['defined/meteo.txt', 'HiStream 内置气象数据'].includes(configuration.meteo?.path)) map(configuration.meteo, 'path')
  map(configuration.scene, 'demFile')
  map(configuration.atmosphere, 'lutFile')
  for (const item of configuration.objects?.items || []) {
    map(item, 'fileName')
    map(item, 'positionFile')
  }
  for (const item of configuration.spectra || []) {
    map(item, 'fileName')
    for (const texture of [item.physicalTexture, item.classTexture, item.opennessTexture]) map(texture, 'fileName')
  }
}

// 作用：把用户输入（工程目录或 project.json 路径，可带引号）解析为 project.json 的绝对路径，并做基本校验。
// 说明：已是统一格式（unified）的工程直接放行；旧格式需通过 validateProject。
function resolveProjectFile(value) {
  let path = normalizeHostPath(value).replace(/^"|"$/g, '')
  if (!path) throw new Error('请输入已有工程目录或 project.json 完整路径')
  path = resolve(path)
  if (!existsSync(path)) throw new Error(`找不到已有工程：${path}`)

  if (statSync(path).isDirectory()) {
    path = join(path, 'project.json')
    if (!existsSync(path)) throw new Error(`工程目录中找不到 project.json：${dirname(path)}`)
  }

  if (!statSync(path).isFile()) throw new Error(`工程入口不是文件：${path}`)
  if (basename(path).toLowerCase() !== 'project.json') throw new Error('工程入口必须是 project.json')
  let source
  try { source = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) }
  catch (error) { throw new Error(`project.json 格式错误：${error.message}`) }
  if (isUnifiedProject(source)) return path
  const validation = validateProject(source)
  if (!validation.valid) throw new Error(validation.errors.join('；'))
  return path
}

// 把对象或 JSON 文本规范化为工程对象（normalizeProject 补齐缺省值）；JSON 解析失败给出中文错误。
function projectFromSource(source) {
  if (source && typeof source === 'object') return normalizeProject(source)
  let parsed
  try { parsed = JSON.parse(String(source || '')) }
  catch (error) { throw new Error(`project.json 格式错误：${error.message}`) }
  return normalizeProject(parsed)
}

// 模块 7：面元规模预检。缓存 OBJ 的三角形数量与中位面积，避免每次预检都重读大文件。
const objTriangleCountCache = new Map()
const objMedianAreaCache = new Map()

// 作用：把工程里的资源引用解析为绝对路径。绝对路径原样；相对路径相对工程目录 baseDir。
// 说明：以 assets/ 开头的相对路径在工程目录下不存在时，回落到安装包共享资源库 RESOURCE_ROOT/assets。
function projectAssetPath(value, baseDir) {
  const hostPath = normalizeHostPath(value)
  if (!hostPath) return ''
  if (isAbsolute(hostPath)) return resolve(hostPath)
  const localPath = resolve(baseDir, hostPath)
  const sharedPath = /^assets[\\/]/i.test(hostPath) ? resolve(RESOURCE_ROOT, hostPath) : ''
  return sharedPath && !existsSync(localPath) && existsSync(sharedPath) ? sharedPath : localPath
}

// 作用：统计 OBJ 的三角形数。n 边形面按扇形剖分计 n-2 个三角形；按 (size, mtime) 缓存。
// 说明：只数 f 行，不解析顶点，因此对大文件也较快；文件不存在返回 0。
function objTriangleCount(path) {
  if (!path || !existsSync(path) || !statSync(path).isFile()) return 0
  const info = statSync(path)
  const cached = objTriangleCountCache.get(path)
  if (cached?.size === info.size && cached?.mtimeMs === info.mtimeMs) return cached.triangles
  const source = readFileSync(path, 'utf8')
  let triangles = 0
  for (const match of source.matchAll(/^[ \t]*f[ \t]+([^\r\n#]+)/gm)) {
    const vertices = match[1].trim().split(/[ \t]+/).filter(Boolean).length
    if (vertices >= 3) triangles += vertices - 2
  }
  objTriangleCountCache.set(path, { size: info.size, mtimeMs: info.mtimeMs, triangles })
  return triangles
}

// 作用：OBJ 三角形面积的中位数（模型坐标单位的平方，按 (size, mtime) 缓存）。
// 说明：支持负索引（相对当前顶点数）；多边形按以首顶点为中心的扇形剖分；叉积范数的一半为面积。
function objMedianTriangleArea(path) {
  if (!path || !existsSync(path) || !statSync(path).isFile()) return 0
  const info = statSync(path)
  const cached = objMedianAreaCache.get(path)
  if (cached?.size === info.size && cached?.mtimeMs === info.mtimeMs) return cached.area
  const vertices = [], areas = []
  for (const line of readFileSync(path, 'utf8').split(/\r?\n/)) {
    const parts = line.trim().split(/[ \t]+/)
    if (parts[0] === 'v') vertices.push(parts.slice(1, 4).map(Number))
    else if (parts[0] === 'f' && parts.length >= 4) {
      const index = parts.slice(1).map(token => {
        const value = parseInt(token, 10)
        return value < 0 ? vertices.length + value : value - 1
      })
      const a = vertices[index[0]]
      for (let k = 1; k + 1 < index.length; k++) {
        const b = vertices[index[k]], c = vertices[index[k + 1]]
        if (!a || !b || !c) continue
        const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], w = [c[0] - a[0], c[1] - a[1], c[2] - a[2]]
        areas.push(0.5 * Math.hypot(u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]))
      }
    }
  }
  areas.sort((x, y) => x - y)
  const area = areas.length ? areas[areas.length >> 1] : 0
  objMedianAreaCache.set(path, { size: info.size, mtimeMs: info.mtimeMs, area })
  return area
}

// 作用：实例位置文件中缩放列（第 4 列，行内以空白/逗号分隔）的中位数；# 与 // 开头为注释。
//       缺失或非正的缩放按 1；文件不存在返回 1。用于把 OBJ 面积换算为实例化后的面元面积。
function medianInstanceScale(path) {
  if (!path || !existsSync(path) || !statSync(path).isFile()) return 1
  const scales = []
  for (const line of readFileSync(path, 'utf8').split(/\r?\n/)) {
    const value = line.replace(/#.*/, '').trim()
    if (!value || value.startsWith('//')) continue
    const scale = Number(value.split(/[ \t,]+/)[3])
    scales.push(Number.isFinite(scale) && scale > 0 ? scale : 1)
  }
  scales.sort((x, y) => x - y)
  return scales.length ? scales[scales.length >> 1] : 1
}

// 作用：生成植被面元相对可见性栅格像元大小的提示文字（面元太小时直射项按邻域估计，精度下降）。
// 步骤 1：取所有植被对象“面积中位数 × 缩放²”的最小值作为代表面元面积 area（m²）。
// 步骤 2：栅格边长 raster：场景较大边 ≥ 1000 m 取 2048，否则 1024；像元 pixel = 包围球直径/raster。
// 步骤 3：ratio = 0.5·area/pixel²（系数 0.5 的含义待核对）；suggested 为 ratio 恰为 0.5 时
//         的方形场景边长（约 raster·√area/(1.001·√2)）。无植被或场景尺寸无效时返回空串。
// Sunlit and sky sampling use one raster over the scene's bounding sphere;
// facets whose projection is below ~0.5 pixel are estimated from neighbours.
function facetPixelRatioNote(project, baseDir) {
  const scene = project.configuration?.scene || {}
  const width = Number(scene.x) || 0, depth = Number(scene.y) || 0, height = Number(scene.height) || 0
  let area = Infinity
  for (const item of project.configuration?.objects?.items || []) {
    if (item.type !== 'Vegetation' || !item.fileName) continue
    const median = objMedianTriangleArea(projectAssetPath(item.fileName, baseDir)) *
      medianInstanceScale(projectAssetPath(item.positionFile, baseDir)) ** 2
    if (median > 0) area = Math.min(area, median)
  }
  if (!Number.isFinite(area) || !(width > 0 && depth > 0)) return ''
  const raster = Math.max(width, depth) >= 1000 ? 2048 : 1024
  const pixel = Math.hypot(width, depth, height) * 1.001 / raster
  const ratio = 0.5 * area / (pixel * pixel)
  const suggested = raster * Math.sqrt(area) / (1.001 * Math.SQRT2)
  return `植被面元/像元面积比 ${ratio.toFixed(2)}（面元中位面积 ${area.toPrecision(3)} m²，像元 ${pixel.toPrecision(3)} m）；` +
    `比值 ≥ 0.5 时叶温误差约 < 0.1 K，对应场景约 ≤ ${Math.round(suggested)} m，更大场景的植被直射按邻域估计`
}

// 统计实例位置文件的有效行数（跳过空行与 # // 注释）；没有位置文件按 1 个实例。
function positionInstanceCount(path) {
  if (!path || !existsSync(path) || !statSync(path).isFile()) return 1
  let count = 0
  for (const line of readFileSync(path, 'utf8').split(/\r?\n/)) {
    const value = line.replace(/#.*/, '').trim()
    if (value && !value.startsWith('//')) count += 1
  }
  return count
}

// 作用：估计 FacetRT/FacetEB 展开后的总三角面数 = 地面（初值 2）+ Σ(OBJ 三角形数 × 实例数)。
// 说明：Fire/Fog/介质类对象不参与面元；没有 fileName 的对象按 12 个三角面计（推测为内置长方体）。
// 输出：{ total, details }，details 按展开面数降序，用于报错时指出最大来源。
function estimateFacetTriangles(project, baseDir) {
  const details = []
  let total = 2
  for (const item of project.configuration?.objects?.items || []) {
    if (item.type === 'Fire' || item.type === 'Fog' || item.medium) continue
    const objectPath = projectAssetPath(item.fileName, baseDir)
    const triangles = item.fileName ? objTriangleCount(objectPath) : 12
    if (!triangles) continue
    const positionPath = projectAssetPath(item.positionFile, baseDir)
    const instances = item.positionFile ? positionInstanceCount(positionPath) : 1
    const expandedTriangles = triangles * instances
    total += expandedTriangles
    details.push({
      name: String(item.name || basename(objectPath, extname(objectPath)) || '未命名对象'),
      triangles,
      instances,
      expandedTriangles
    })
  }
  details.sort((left, right) => right.expandedTriangles - left.expandedTriangles)
  return { total, details }
}

// 作用：面元总数超过 MAX_FACET_TRIANGLES 时抛错，并按模式建议改用对应的体元模式。
function assertFacetScale(project, baseDir, mode) {
  const estimate = estimateFacetTriangles(project, baseDir)
  if (estimate.total <= MAX_FACET_TRIANGLES) return estimate
  const largest = estimate.details[0]
  const source = largest
    ? `最大来源：${largest.name}（${largest.triangles.toLocaleString('zh-CN')} 面 × ${largest.instances.toLocaleString('zh-CN')} 实例 = ${largest.expandedTriangles.toLocaleString('zh-CN')} 面）。`
    : ''
  const recommendation = mode === 'eFacetEB'
    ? '请切换到“体元辐射传输与能量平衡（Voxel RT–EB）”'
    : '请切换到“体元辐射传输（VoxelRT）”'
  throw new Error(
    `面元规模预估为 ${estimate.total.toLocaleString('zh-CN')} 个三角面，超过安全上限 ${MAX_FACET_TRIANGLES.toLocaleString('zh-CN')}。` +
    `${source}${recommendation}，或减少实例数量、简化 OBJ 后再运行。`)
}

// 大气查找表缓存（按 (size, mtime) 失效）。
const atmosphereLutCache = new Map()

// 模块：运行前预检（runPreflight）及耗时标签。
// Run preflight: scale, memory and duration estimates shown before a run.
// Durations come from earlier completed runs of the same mode in this
// project's output/runs (seconds per node or per observation, median of the
// latest three), so they reflect this scene on this machine.
function durationLabel(seconds) {
  const value = Math.max(0, Math.round(Number(seconds) || 0))
  return value < 90 ? `${value} 秒` : value < 5400 ? `${Math.round(value / 60)} 分钟` : `${(value / 3600).toFixed(1)} 小时`
}

// 作用：运行前估算规模、显存与耗时，并生成警告（不阻止运行，面元超限除外由 assertFacetScale 拦截）。
// 输出：{ mode, nodes, angles, bands, timeout*, facet|voxel, estimateSeconds, estimateBasis, warnings }。
// 步骤 1：确定节点数（能量平衡模式按 meteo.start/end，同引擎规则）、视角数、波段数和超时。
// 步骤 2：面元模式估算三角面数与可见性栅格；体元模式估算网格与活动体元的辐射场显存。
// 步骤 3：用同模式历史运行的 run-manifest 取最近 3 次的单位耗时中位数，外推本次耗时并与超时比较。
function runPreflight(input, project, mode) {
  const baseDir = dirname(input)
  const c = project.configuration
  const warnings = []
  const energy = isEnergyBalanceMode(mode)
  const meteoStart = Math.max(0, Math.round(Number(c.meteo?.start) || 0))
  const meteoEnd = Math.max(meteoStart + 1, Math.round(Number(c.meteo?.end) || 0))
  const nodes = energy ? meteoEnd - meteoStart : 1
  const perspective = ['perspective', 'central', 'center'].includes(String(c.sensor?.projection))
  const angles = perspective ? 1 : Math.max(1, sensorViewAngles(c.sensor, c.light?.azimuth).length)
  const bands = Math.max(1, sensorBandValues(c.sensor).length)
  const timeoutMs = projectRunTimeoutMs(project)
  const result = { mode, nodes, angles, bands, timeoutSeconds: Math.round(timeoutMs / 1000),
    timeoutSource: normalizeRunTimeoutMinutes(c.control?.runTimeoutMinutes) ? 'project' : 'server',
    serverTimeoutSeconds: Math.round(RUN_TIMEOUT_MS / 1000), warnings }
  if (mode === 'eFacetRT' || mode === 'eFacetEB') {
    const estimate = estimateFacetTriangles(project, baseDir)
    // 可见性栅格边长：大场景 2048，否则 1024（与 facetPixelRatioNote 保持一致）。aBufferMiB 为 A-buffer 显存粗估
    // （raster² × 64 层 × 8 字节，各系数含义待核对）。
    const raster = Math.max(Number(c.scene?.x) || 0, Number(c.scene?.y) || 0) >= 1000 ? 2048 : 1024
    result.facet = { triangles: estimate.total, limit: MAX_FACET_TRIANGLES, raster,
      largest: estimate.details[0] || null,
      aBufferMiB: Math.round(raster * raster * 64 * 8 / 1048576) }
    if (estimate.total > MAX_FACET_TRIANGLES) warnings.push(`面元约 ${estimate.total.toLocaleString('zh-CN')} 个，超过上限 ${MAX_FACET_TRIANGLES.toLocaleString('zh-CN')}，运行会被拒绝；请改用体元模式或简化模型`)
    else if (estimate.total > MAX_FACET_TRIANGLES * 0.6) warnings.push(`面元约 ${estimate.total.toLocaleString('zh-CN')} 个，接近上限，可见性图与显存开销较大`)
    const periodic = Math.round(Number(c.control?.periodicTraversalCount) || 0)
    // 周期遍历次数向上取整到引擎支持的瓦片档位 4/8/12/20（档位定义以引擎为准，待核对）。
    if (periodic > 0) result.facet.periodicTiles = periodic <= 4 ? 4 : periodic <= 8 ? 8 : periodic <= 12 ? 12 : 20
  } else if (mode === 'eVoxelRT' || mode === 'eVoxelEB') {
    // 体元模式：网格 = ceil(场景尺寸/体元边长)；活动体元按网格 5 % 作上界（表面场景通常 1–5 %）。
    // 每体元字节数：VoxelRT = (64 + 2·波段数)·8，VoxelEB = 64·12（系数来自引擎的辐射场布局，待核对）。
    const voxel = Math.max(0.01, Number(c.scene?.voxel) || 1)
    const grid = [Math.ceil((Number(c.scene?.x) || 0) / voxel), Math.ceil((Number(c.scene?.y) || 0) / voxel),
      Math.ceil((Number(c.scene?.height) || 0) / voxel)]
    const cells = grid[0] * grid[1] * grid[2]
    // Active voxels are typically 1-5 % of the grid for surface scenes; the
    // bound below uses 5 % and the per-voxel radiance field of each engine.
    const activeBound = Math.ceil(cells * 0.05)
    const perVoxelBytes = mode === 'eVoxelRT' ? (64 + 2 * bands) * 8 : 64 * 12
    result.voxel = { size: voxel, grid, cells, activeBound, radianceMiB: Math.round(activeBound * perVoxelBytes / 1048576) }
    if (cells > 400e6) warnings.push(`体元网格 ${grid.join('×')}（${(cells / 1e6).toFixed(0)} M 格）过大，建议增大体元尺寸`)
    if (result.voxel.radianceMiB > 16384) warnings.push(`体元辐射场估计可达 ${(result.voxel.radianceMiB / 1024).toFixed(1)} GB 显存`)
  }
  // 步骤 3：读取历史 run-manifest；只用同模式、已完成（或未收敛但保留结果）的运行，
  // 单位 = 能量平衡按节点、辐射传输按观测视角数。
  // History-based duration estimate.
  const root = resolve(baseDir, normalizeHostPath(c.outDir || 'output'), 'runs')
  const samples = []
  if (existsSync(root)) {
    for (const entry of readdirSync(root, { withFileTypes: true })) {
      if (!entry.isDirectory()) continue
      try {
        const manifest = JSON.parse(readFileSync(join(root, entry.name, 'run-manifest.json'), 'utf8'))
        if (manifest.mode !== mode || !['completed', 'non_converged'].includes(manifest.status) || !manifest.finishedAt) continue
        const seconds = (Date.parse(manifest.finishedAt) - Date.parse(manifest.startedAt)) / 1000
        const units = energy ? Math.max(1, manifest.completed?.nodeCount || manifest.requested?.nodeCount || 1)
          : Math.max(1, manifest.requested?.viewAngles?.length || 1)
        if (seconds > 0) samples.push({ at: manifest.finishedAt, perUnit: seconds / units })
      } catch {}
    }
  }
  samples.sort((a, b) => String(b.at).localeCompare(String(a.at)))
  const recent = samples.slice(0, 3).map(sample => sample.perUnit).sort((a, b) => a - b)
  if (recent.length) {
    const perUnit = recent[recent.length >> 1]
    result.estimateSeconds = Math.round(perUnit * (energy ? nodes : angles))
    result.estimateBasis = `${recent.length} 次同模式历史运行，约 ${perUnit.toFixed(1)} s/${energy ? '节点' : '观测'}`
    if (result.estimateSeconds > timeoutMs / 1000) {
      // 建议上限 = 预计耗时 × 1.5，向上取整到分钟（不超过工程允许的 1440 分钟）。
      result.suggestedTimeoutMinutes = Math.min(1440, Math.max(1, Math.ceil(result.estimateSeconds * 1.5 / 60)))
      warnings.push(`预计约 ${durationLabel(result.estimateSeconds)}，超过单次运行上限 ${runTimeoutLabel(timeoutMs)}，将被中途终止；请减少节点/观测数，或在仿真控制中把“单次运行上限”调到 ${result.suggestedTimeoutMinutes} 分钟`)
    } else if (result.estimateSeconds > 0.8 * timeoutMs / 1000) warnings.push(`预计约 ${durationLabel(result.estimateSeconds)}，接近单次运行上限 ${runTimeoutLabel(timeoutMs)}`)
  } else if (energy && nodes > 24) {
    warnings.push(`本次 ${nodes} 个时间节点且无历史耗时可参考；单次运行上限为 ${runTimeoutLabel(timeoutMs)}，建议先用少量节点试算`)
  }
  return result
}

// 作用：读取大气查找表 CSV 并按 (模式, 气溶胶, 水汽, 能见度, 海拔, 天顶角) 组织为光谱曲线。
// 格式：v2 以 atmosphere_model 表头开头，列为 模式,气溶胶,水汽,能见度,海拔,天顶角,波长,透过率,程辐射；
//       v1 无前两列和水汽列，按 midlatitude-summer / rural / 水汽=2 补齐。
// 说明：透过率限幅到 [0,1]，程辐射限幅为非负；列数不足或含非数值的行被跳过；单位待核对。
// 输出：{ spectra, waterVapor[], visibility[], altitude[], zenith[] }（各轴升序），按 (size, mtime) 缓存。
function loadAtmosphereLut(path) {
  const normalized = resolve(normalizeHostPath(path))
  if (!existsSync(normalized)) throw new Error(`找不到大气查找表：${normalized}`)
  const stat = statSync(normalized)
  const cached = atmosphereLutCache.get(normalized)
  if (cached && cached.size === stat.size && cached.mtimeMs === stat.mtimeMs) return cached.table
  const spectra = new Map(), waterVapor = new Set(), visibility = new Set(), altitude = new Set(), zenith = new Set()
  let version2 = false
  for (const line of readFileSync(normalized, 'utf8').split(/\r?\n/)) {
    if (!line || line.startsWith('#')) continue
    if (line.startsWith('atmosphere_model')) { version2 = true; continue }
    if (line.startsWith('visibility_km')) continue
    const cells = line.split(',')
    const model = version2 ? cells[0] : 'midlatitude-summer'
    const aerosol = version2 ? cells[1] : 'rural'
    const values = (version2 ? cells.slice(2) : cells).map(Number)
    if (values.length < (version2 ? 7 : 6) || !values.every(Number.isFinite)) continue
    const [water, v, a, z, wavelength, transmittance, pathRadiance] = version2
      ? values : [2, ...values]
    waterVapor.add(water); visibility.add(v); altitude.add(a); zenith.add(z)
    const key = `${model}|${aerosol}|${water}|${v}|${a}|${z}`
    const spectrum = spectra.get(key) || { wavelength: [], transmittance: [], pathRadiance: [] }
    spectrum.wavelength.push(wavelength)
    spectrum.transmittance.push(Math.max(0, Math.min(1, transmittance)))
    spectrum.pathRadiance.push(Math.max(0, pathRadiance))
    spectra.set(key, spectrum)
  }
  const table = {
    spectra,
    waterVapor: [...waterVapor].sort((a, b) => a - b),
    visibility: [...visibility].sort((a, b) => a - b),
    altitude: [...altitude].sort((a, b) => a - b),
    zenith: [...zenith].sort((a, b) => a - b)
  }
  if (!spectra.size || !table.visibility.length) throw new Error(`大气查找表为空：${normalized}`)
  atmosphereLutCache.set(normalized, { size: stat.size, mtimeMs: stat.mtimeMs, table })
  return table
}

// 在升序轴 axis 上找 value 的上下相邻节点；超出范围时两端取同一节点（不外推）。
function atmosphereAxisBracket(axis, value) {
  if (value <= axis[0]) return [axis[0], axis[0]]
  if (value >= axis.at(-1)) return [axis.at(-1), axis.at(-1)]
  const right = axis.findIndex((item) => item >= value)
  return [axis[right - 1], axis[right]]
}

// 线性插值权重：upper=true 取上节点权重，否则取下节点；low==high 时全部权重给下节点。
function atmosphereAxisWeight(value, low, high, upper) {
  if (high <= low) return upper ? 0 : 1
  const fraction = Math.max(0, Math.min(1, (value - low) / (high - low)))
  return upper ? fraction : 1 - fraction
}

// 沿波长方向线性插值 [透过率, 程辐射]；超出光谱范围取端点值；用二分查找定位区间。
function atmosphereSpectralSample(spectrum, wavelength) {
  const wavelengths = spectrum.wavelength
  if (wavelength <= wavelengths[0]) return [spectrum.transmittance[0], spectrum.pathRadiance[0]]
  if (wavelength >= wavelengths.at(-1)) return [spectrum.transmittance.at(-1), spectrum.pathRadiance.at(-1)]
  let low = 0, high = wavelengths.length - 1
  while (high - low > 1) {
    const middle = (low + high) >> 1
    if (wavelengths[middle] < wavelength) low = middle
    else high = middle
  }
  const weight = (wavelength - wavelengths[low]) / (wavelengths[high] - wavelengths[low])
  return [
    spectrum.transmittance[low] + (spectrum.transmittance[high] - spectrum.transmittance[low]) * weight,
    spectrum.pathRadiance[low] + (spectrum.pathRadiance[high] - spectrum.pathRadiance[low]) * weight
  ]
}

// 作用：在查找表中对 (水汽, 能见度, 海拔, |天顶角|) 做四线性插值（16 个角点）再沿波长插值。
// 说明：缺失的角点被跳过，结果按已累计的权重归一；波长在 350–14000 nm 之外或全部缺失时视为大气透明
//       （透过率 1、程辐射 0）。
function sampleAtmosphereLut(table, model, aerosol, waterVapor, visibility, altitude, zenith, wavelength) {
  if (wavelength < 350 || wavelength > 14000) return { transmittance: 1, pathRadiance: 0 }
  const wb = atmosphereAxisBracket(table.waterVapor, waterVapor)
  const vb = atmosphereAxisBracket(table.visibility, visibility)
  const ab = atmosphereAxisBracket(table.altitude, altitude)
  const zb = atmosphereAxisBracket(table.zenith, Math.abs(zenith))
  let transmittance = 0, pathRadiance = 0, accumulatedWeight = 0
  for (let iw = 0; iw < 2; iw += 1) for (let iv = 0; iv < 2; iv += 1) for (let ia = 0; ia < 2; ia += 1) for (let iz = 0; iz < 2; iz += 1) {
    const w = wb[iw]
    const v = vb[iv], a = ab[ia], z = zb[iz]
    const weight = atmosphereAxisWeight(waterVapor, wb[0], wb[1], Boolean(iw))
      * atmosphereAxisWeight(visibility, vb[0], vb[1], Boolean(iv))
      * atmosphereAxisWeight(altitude, ab[0], ab[1], Boolean(ia))
      * atmosphereAxisWeight(Math.abs(zenith), zb[0], zb[1], Boolean(iz))
    if (!weight) continue
    const spectrum = table.spectra.get(`${model}|${aerosol}|${w}|${v}|${a}|${z}`)
    if (!spectrum) continue
    const sample = atmosphereSpectralSample(spectrum, wavelength)
    transmittance += weight * sample[0]
    pathRadiance += weight * sample[1]
    accumulatedWeight += weight
  }
  if (!accumulatedWeight) return { transmittance: 1, pathRadiance: 0 }
  return {
    transmittance: Math.max(0, Math.min(1, transmittance / accumulatedWeight)),
    pathRadiance: Math.max(0, pathRadiance / accumulatedWeight)
  }
}

// 普朗克黑体光谱辐亮度 B(λ,T) = 2hc²/λ⁵ / (exp(c2/(λT)) − 1)，λ 以 µm 计，输出 W m-2 sr-1 µm-1。
// 常数：2hc² = 1.19104e8，c2 = 14387.7 µm·K。输入波长 > 50 视为 nm，否则视为 µm（兼容两种单位）。
function planckRadiance(wavelengthNm, temperatureK) {
  const wavelengthUm = wavelengthNm > 50 ? wavelengthNm / 1000 : wavelengthNm
  return 1.19104e8 / (Math.pow(wavelengthUm, 5) * Math.expm1(14387.7 / (temperatureK * wavelengthUm)))
}

// planckRadiance 的反函数：由光谱辐亮度求亮温（K），单位约定同上。
function inversePlanckTemperature(wavelengthNm, radiance) {
  const wavelengthUm = wavelengthNm > 50 ? wavelengthNm / 1000 : wavelengthNm
  return 14387.7 / (wavelengthUm * Math.log(1.19104e8 / (radiance * Math.pow(wavelengthUm, 5)) + 1))
}


// 模块 8：输出文件命名与过程输出开关。几何后缀：面元模式 _f，体元模式 _v，其余无。
function outputGeometrySuffix(mode) {
  if (mode === 'eFacetRT' || mode === 'eFacetEB') return '_f'
  if (mode === 'eVoxelRT' || mode === 'eVoxelEB') return '_v'
  return ''
}

// 是否为能量平衡模式（FacetEB/VoxelEB）；这类模式按气象时间节点输出。
function isEnergyBalanceMode(mode) {
  return mode === "eFacetEB" || mode === "eVoxelEB"
}

// 作用：确定是否输出“辐射过程量/能量过程量”。新字段 radiationProcess/energyProcess 为空时，
//       回退到旧工程的单一开关 sensor.process：非 EB 模式作辐射过程，EB 模式作能量过程（兼容旧工程）。
function processOutputSelection(projectSource, mode) {
  const sensor = projectFromSource(projectSource).configuration.sensor
  const legacy = Boolean(sensor.process)
  const radiation = sensor.radiationProcess == null
    ? (!isEnergyBalanceMode(mode) && legacy) : Boolean(sensor.radiationProcess)
  const energy = sensor.energyProcess == null
    ? (isEnergyBalanceMode(mode) && legacy) : Boolean(sensor.energyProcess)
  return { radiation, energy }
}

// 该模式下是否需要任何过程输出：EB 模式为辐射或能量任一；其余仅看辐射过程。
function processOutputEnabled(projectSource, mode) {
  const selected = processOutputSelection(projectSource, mode)
  return isEnergyBalanceMode(mode) ? selected.radiation || selected.energy : selected.radiation
}

// 文件名中的角度标记：保留 2 位小数（度）；非有限数写 0.00。
function outputAngleToken(value) {
  const number = Number(value)
  return Number.isFinite(number) ? number.toFixed(2) : '0.00'
}

// 把儒略日（年内日 + 日小数）转成文件名时间标记 DOY<d>_HH-MM；分钟满 1440 时进位到下一天。
function outputTimeToken(julianTime) {
  const value = Number(julianTime)
  if (!Number.isFinite(value)) return ''
  let day = Math.floor(value)
  let minutes = Math.round((value - day) * 1440)
  if (minutes >= 1440) { day += Math.floor(minutes / 1440); minutes %= 1440 }
  if (minutes < 0) minutes = 0
  const hour = String(Math.floor(minutes / 60)).padStart(2, '0')
  const minute = String(minutes % 60).padStart(2, '0')
  return 'DOY' + day + '_' + hour + '-' + minute
}

// 作用：生成结果 TIFF 文件名。EB 模式：T=<时间>_VZA=_VAA=；RT 模式：SZA=_SAA=_VZA=_VAA=；
//       再加几何后缀（_f/_v）与大气标记（_a）。需与引擎输出命名及 tiffObservation 的解析保持一致。
function outputTiffName(mode, values = {}) {
  const suffix = outputGeometrySuffix(mode) + (values.atmosphere ? '_a' : '')
  const vza = outputAngleToken(values.vza)
  const vaa = outputAngleToken(values.vaa)
  if (isEnergyBalanceMode(mode)) {
    const time = String(values.timeToken || outputTimeToken(values.julianTime) || 'UNKNOWN')
    return 'T=' + time + '_VZA=' + vza + '_VAA=' + vaa + suffix + '.tif'
  }
  return 'SZA=' + outputAngleToken(values.sza) + '_SAA=' + outputAngleToken(values.saa) +
    '_VZA=' + vza + '_VAA=' + vaa + suffix + '.tif'
}

// 模块：大文件读取与辐射结果（面元/体元 JSON）解析。
// readFileSync 对超过 2 GiB 的文件会抛 ERR_FS_FILE_TOO_LARGE（与 Buffer 上限无关）。
// 面元结果和三维过程数据可能达到数 GB，因此分块读入自行分配的 Buffer。
function readLargeFileSync(path) {
  const { size } = statSync(path)
  if (!(size > 0)) return Buffer.alloc(0)
  if (size > MAX_RESULT_BYTES) {
    const label = (bytes) => (bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB`
      : bytes >= 1024 ** 2 ? `${(bytes / 1024 ** 2).toFixed(0)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`)
    throw new Error(`结果文件 ${label(size)} 超过上限 ${label(MAX_RESULT_BYTES)}，请减少面元数量或简化 OBJ`)
  }
  const buffer = Buffer.allocUnsafe(size)
  const descriptor = openSync(path, 'r')
  let offset = 0
  try {
    while (offset < size) {
      const read = readSync(descriptor, buffer, offset, Math.min(size - offset, 1 << 30), offset)
      if (read <= 0) break
      offset += read
    }
  } finally {
    closeSync(descriptor)
  }
  return offset === size ? buffer : buffer.subarray(0, offset)
}

// 作用：读取 FacetRT/FacetEB 引擎写出的面元结果 JSON（可达数 GB），返回类型化数组形式的完整结果。
// 输入：jsonPath；geometryOnly=true 时只读几何（vertexPositions）和 wavelengths，用于逐步导出的几何文件。
// 输出：{ facetCount, metrics:[[id,name,Float32Array]], vertexPositions(facetCount×9), backend, iterations,
//       maxDelta, wavelengths, bandRadiosity, bandStates, quantity, units }。
// 说明：不把整个文件转成 JS 字符串（受 V8 字符串长度上限限制），而是在 Buffer 上手工扫描键并逐个数字解析。
//       每面元有正反两个面（surfaceCount = 2·facetCount），指标数组按 (面元, 面) 交错。
function readRadiosityResult(jsonPath, geometryOnly = false) {
  if (!existsSync(jsonPath)) throw new Error(`找不到面元结果：${jsonPath}`)
  // 大场景/多波段面元结果 JSON 会超过 512 MB。若用 readFileSync(..., 'utf8') + JSON.parse，
  // 会触发 V8 单字符串长度上限（0x1fffffe8 字符）：
  //   Cannot create a string longer than 0x1fffffe8 characters
  // 这里改为直接基于 Buffer 逐数字解析（与 sampleRadiosityResult 同一思路），
  // 不把整个文件转换成 JS 字符串，因此不再受该上限限制。
  // readFileSync 对超过 2 GiB 的文件会抛 ERR_FS_FILE_TOO_LARGE，而面元结果可达数 GB
  // （约 750 万面元 ≈ 3.3 GB），因此统一用 readLargeFileSync 分块读入自行分配的 Buffer。
  const source = readLargeFileSync(jsonPath)
  const header = source.subarray(0, Math.min(source.length, 1 << 16)).toString('utf8')
  const facetCount = Number(header.match(/"facetCount"\s*:\s*(\d+)/)?.[1])
  if (!Number.isInteger(facetCount) || facetCount < 1) throw new Error('面元结果缺少有效的 facetCount')
  const quantity = String(header.match(/"quantity"\s*:\s*"([^"]*)"/)?.[1] || '')
  const units = String(header.match(/"units"\s*:\s*"([^"]*)"/)?.[1] || '')
  const backend = String(header.match(/"backend"\s*:\s*"([^"]*)"/)?.[1] || '')
  const surfaceCount = facetCount * 2

  // 读取顶层标量（如 iterations、maxDelta）：找到键后读到下一个分隔符（, 换行 }）为止；缺失返回 undefined。
  const readScalar = (key) => {
    const keyOffset = source.indexOf('"' + key + '"')
    if (keyOffset < 0) return undefined
    const colon = source.indexOf(58, keyOffset)
    if (colon < 0) return undefined
    let end = colon + 1
    while (end < source.length && ![44, 10, 13, 125].includes(source[end])) end += 1
    const value = Number(source.toString('ascii', colon + 1, end).trim())
    return Number.isFinite(value) ? value : undefined
  }
  const iterations = readScalar('iterations')
  const maxDelta = readScalar('maxDelta')

  // 顶层数值数组按固定顺序写入；用递增的 searchFrom 避免对超大文件反复全量扫描。
  // findArray/readFixed/readDynamic：按引擎写出顺序依次定位数组；readFixed 要求元素个数恰为 count（Float32 存储），
  // readDynamic 读到 ] 为止（用于元素个数不定的 wavelengths）。
  let searchFrom = 0
  const findArray = (key) => {
    const keyOffset = source.indexOf('"' + key + '"', searchFrom)
    if (keyOffset < 0) return -1
    const arrayOffset = source.indexOf(91, keyOffset)
    if (arrayOffset < 0) throw new Error('面元结果缺少 ' + key + ' 数组')
    searchFrom = keyOffset + key.length + 2
    return arrayOffset
  }
  const readFixed = (key, count) => {
    const arrayOffset = findArray(key)
    if (arrayOffset < 0) return null
    const values = new Float32Array(count)
    let position = arrayOffset + 1
    for (let index = 0; index < count; index += 1) {
      while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13 || source[position] === 44)) position += 1
      if (position >= source.length) throw new Error(key + ' 数据不完整')
      const numberStart = position
      while (position < source.length && source[position] !== 44 && source[position] !== 93) position += 1
      if (position === numberStart) throw new Error(key + ' 数据不完整')
      const value = Number(source.toString('ascii', numberStart, position))
      if (!Number.isFinite(value)) throw new Error(key + ' 包含无效数值')
      values[index] = value
    }
    while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13)) position += 1
    if (position >= source.length || source[position] !== 93) throw new Error(key + ' 数据不完整')
    return values
  }
  const readDynamic = (key) => {
    const arrayOffset = findArray(key)
    if (arrayOffset < 0) return null
    const values = []
    let position = arrayOffset + 1
    while (true) {
      while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13 || source[position] === 44)) position += 1
      if (position >= source.length || source[position] === 93) break
      const numberStart = position
      while (position < source.length && source[position] !== 44 && source[position] !== 93) position += 1
      const value = Number(source.toString('ascii', numberStart, position))
      if (!Number.isFinite(value)) throw new Error(key + ' 包含无效数值')
      values.push(value)
    }
    return values
  }

  // 步骤 1：先读三角形顶点坐标（必须有）；geometryOnly 时顺带读写在其后的 wavelengths 就返回。
  // 步骤 2：按文件内写入顺序依次读各指标数组（顺序错误会因 searchFrom 单向前进而找不到）。
  const arrays = {}
  arrays.vertexPositions = readFixed('vertexPositions', facetCount * 9)
  if (!arrays.vertexPositions) throw new Error('面元结果缺少完整的三角形坐标')
  if (geometryOnly) {
    // The FacetEB step-sync geometry file may also carry the engine's actual
    // band list ("wavelengths", written after vertexPositions); use it when present.
    const geometryWavelengths = (readDynamic('wavelengths') || []).filter((value) => Number.isFinite(value) && value > 0)
    return { facetCount, vertexPositions: arrays.vertexPositions, backend, metrics: [], bandRadiosity: new Float32Array(0), wavelengths: geometryWavelengths }
  }
  arrays.wavelengths = readDynamic('wavelengths')
  // 文件内的写入顺序：sunlit, radiosity, lightEnhancement, temperature, ...
  const metricKeys = ['sunlit', 'radiosity', 'lightEnhancement', 'temperature', 'netRadiation', 'sensibleHeat', 'latentHeat', 'storageHeat', 'photovoltaicPower']
  for (const key of metricKeys) arrays[key] = readFixed(key, surfaceCount)
  arrays.bandRadiosity = readFixed('bandRadiosity', surfaceCount * Math.max(1, (arrays.wavelengths || []).length))
  // Per band and facet: front lit, front shaded, back lit, back shaded.
  arrays.bandStates = readFixed('bandStates', facetCount * 4 * Math.max(1, (arrays.wavelengths || []).length))

  const wavelengths = (arrays.wavelengths || []).filter((value) => Number.isFinite(value) && value > 0)
  const bandRadiosity = arrays.bandRadiosity || new Float32Array(0)
  if (bandRadiosity.length && bandRadiosity.length < surfaceCount * Math.max(1, wavelengths.length)) {
    throw new Error('面元多波段辐亮度数据不完整')
  }

  // 步骤 3：整理指标清单 [id, 显示名, 是否必需]。quantity 为 spectral radiance 时 radiosity 的含义是光谱辐亮度。
  // 必需项缺失即报错；能量平衡类指标（温度、热通量、光伏）仅在文件含有时输出。
  const radianceResult = quantity.toLowerCase() === 'spectral radiance'
  const metricDefinitions = [
    ['radiosity', radianceResult ? '光谱辐亮度 [W m⁻² sr⁻¹ μm⁻¹]' : '辐射度', true],
    ['lightEnhancement', '光照增强', true],
    ['sunlit', '光照比例', true],
    ['temperature', '温度 [K]', false],
    ['netRadiation', '净辐射 [W m⁻²]', false],
    ['sensibleHeat', '显热 [W m⁻²]', false],
    ['latentHeat', '潜热 [W m⁻²]', false],
    ['storageHeat', '储热 [W m⁻²]', false],
    ['photovoltaicPower', '光伏功率 [W m⁻²]', false]
  ]
  const metrics = []
  for (const [id, name, required] of metricDefinitions) {
    const values = arrays[id]
    if (required && !values) throw new Error('面元结果缺少 ' + id + ' 数组')
    if (values) metrics.push([id, name, values])
  }
  return {
    facetCount,
    metrics,
    vertexPositions: arrays.vertexPositions,
    backend,
    iterations,
    maxDelta,
    wavelengths,
    bandRadiosity,
    bandStates: arrays.bandStates || new Float32Array(0),
    quantity,
    units
  }
}

// 三维显示的面元/体元采样上限（仅用于前端显示；磁盘上的结果保留全部面元）。
const THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT = 1000000

// 作用：为三维显示读取面元结果的等间隔抽样（面元数超过 maximumFacets 时抽样），返回普通数组。
// 输出：{ kind:'facet', facetCount(抽样后), sourceFacetCount, sampled, metrics:[{id,label,values}],
//       vertexPositions, backend, iterations, maxDelta }。
// 说明：单遍扫描数组，只解析落在被选面元范围内的数值，其余仅跳过，避免一次性解析数 GB 数据。
//       抽样序号 = floor(i·N/n)（保持在 [0,N-1]）；每面元的指标有正反 2 个值。
function sampleRadiosityResult(jsonPath, maximumFacets = THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT, geometryOnly = false) {
  if (!existsSync(jsonPath)) throw new Error('找不到面元结果：' + jsonPath)
  const source = readLargeFileSync(jsonPath)
  const header = source.subarray(0, Math.min(source.length, 65536)).toString('utf8')
  const sourceFacetCount = Number(header.match(/"facetCount"\s*:\s*(\d+)/)?.[1])
  if (!Number.isInteger(sourceFacetCount) || sourceFacetCount < 1) throw new Error('面元结果缺少有效的 facetCount')
  const facetCount = Math.min(sourceFacetCount, maximumFacets)
  const sourceFacets = new Uint32Array(facetCount)
  for (let sampledFacet = 0; sampledFacet < facetCount; sampledFacet += 1) {
    sourceFacets[sampledFacet] = facetCount === sourceFacetCount
      ? sampledFacet
      : Math.min(sourceFacetCount - 1, Math.floor(sampledFacet * sourceFacetCount / facetCount))
  }

  // sampleArray：顺序扫描数组，sourceValue 为当前元素序号，仅取 [targetStart,targetEnd) 内的值写入抽样结果。
  const sampleArray = (key, valuesPerFacet) => {
    const keyOffset = source.indexOf('"' + key + '"')
    const arrayOffset = keyOffset < 0 ? -1 : source.indexOf(91, keyOffset)
    if (arrayOffset < 0) throw new Error('面元结果缺少 ' + key + ' 数组')
    const values = new Array(facetCount * valuesPerFacet)
    let position = arrayOffset + 1
    let sourceValue = 0
    let sampledFacet = 0
    let targetStart = sourceFacets[0] * valuesPerFacet
    let targetEnd = targetStart + valuesPerFacet
    while (position < source.length && sampledFacet < facetCount) {
      while (position < source.length && (source[position] === 32 || source[position] === 9 || source[position] === 10 || source[position] === 13 || source[position] === 44)) position += 1
      if (position >= source.length || source[position] === 93) break
      const numberStart = position
      while (position < source.length && source[position] !== 44 && source[position] !== 93) position += 1
      if (sourceValue >= targetStart && sourceValue < targetEnd) {
        const value = Number(source.toString('ascii', numberStart, position).trim())
        if (!Number.isFinite(value)) throw new Error(key + ' 包含无效数值')
        values[sampledFacet * valuesPerFacet + sourceValue - targetStart] = value
      }
      sourceValue += 1
      if (sourceValue === targetEnd) {
        sampledFacet += 1
        if (sampledFacet < facetCount) {
          targetStart = sourceFacets[sampledFacet] * valuesPerFacet
          targetEnd = targetStart + valuesPerFacet
        }
      }
    }
    if (sampledFacet !== facetCount) throw new Error(key + ' 数据不完整')
    return values
  }
  const scalar = (key) => {
    const keyOffset = source.indexOf('"' + key + '"')
    if (keyOffset < 0) return undefined
    const colon = source.indexOf(58, keyOffset)
    if (colon < 0) return undefined
    let end = colon + 1
    while (end < source.length && ![44, 10, 13, 125].includes(source[end])) end += 1
    const value = Number(source.toString('ascii', colon + 1, end).trim())
    return Number.isFinite(value) ? value : undefined
  }
  const backend = header.match(/"backend"\s*:\s*"([^"]*)"/)?.[1]
  const quantity = header.match(/"quantity"\s*:\s*"([^"]*)"/)?.[1] || ''
  const units = header.match(/"units"\s*:\s*"([^"]*)"/)?.[1] || ''
  const radiosityLabel = quantity.toLowerCase() === 'spectral radiance'
    ? '光谱辐亮度 [W m⁻² sr⁻¹ μm⁻¹]'
    : `辐射度${units ? ` [${units.replaceAll('-2', '⁻²')}]` : ' [W m⁻²]'}`
  const vertexPositions = sampleArray('vertexPositions', 9)
  // 指标清单（第三项为必需）；geometryOnly 时不读任何指标。此处没有光伏功率项（与 readRadiosityResult 不同）。
  const metricDefinitions = [
    ['radiosity', radiosityLabel, true],
    ['lightEnhancement', '光照增强 [-]', true],
    ['sunlit', '光照比例 [-]', true],
    ['temperature', '温度 [K]'],
    ['netRadiation', '净辐射 [W m⁻²]'],
    ['sensibleHeat', '显热 [W m⁻²]'],
    ['latentHeat', '潜热 [W m⁻²]'],
    ['storageHeat', '储热 [W m⁻²]']
  ]
  const metrics = metricDefinitions.filter(([key, , required]) => {
    const exists = source.indexOf('"' + key + '"') >= 0
    if (required && !exists && !geometryOnly) throw new Error('面元结果缺少 ' + key + ' 数组')
    return exists && !geometryOnly
  }).map(([id, label]) => ({ id, label, values: sampleArray(id, 2) }))

  return {
    kind: 'facet',
    path: jsonPath,
    facetCount,
    sourceFacetCount,
    sampled: facetCount < sourceFacetCount,
    metrics,
    vertexPositions,
    backend,
    iterations: scalar('iterations'),
    maxDelta: scalar('maxDelta')
  }
}

// 通过读前 64 KB 头部判断一个 JSON 是不是面元结果（含 backend、facetCount、vertexPositions）；出错返回 false。
function isRadiosityResultFile(path) {
  let descriptor
  try {
    descriptor = openSync(path, 'r')
    const sample = Buffer.alloc(Math.min(65536, statSync(path).size))
    readSync(descriptor, sample, 0, sample.length, 0)
    const header = sample.toString('utf8')
    return /"backend"\s*:/.test(header) &&
      /"facetCount"\s*:\s*\d+/.test(header) &&
      /"vertexPositions"\s*:\s*\[/.test(header)
  } catch {
    return false
  } finally {
    if (descriptor != null) closeSync(descriptor)
  }
}

// 读取三维过程结果的元数据 JSON（≤ 1 MB 且 kind 以 -process 结尾），否则返回 null。
// 元数据描述同目录的二进制数据：dataFile、recordFloats、fields[{id,label,offset}]、geometry(facet|voxel) 等。
function processResultMetadata(path) {
  if (extname(path).toLowerCase() !== '.json' || statSync(path).size > 1024 * 1024) return null
  try {
    const metadata = JSON.parse(readFileSync(path, 'utf8'))
    return String(metadata.kind || '').endsWith('-process') ? metadata : null
  } catch {
    return null
  }
}

// 作用：读取三维过程结果（面元或体元的逐元素过程量），按显示上限抽样后返回。
// 输入：元数据 JSON 路径、maximumElements。数据文件为 float32 小端定长记录，每条记录 recordFloats 个浮点，
//       各字段位于 fields[i].offset（以 float 为单位）。
// 步骤 1：校验元数据（字段定义、偏移不越界、元素数）。
// 步骤 2：面元：几何取自 geometryFile（缺省 ../faceteb.json），每面元有正反 2 条记录；
//         体元：每体元 1 条记录，positionOffsets 给出 xyz 在记录中的偏移（缺省 0,1,2）。
// 步骤 3：可选 soilProfile（土壤分层温度）按 layerCount/offsets/depths 读取，单位缺省 degC 与 m。
// FacetRT and FacetEB use the same display-only sampling limit. Files on disk
// retain every facet and all process values.
function readProcessResult(metadataPath, maximumElements = THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT) {
  const metadata = processResultMetadata(metadataPath)
  if (!metadata) throw new Error('无效的三维过程结果')
  const dataPath = resolve(dirname(metadataPath), String(metadata.dataFile || ''))
  if (!existsSync(dataPath)) throw new Error('找不到过程二进制数据：' + dataPath)
  const data = readLargeFileSync(dataPath)
  const recordFloats = Number(metadata.recordFloats)
  const fields = Array.isArray(metadata.fields)
    ? metadata.fields.filter((field) => field && typeof field === 'object' && Number.isInteger(Number(field.offset)))
    : []
  if (!Number.isInteger(recordFloats) || recordFloats < 1 || !fields.length) throw new Error('过程结果字段定义无效')
  if (fields.some((field) => Number(field.offset) < 0 || Number(field.offset) >= recordFloats)) throw new Error('过程结果字段偏移越界')
  const recordBytes = recordFloats * 4
  const sourceCount = Number(metadata.geometry === 'facet' ? metadata.facetCount : metadata.voxelCount)
  if (!Number.isInteger(sourceCount) || sourceCount < 1) throw new Error('过程结果缺少有效元素数量')

  if (metadata.geometry === 'facet') {
    const geometryPath = resolve(dirname(metadataPath), String(metadata.geometryFile || '../faceteb.json'))
    const geometry = sampleRadiosityResult(geometryPath, maximumElements, true)
    if (data.length < sourceCount * 2 * recordBytes) throw new Error('面元过程二进制数据不完整')
    const sampledMetrics = fields.map((field) => ({
      id: String(field.id || 'field_' + field.offset),
      label: String(field.label || field.id || '字段 ' + field.offset),
      values: new Array(geometry.facetCount * 2)
    }))
    for (let facet = 0; facet < geometry.facetCount; facet += 1) {
      const sourceFacet = geometry.facetCount === sourceCount
        ? facet
        : Math.min(sourceCount - 1, Math.floor(facet * sourceCount / geometry.facetCount))
      for (let side = 0; side < 2; side += 1) {
        const recordOffset = (sourceFacet * 2 + side) * recordBytes
        for (let metric = 0; metric < sampledMetrics.length; metric += 1) {
          sampledMetrics[metric].values[facet * 2 + side] =
            data.readFloatLE(recordOffset + Number(fields[metric].offset) * 4)
        }
      }
    }
    return {
      ...geometry,
      path: metadataPath,
      metrics: sampledMetrics,
      processType: metadata.processType,
      time: metadata.time,
      node: metadata.node,
      backend: 'HiStream Facet ' + String(metadata.processType || 'process')
    }
  }

  if (metadata.geometry !== 'voxel') throw new Error('不支持的过程几何类型：' + metadata.geometry)
  if (data.length < sourceCount * recordBytes) throw new Error('体元过程二进制数据不完整')
  const voxelCount = Math.min(sourceCount, maximumElements)
  // 体元抽样：与面元相同的等间隔下标映射。
  const sourceVoxels = new Uint32Array(voxelCount)
  for (let voxel = 0; voxel < voxelCount; voxel += 1) {
    sourceVoxels[voxel] = voxelCount === sourceCount
      ? voxel
      : Math.min(sourceCount - 1, Math.floor(voxel * sourceCount / voxelCount))
  }
  const positionOffsets = Array.isArray(metadata.positionOffsets) ? metadata.positionOffsets.map(Number) : [0, 1, 2]
  const voxelPositions = new Array(voxelCount * 3)
  const metrics = fields.map((field) => ({
    id: String(field.id || 'field_' + field.offset),
    label: String(field.label || field.id || '字段 ' + field.offset),
    values: new Array(voxelCount)
  }))
  const profileMetadata = metadata.soilProfile && typeof metadata.soilProfile === 'object'
    ? metadata.soilProfile : null
  const profileLayerCount = Number(profileMetadata?.layerCount)
  const profileOffsets = Array.isArray(profileMetadata?.offsets)
    ? profileMetadata.offsets.map(Number) : []
  const hasSoilProfile = Number.isInteger(profileLayerCount) && profileLayerCount > 0 &&
    profileOffsets.length === profileLayerCount &&
    profileOffsets.every((offset) => Number.isInteger(offset) && offset >= 0 && offset < recordFloats)
  const profileDepths = hasSoilProfile && Array.isArray(profileMetadata.depths) &&
    profileMetadata.depths.length === profileLayerCount
    ? profileMetadata.depths.map(Number) : Array.from({ length: profileLayerCount }, (_, layer) => layer)
  const soilProfile = hasSoilProfile ? {
    layerCount: profileLayerCount,
    temperatureUnit: String(profileMetadata.temperatureUnit || 'degC'),
    depthUnit: String(profileMetadata.depthUnit || 'm'),
    depths: profileDepths,
    values: Array.from({ length: profileLayerCount }, () => new Array(voxelCount))
  } : null
  for (let voxel = 0; voxel < voxelCount; voxel += 1) {
    const recordOffset = sourceVoxels[voxel] * recordBytes
    for (let axis = 0; axis < 3; axis += 1) {
      voxelPositions[voxel * 3 + axis] = data.readFloatLE(recordOffset + positionOffsets[axis] * 4)
    }
    for (let metric = 0; metric < metrics.length; metric += 1) {
      metrics[metric].values[voxel] = data.readFloatLE(recordOffset + Number(fields[metric].offset) * 4)
    }
    if (soilProfile) {
      for (let layer = 0; layer < soilProfile.layerCount; layer += 1) {
        soilProfile.values[layer][voxel] = data.readFloatLE(recordOffset + profileOffsets[layer] * 4)
      }
    }
  }
  return {
    kind: 'voxel',
    path: metadataPath,
    voxelCount,
    sourceVoxelCount: sourceCount,
    sampled: voxelCount < sourceCount,
    voxelSize: Number(metadata.voxelSize) || 1,
    voxelPositions,
    metrics,
    soilProfile,
    processType: metadata.processType,
    time: metadata.time,
    node: metadata.node,
    backend: 'HiStream Voxel ' + String(metadata.processType || 'process')
  }
}

// 模块 8b：FacetRT/FacetEB 面元结果 → 单视角影像 TIFF（纯 CPU 光栅化，不依赖 GPU）。
// 作用：把面元结果（每面元正反面的辐亮度/温度等）投影到传感器像面，写出多波段 float32 TIFF，
//       并按需写出统计 CSV、过程量 TIFF 等附带产物（函数很长，按步骤分段）。
// 输入：jsonPath 面元结果；inputPath 冻结工程；mode；viewAngleOverride=[VZA,VAA]（度）覆盖工程视角；
//       step 非空表示 FacetEB 逐节点导出（只读几何，指标由 step 提供）；sharedResult 为同节点多视角共享的结果缓存。
// 坐标系：世界坐标 x=北，y=向上，z=东，原点在场景中心地面；方位角自 +X（北）顺时针向 +Z（东）；
//       天顶角自 +Y 起算；传感器位置 (positionX, positionY, height) 对应 (x+W/2, z+D/2, y)。
// 步骤 1：读取结果与工程，确定影像尺寸、波段名与输出类型（温度或反射率/辐亮度）。
// 步骤 2：建立相机（平行投影或透视）与大气/天空辐射函数。
// 步骤 3：按相机把三角形光栅化，得到每像元可见的（面元,正反面）索引与深度。
// 步骤 4：沿太阳方向做阴影光栅，判定每个可见点是否被照亮。
// 步骤 5：按光照状态与波段取值、叠加大气、写出 TIFF 与统计文件（见后续分段注释）。
function writeRadiosityTiffForAngle(jsonPath, inputPath, mode, viewAngleOverride = null, includeProcess = true, removeStepFiles = false, step = null, sharedResult = null) {
  const { facetCount, metrics, vertexPositions, backend, bandRadiosity, bandStates = new Float32Array(0), wavelengths: engineWavelengths = [] } = sharedResult || readRadiosityResult(jsonPath, Boolean(step))
  if (!vertexPositions || vertexPositions.length < facetCount * 9) throw new Error('面元结果缺少完整的三角形坐标')
  if (!inputPath || !existsSync(inputPath)) throw new Error('找不到生成影像所需的 project.json')
  const project = projectFromSource(readFileSync(inputPath, 'utf8'))
  const { scene, sensor, light, atmosphere = {}, meteo } = project.configuration
  // 影像尺寸：sensor.x/y 取正整数，缺省 512×512。
  const positiveInteger = (value, fallback) => {
    value = Math.round(Number(value))
    return Number.isInteger(value) && value > 0 ? value : fallback
  }
  const width = positiveInteger(sensor.x, 512)
  const height = positiveInteger(sensor.y, 512)
  // 波段（波长，nm）：优先用引擎结果里记录的波段；否则用工程配置；两者不一致仅警告，以引擎为准。
  // Band list: prefer the list the engine actually solved (written into the
  // result as "wavelengths"); otherwise use the shared rule in project-schema.js,
  // which ProjectJson::sensorBands() reproduces exactly.  A mismatch means an
  // engine/server version skew, so it is reported instead of silently mixed.
  const requestedWavelengths = sensorBandValues(sensor)
  if (!requestedWavelengths.length) requestedWavelengths.push(10500) // engine fallback (ProjectJson::sensorBands)
  const wavelengths = engineWavelengths.length ? [...engineWavelengths] : requestedWavelengths
  if (engineWavelengths.length && (engineWavelengths.length !== requestedWavelengths.length
    || engineWavelengths.some((value, index) => Math.abs(value - requestedWavelengths[index]) > 1e-3))) {
    console.warn(`面元结果波段与工程配置不一致：引擎 ${engineWavelengths.length} 个，工程 ${requestedWavelengths.length} 个；按引擎写出的波段出图`)
  }
  // EB 模式且勾选“输出温度”时只输出 1 个波段（温度 K）；否则每个波长一个波段。
  const temperatureOutput = isEnergyBalanceMode(mode) && Boolean(sensor.temperature)
  const imageBandCount = temperatureOutput ? 1 : wavelengths.length
  // 光学/热红外判别：≤2.5 视为 µm 并换算 nm；波长 ≤ 2500 nm 为光学波段（输出反射率），其余输出光谱辐亮度。
  const isOpticalWavelength = (wavelength) => {
    const value = Number(wavelength)
    const nanometers = value <= 2.5 ? value * 1000 : value
    return nanometers <= 2500
  }
  const bandNames = temperatureOutput
    ? ['温度 [K]']
    : wavelengths.map((wavelength, index) => {
      return isOpticalWavelength(wavelength)
        ? 'Reflectance [-] @ ' + wavelength + ' nm'
        : 'Spectral radiance [W m-2 sr-1 um-1] @ ' + wavelength + ' nm'
    })
  // 步骤 2：相机与几何。viewAngles=[VZA,VAA]、sunAngles=[太阳天顶, 太阳方位]，单位度。
  // 下面是小型三维向量工具（dot3/cross3/…）与相机基（cameraBasis）：平行投影时 up 尽量取“北”方向在像面上的投影，
  // 透视投影时 up 取世界向上，退化（前向与首选 up 平行）时改用备选方向。
  const viewAngles = viewAngleOverride || [Number(sensor.vza) || 0, Number(sensor.vaa) || 0]
  const sunAngles = [Number(light.zenith) || 0, Number(light.azimuth) || 0]
  const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
  const subtract3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
  const length3 = (value) => Math.hypot(value[0], value[1], value[2])
  const normalize3 = (value, fallback) => {
    const length = length3(value)
    return length > 1e-9 ? value.map((component) => component / length) : fallback
  }
  const cameraBasis = (forward, perspectiveView = false) => {
    const north = [1, 0, 0]
    const worldUp = [0, 1, 0]
    const preferredUp = perspectiveView ? worldUp : north
    const fallbackUp = perspectiveView ? north : worldUp
    const preferredProjection = dot3(preferredUp, forward)
    let up = preferredUp.map((component, axis) => component - preferredProjection * forward[axis])
    if (length3(up) <= 1e-9) {
      const fallbackProjection = dot3(fallbackUp, forward)
      up = fallbackUp.map((component, axis) => component - fallbackProjection * forward[axis])
    }
    up = normalize3(up, [1, 0, 0])
    const right = normalize3(cross3(forward, up), [0, 0, 1])
    return { right, up: normalize3(cross3(right, forward), up) }
  }
  const projection = String(sensor.projection || 'parallel').trim().toLowerCase()
  const perspective = ['perspective', 'central', 'center'].includes(projection)
  const sceneWidth = Math.max(1e-6, Number(scene.x) || 1)
  const sceneDepth = Math.max(1e-6, Number(scene.y) || 1)
  // 相机位置（透视投影用）：传感器位置无效时缺省为场景正上方 3000 m；高度至少 0.01 m。
  const sensorPosition = [Number(sensor.positionX), Number(sensor.positionY), Number(sensor.height)]
  const sensorWorld = sensorPosition.every(Number.isFinite)
    ? sensorPosition
    : [sceneWidth / 2, sceneDepth / 2, 3000]
  const cameraPosition = [sensorWorld[0] - sceneWidth / 2, Math.max(0.01, sensorWorld[2]), sensorWorld[1] - sceneDepth / 2]
  const directionFromAngles = (zenithDegrees, azimuthDegrees) => {
    const zenith = (Number.isFinite(zenithDegrees) ? zenithDegrees : 0) * Math.PI / 180
    const azimuth = (Number.isFinite(azimuthDegrees) ? azimuthDegrees : 0) * Math.PI / 180
    return [Math.sin(zenith) * Math.cos(azimuth), Math.cos(zenith), Math.sin(zenith) * Math.sin(azimuth)]
  }
  // cameraOut 为从场景指向传感器的单位向量（由 VZA/VAA 得到），cameraForward 为其反向；
  // 透视投影的视场角 fov 限幅到 [0.1°,120°]，缺省 60°（垂直视场）。
  let cameraOut, cameraForward, cameraRight, cameraUp, perspectiveTanHalfFov = 1
  if (perspective) {
    cameraOut = directionFromAngles(viewAngles[0], viewAngles[1])
    cameraForward = cameraOut.map((component) => -component)
    ;({ right: cameraRight, up: cameraUp } = cameraBasis(cameraForward, true))
    const verticalFov = Math.max(0.1, Math.min(120, Number(sensor.fov) || 60))
    perspectiveTanHalfFov = Math.tan(verticalFov * 0.5 * Math.PI / 180)
  } else {
    // Azimuth is clockwise from +X (north) toward +Z (east).
    cameraOut = directionFromAngles(viewAngles[0], viewAngles[1])
    cameraForward = cameraOut.map((component) => -component)
    ;({ right: cameraRight, up: cameraUp } = cameraBasis(cameraForward))
  }
  // 命名用的视角：透视投影下由相机方向反算 VZA/VAA（文件名需要）；天顶几乎垂直时方位取 0。
  const horizontalViewLength = Math.hypot(cameraOut[0], cameraOut[2])
  const namingViewAngles = perspective
    ? [Math.acos(Math.max(-1, Math.min(1, cameraOut[1]))) * 180 / Math.PI,
      horizontalViewLength < 1e-9 ? 0 : (Math.atan2(cameraOut[2], cameraOut[0]) * 180 / Math.PI + 360) % 360]
    : viewAngles
  // 大气参数规整（非法值回落到缺省）：模式默认 midlatitude-summer，水汽限幅 [0.5,5]，气溶胶默认 rural，
  // 能见度限幅 [10,50] km、缺省 23；查找表缺省为 assets/atmosphere/simple_modtran_lut.csv；
  // 传感器海拔以 km 计（高度/1000）。
  const atmosphereEnabled = Boolean(atmosphere.enabled)
  const rawAtmosphereModel = String(atmosphere.model || '')
  const atmosphereModel = ['tropical', 'midlatitude-summer', 'midlatitude-winter'].includes(rawAtmosphereModel)
    ? rawAtmosphereModel : 'midlatitude-summer'
  const atmosphereWaterVapor = Math.max(0.5, Math.min(5, Number(atmosphere.waterVapor) || 2))
  const rawAtmosphereAerosol = String(atmosphere.aerosol || '')
  const atmosphereAerosol = ['rural', 'urban'].includes(rawAtmosphereAerosol) ? rawAtmosphereAerosol : 'rural'
  const atmosphereVisibility = Math.max(10, Math.min(50, Number(atmosphere.visibility) || 23))
  const configuredAtmospherePath = String(atmosphere.lutFile || '')
  const atmospherePath = configuredAtmospherePath && configuredAtmospherePath !== 'assets/atmosphere/simple_modtran_lut.csv'
    ? (isAbsolute(configuredAtmospherePath) ? configuredAtmospherePath : resolve(dirname(inputPath), configuredAtmospherePath))
    : join(RESOURCE_ROOT, 'assets', 'atmosphere', 'simple_modtran_lut.csv')
  const atmosphereTable = atmosphereEnabled ? loadAtmosphereLut(atmospherePath) : null
  const atmosphereAltitude = Math.max(0, Number(sensorWorld[2]) || 0) / 1000
  // 温度输出用的热红外波长（取第一个非光学波段，缺省 10500 nm）；天空等效温度下限 120 K，缺省 250 K。
  const thermalOutputWavelength = wavelengths.find((wavelength) => !isOpticalWavelength(wavelength)) || 10500
  const skyTemperature = Math.max(120, Number(light.skyTemperature) || 250)
  // 作用：天空（面元未被遮挡的半球背景）在某波长、某天顶角方向的输出值。
  // 光学波段（≤2500 nm）：返回无量纲的天空贡献（0.002–0.85），由经验光谱权重与气溶胶因子构成，
  //       有大气查找表时用其透过率代替经验透过率；各经验系数为工程经验值，物理来源待核对。
  // 热红外波段：优先用查找表的程辐射；否则用 Planck(skyTemperature) × 大气发射率，发射率随天顶角
  //       自天顶值向地平线线性增加；asTemperature 时再反演为亮温（K）。
  const skyOutputValue = (wavelengthNm, skyZenithDegrees, asTemperature) => {
    const wavelength = wavelengthNm > 0 && wavelengthNm <= 50 ? wavelengthNm * 1000 : wavelengthNm
    const zenithRadians = Math.max(0, Math.min(89.5, skyZenithDegrees)) * Math.PI / 180
    const correction = atmosphereTable
      ? sampleAtmosphereLut(atmosphereTable, atmosphereModel, atmosphereAerosol,
        atmosphereWaterVapor, atmosphereVisibility, atmosphereAltitude,
        skyZenithDegrees, wavelength)
      : { transmittance: 1, pathRadiance: 0 }
    if (wavelength <= 2500) {
      const spectralWeight = Math.max(0.06, Math.min(2, Math.pow(550 / Math.max(350, Math.min(2500, wavelength)), 1.8)))
      const aerosol = Math.max(0.4, Math.min(2.3, 23 / atmosphereVisibility))
      const cosineZenith = Math.max(0.12, Math.cos(zenithRadians))
      const airMass = 1 / cosineZenith
      const empiricalTransmittance = Math.exp(-(0.10 * spectralWeight + 0.025 * aerosol) * airMass)
      const transmittance = atmosphereTable ? correction.transmittance : empiricalTransmittance
      const horizonBoost = 1 + 0.55 * (1 - cosineZenith)
      return Math.max(0.002, Math.min(0.85,
        (1 - transmittance) * 0.55 * spectralWeight * horizonBoost + 0.008 * aerosol * horizonBoost))
    }
    let radiance = atmosphereTable ? correction.pathRadiance : 0
    if (!(radiance > 0)) {
      const cosineZenith = Math.max(0.05, Math.cos(zenithRadians))
      const zenithEmissivity = atmosphereTable && correction.transmittance < 0.999
        ? Math.max(0.08, Math.min(0.995, 1 - correction.transmittance)) : 0.72
      // Keep the fallback sky visibly directional: zenith radiance uses the
      // configured emissivity and increases linearly with 1-cos(theta)
      // toward the horizon radiance.
      const atmosphericFraction = Math.max(zenithEmissivity, Math.min(1,
        1 - (1 - zenithEmissivity) * cosineZenith))
      radiance = planckRadiance(wavelength, skyTemperature) * atmosphericFraction
    }
    return asTemperature ? inversePlanckTemperature(wavelength, radiance) : radiance
  }
  // 顶点读取与有效性检查：任何顶点含非有限坐标即报错并指出第几个面元。
  const point = (offset) => [Number(vertexPositions[offset]), Number(vertexPositions[offset + 1]), Number(vertexPositions[offset + 2])]
  for (let vertex = 0; vertex < facetCount * 3; vertex += 1) {
    const position = point(vertex * 3)
    if (!position.every(Number.isFinite)) throw new Error('第 ' + (Math.floor(vertex / 3) + 1) + ' 个面元包含无效坐标')
  }
  // 步骤 3：确定像面范围。平行投影下范围取场景地面四角在相机基上的投影包络（与引擎 Geometry::createSensor() 一致），
  // 再按影像宽高比补足并略微外扩半个像元；透视投影用归一化范围 [-1,1]。
  const imageAspect = width / height
  let viewMinimumX = perspective ? -1 : Infinity
  let viewMaximumX = perspective ? 1 : -Infinity
  let viewMinimumY = perspective ? -1 : Infinity
  let viewMaximumY = perspective ? 1 : -Infinity
  if (!perspective) {
    // Match Geometry::createSensor(): the angled source image is fitted to
    // the scene ground extent, not to the height-dependent mesh bounds.
    const groundCorners = [
      [-sceneWidth / 2, 0, -sceneDepth / 2],
      [-sceneWidth / 2, 0, sceneDepth / 2],
      [sceneWidth / 2, 0, -sceneDepth / 2],
      [sceneWidth / 2, 0, sceneDepth / 2]
    ]
    for (const position of groundCorners) {
      const projectedX = dot3(position, cameraRight)
      const projectedY = dot3(position, cameraUp)
      viewMinimumX = Math.min(viewMinimumX, projectedX)
      viewMaximumX = Math.max(viewMaximumX, projectedX)
      viewMinimumY = Math.min(viewMinimumY, projectedY)
      viewMaximumY = Math.max(viewMaximumY, projectedY)
    }
  }
  let viewRangeX = Math.max(1e-6, viewMaximumX - viewMinimumX)
  let viewRangeY = Math.max(1e-6, viewMaximumY - viewMinimumY)
  const viewCenterX = (viewMinimumX + viewMaximumX) / 2
  const viewCenterY = (viewMinimumY + viewMaximumY) / 2
  if (viewRangeX / viewRangeY > imageAspect) viewRangeY = viewRangeX / imageAspect
  else viewRangeX = viewRangeY * imageAspect
  viewRangeX *= 1 + 0.5 / width
  viewRangeY *= 1 + 0.5 / height
  viewMinimumX = viewCenterX - viewRangeX / 2
  viewMaximumY = viewCenterY + viewRangeY / 2

  // 正射（天底）影像范围：东西/南北范围按宽高比补足（与 VoxelRT 的天底影像约定一致）；orthographicScale 目前恒为 1。
  const orthographicScale = 1.0
  let orthRangeEast = sceneDepth
  let orthRangeNorth = sceneWidth
  if (orthRangeEast / orthRangeNorth > imageAspect) orthRangeNorth = orthRangeEast / imageAspect
  else orthRangeEast = orthRangeNorth * imageAspect
  orthRangeEast /= orthographicScale
  orthRangeNorth /= orthographicScale
  const orthMinimumZ = -orthRangeEast / 2
  const orthMaximumX = orthRangeNorth / 2

  const pixelCount = width * height
  // 把世界坐标顶点投影到像素坐标：平行投影用相机基的 right/up；透视投影按针孔模型，深度≤1e-6 视为无效；
  // depth 越大越近相机（取负的相机深度或沿 cameraOut 的距离），供深度缓冲比较。
  const viewVertex = (position) => {
    if (!perspective) return {
      x: (dot3(position, cameraRight) - viewMinimumX) / viewRangeX * width,
      y: (viewMaximumY - dot3(position, cameraUp)) / viewRangeY * height,
      depth: dot3(position, cameraOut), valid: true
    }
    const relative = subtract3(position, cameraPosition)
    const cameraDepth = dot3(relative, cameraForward)
    const halfWidth = perspectiveTanHalfFov * width / height
    return {
      x: (0.5 + dot3(relative, cameraRight) / (2 * cameraDepth * halfWidth)) * width,
      y: (0.5 - dot3(relative, cameraUp) / (2 * cameraDepth * perspectiveTanHalfFov)) * height,
      depth: -cameraDepth, valid: cameraDepth > 1e-6
    }
  }
  const edge = (a, b, x, y) => (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x)
  // 每个面元朝向观测者的那一面：由三角形法线与（相机方向或指向相机的向量）点积符号决定，
  // 值索引 = facet·2 + side（side 0 为法线正侧，1 为背侧）。
  const observedSideIndices = new Int32Array(facetCount)
  for (let facet = 0; facet < facetCount; facet += 1) {
    const a3 = point(facet * 9), b3 = point(facet * 9 + 3), c3 = point(facet * 9 + 6)
    const normal = cross3(subtract3(b3, a3), subtract3(c3, a3))
    const centroid = [(a3[0] + b3[0] + c3[0]) / 3, (a3[1] + b3[1] + c3[1]) / 3, (a3[2] + b3[2] + c3[2]) / 3]
    const sideDirection = perspective ? subtract3(cameraPosition, centroid) : cameraOut
    const localSide = dot3(normal, sideDirection) >= 0 ? 0 : 1
    observedSideIndices[facet] = facet * 2 + localSide
  }
  // 软件光栅化：逐三角形以重心坐标覆盖测试（像元中心采样，容差 1e-7），深度缓冲保留最近面，
  // 输出每像元的 (面元,面) 索引（-1 为空）、深度与可见像元数。
  const rasterizeSurfaceIndices = (projectVertex) => {
    const depthBuffer = new Float64Array(pixelCount)
    depthBuffer.fill(-Infinity)
    const surfaceIndexImage = new Int32Array(pixelCount)
    surfaceIndexImage.fill(-1)
    let visiblePixels = 0
    for (let facet = 0; facet < facetCount; facet += 1) {
      const valueIndex = observedSideIndices[facet]
      const a = projectVertex(point(facet * 9))
      const b = projectVertex(point(facet * 9 + 3))
      const c = projectVertex(point(facet * 9 + 6))
      if (!a.valid || !b.valid || !c.valid) continue
      const area = edge(a, b, c.x, c.y)
      if (Math.abs(area) < 1e-10) continue
      const minPixelX = Math.max(0, Math.floor(Math.min(a.x, b.x, c.x)))
      const maxPixelX = Math.min(width - 1, Math.ceil(Math.max(a.x, b.x, c.x)) - 1)
      const minPixelY = Math.max(0, Math.floor(Math.min(a.y, b.y, c.y)))
      const maxPixelY = Math.min(height - 1, Math.ceil(Math.max(a.y, b.y, c.y)) - 1)
      for (let y = minPixelY; y <= maxPixelY; y += 1) {
        for (let x = minPixelX; x <= maxPixelX; x += 1) {
          const sampleX = x + 0.5, sampleY = y + 0.5
          const weightA = edge(b, c, sampleX, sampleY) / area
          const weightB = edge(c, a, sampleX, sampleY) / area
          const weightC = 1 - weightA - weightB
          if (weightA < -1e-7 || weightB < -1e-7 || weightC < -1e-7) continue
          const depth = weightA * a.depth + weightB * b.depth + weightC * c.depth
          const pixel = y * width + x
          if (depth <= depthBuffer[pixel]) continue
          if (!Number.isFinite(depthBuffer[pixel])) visiblePixels += 1
          depthBuffer[pixel] = depth
          surfaceIndexImage[pixel] = valueIndex
        }
      }
    }
    return { surfaceIndexImage, depthBuffer, visiblePixels }
  }
  // 先渲染所需（可能倾斜）观测视角，其结果供后续逐像元取值使用。
  // Match VoxelRT: first render the requested oblique observation, then map
  // each nadir ground pixel back into that source image.  Directly rerendering
  // visible facets from nadir changes crown displacement and occlusion.
  const observedImage = rasterizeSurfaceIndices(viewVertex)
  // Sunlit/shaded state of every observed point, decided geometrically: the
  // point is sunlit when its own facet is the surface nearest the sun along
  // the solar ray through it (a shadow raster from the sun direction).
  const useSurfaceStates = !temperatureOutput
    && bandStates.length >= facetCount * 4 * wavelengths.length
  // The shadow raster depends only on the sun and the geometry, so all view
  // angles of one node share it through the node's shared result.
  const sunRasterCache = sharedResult?.sunRasters
  // 太阳方向阴影光栅：沿太阳方向把全部三角形投到一张“太阳视角”深度图（分辨率受 maxCells=3072 与
  // 像元大小/8 约束），记录每格最近的面元；太阳在地平线之下（sunOut[1] ≤ 1e-4）则不建立（视为无直射）。
  // 同一节点各视角共享缓存（key = 太阳天顶,方位）。
  const sunRasterFor = (sunZenith, sunAzimuth) => {
    const key = sunZenith + ',' + sunAzimuth
    if (sunRasterCache?.has(key)) return sunRasterCache.get(key)
    let raster = null
    const sunOut = directionFromAngles(sunZenith, sunAzimuth)
    if (sunOut[1] > 1e-4) {
      const { right: sunRight, up: sunUp } = cameraBasis(sunOut.map((component) => -component))
      let minU = Infinity, maxU = -Infinity, minV = Infinity, maxV = -Infinity
      for (let vertex = 0; vertex < facetCount * 3; vertex += 1) {
        const position = point(vertex * 3)
        const u = dot3(position, sunRight), v = dot3(position, sunUp)
        minU = Math.min(minU, u); maxU = Math.max(maxU, u)
        minV = Math.min(minV, v); maxV = Math.max(maxV, v)
      }
      const imagePixel = perspective
        ? Math.max(sceneWidth, sceneDepth) / Math.max(width, height)
        : Math.min(sceneWidth / width, sceneDepth / height)
      const maxCells = 3072
      const cell = Math.max(imagePixel / 8, (maxU - minU) / maxCells, (maxV - minV) / maxCells, 1e-6)
      const nx = Math.ceil((maxU - minU) / cell) + 1
      const ny = Math.ceil((maxV - minV) / cell) + 1
      const sunIds = new Int32Array(nx * ny).fill(-1)
      const sunDepth = new Float32Array(nx * ny).fill(-Infinity)
      const sunVertex = (position) => ({
        x: (dot3(position, sunRight) - minU) / cell,
        y: (maxV - dot3(position, sunUp)) / cell,
        depth: dot3(position, sunOut)
      })
      for (let facet = 0; facet < facetCount; facet += 1) {
        const a = sunVertex(point(facet * 9))
        const b = sunVertex(point(facet * 9 + 3))
        const c = sunVertex(point(facet * 9 + 6))
        const area = edge(a, b, c.x, c.y)
        if (Math.abs(area) < 1e-12) continue
        const minX = Math.max(0, Math.floor(Math.min(a.x, b.x, c.x)))
        const maxX = Math.min(nx - 1, Math.ceil(Math.max(a.x, b.x, c.x)) - 1)
        const minY = Math.max(0, Math.floor(Math.min(a.y, b.y, c.y)))
        const maxY = Math.min(ny - 1, Math.ceil(Math.max(a.y, b.y, c.y)) - 1)
        for (let y = minY; y <= maxY; y += 1) {
          for (let x = minX; x <= maxX; x += 1) {
            const sampleX = x + 0.5, sampleY = y + 0.5
            const weightA = edge(b, c, sampleX, sampleY) / area
            const weightB = edge(c, a, sampleX, sampleY) / area
            const weightC = 1 - weightA - weightB
            if (weightA < -1e-7 || weightB < -1e-7 || weightC < -1e-7) continue
            const depth = weightA * a.depth + weightB * b.depth + weightC * c.depth
            const cellIndex = y * nx + x
            if (depth <= sunDepth[cellIndex]) continue
            sunDepth[cellIndex] = depth
            sunIds[cellIndex] = facet
          }
        }
      }
      raster = { sunOut, sunRight, sunUp, minU, maxV, cell, nx, ny, sunIds, sunDepth }
    }
    sunRasterCache?.set(key, raster)
    return raster
  }
  // 判定每个可见像元对应的点是否被太阳直射（Uint8 掩膜）。
  // isSunlit：该点所在格内最近面元就是自己（或格内无面元）则受照；若另有面元比它更近太阳超过 2 个格宽则处于阴影；
  // 否则在 3×3 邻域内找自己以容忍共享边的离散误差。
  const computeObservedLit = (sunZenith, sunAzimuth) => {
    const observedLit = new Uint8Array(pixelCount)
    const raster = sunRasterFor(sunZenith, sunAzimuth)
    if (raster) {
      const { sunOut, sunRight, sunUp, minU, maxV, cell, nx, ny, sunIds, sunDepth } = raster
      const isSunlit = (facet, position) => {
        const i = Math.floor((dot3(position, sunRight) - minU) / cell)
        const j = Math.floor((maxV - dot3(position, sunUp)) / cell)
        if (i < 0 || j < 0 || i >= nx || j >= ny) return true
        const cellIndex = j * nx + i
        if (sunIds[cellIndex] === facet || sunIds[cellIndex] < 0) return true
        // Another surface clearly nearer the sun casts the shadow; otherwise the
        // hit lies on the shared edge of neighbouring facets.
        if (sunDepth[cellIndex] - dot3(position, sunOut) > 2 * cell) return false
        for (let dj = -1; dj <= 1; dj += 1) {
          for (let di = -1; di <= 1; di += 1) {
            const x = i + di, y = j + dj
            if (x >= 0 && y >= 0 && x < nx && y < ny && sunIds[y * nx + x] === facet) return true
          }
        }
        return false
      }
      const halfWidth = perspectiveTanHalfFov * width / height
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const pixel = y * width + x
          const side = observedImage.surfaceIndexImage[pixel]
          if (side < 0) continue
          const depth = observedImage.depthBuffer[pixel]
          let position
          if (perspective) {
            const cameraDepth = -depth
            const across = ((x + 0.5) / width - 0.5) * 2 * halfWidth
            const upward = (0.5 - (y + 0.5) / height) * 2 * perspectiveTanHalfFov
            position = [0, 1, 2].map((axis) => cameraPosition[axis] + cameraDepth * (
              cameraForward[axis] + across * cameraRight[axis] + upward * cameraUp[axis]))
          } else {
            const across = viewMinimumX + (x + 0.5) / width * viewRangeX
            const upward = viewMaximumY - (y + 0.5) / height * viewRangeY
            position = [0, 1, 2].map((axis) =>
              across * cameraRight[axis] + upward * cameraUp[axis] + depth * cameraOut[axis])
          }
          observedLit[pixel] = isSunlit(side >> 1, position) ? 1 : 0
        }
      }
    }
    return observedLit
  }
  // 步骤 4 结果：observedLit 为观测影像每像元是否受太阳直射；仅在引擎提供逐波段光照状态（bandStates）时才需要。
  const observedLit = useSurfaceStates
    ? computeObservedLit(sunAngles[0], sunAngles[1]) : new Uint8Array(pixelCount)
  // 平行投影时，输出为天底（正射）影像：对每个地面格点反查它在上面倾斜观测影像中的像元，
  // 继承该像元的 (面元,面) 索引与光照状态（orthSourcePixel）。网格按 width-1/height-1 等分取点；
  // 范围外的格点保持 -1（无数据）。透视投影直接使用观测影像。
  const orthSurfaceIndexImage = new Int32Array(pixelCount)
  orthSurfaceIndexImage.fill(-1)
  const orthSourcePixel = new Int32Array(pixelCount)
  orthSourcePixel.fill(-1)
  let orthVisiblePixels = 0
  const rasterWidth = Math.max(1, width - 1)
  const rasterHeight = Math.max(1, height - 1)
  for (let y = 0; y < height; y += 1) {
    const worldX = orthMaximumX - y / rasterHeight * orthRangeNorth
    if (worldX < -sceneWidth / 2 || worldX > sceneWidth / 2) continue
    for (let x = 0; x < width; x += 1) {
      const worldZ = orthMinimumZ + x / rasterWidth * orthRangeEast
      if (worldZ < -sceneDepth / 2 || worldZ > sceneDepth / 2) continue
      const source = viewVertex([worldX, 0, worldZ])
      if (!source.valid) continue
      const sourceX = Math.trunc(source.x)
      const sourceY = Math.trunc(source.y)
      if (sourceX < 0 || sourceX >= width || sourceY < 0 || sourceY >= height) continue
      const side = observedImage.surfaceIndexImage[sourceY * width + sourceX]
      if (side < 0) continue
      orthSurfaceIndexImage[y * width + x] = side
      orthSourcePixel[y * width + x] = sourceY * width + sourceX
      orthVisiblePixels += 1
    }
  }
  // 把观测影像的光照掩膜换算到输出影像（平行投影需经 orthSourcePixel 重映射）。
  const outputLitFrom = (lit) => perspective
    ? lit : Uint8Array.from(orthSourcePixel, (source) => source >= 0 ? lit[source] : 0)
  const outputSurfaceIndexImage = perspective
    ? observedImage.surfaceIndexImage : orthSurfaceIndexImage
  const outputLit = outputLitFrom(observedLit)
  // 透视投影下视线朝上（ray.y>0）、没有命中任何面元的像元属于天空：记录其天顶角（度），后续用 skyOutputValue 填充；
  // 其余像元保持 NaN。
  const skyZenithImage = new Float32Array(pixelCount)
  skyZenithImage.fill(Number.NaN)
  if (perspective) {
    const verticalScale = perspectiveTanHalfFov
    const horizontalScale = perspectiveTanHalfFov * width / height
    for (let y = 0; y < height; y += 1) {
      const screenY = (1 - 2 * (y + 0.5) / height) * verticalScale
      for (let x = 0; x < width; x += 1) {
        const screenX = (2 * (x + 0.5) / width - 1) * horizontalScale
        const ray = normalize3([
          cameraForward[0] + cameraRight[0] * screenX + cameraUp[0] * screenY,
          cameraForward[1] + cameraRight[1] * screenX + cameraUp[1] * screenY,
          cameraForward[2] + cameraRight[2] * screenX + cameraUp[2] * screenY
        ], cameraForward)
        if (ray[1] > 0) {
          skyZenithImage[y * width + x] = Math.acos(Math.max(0, Math.min(1, ray[1]))) * 180 / Math.PI
        }
      }
    }
  }

  // 步骤 5：节点与时间标记。EB 模式一次写出 [startNode,endNode) 各节点的 TIFF；逐步导出（step）只处理该节点。
  // dTime 为气象时间步长（秒），缺省 1800；时间标记与引擎命名一致：DOY<d>_HH-MM。
  const startNode = step ? step.node : Math.max(0, Math.round(Number(meteo.start) || 0))
  const endNode = step ? step.node + 1 : Math.max(startNode + 1, Math.round(Number(meteo.end) || startNode + 1))
  const dTime = Math.max(1, Number(meteo.dTime) || 1800)
  const clockToken = (seconds, dayOfYear = null) => {
    let totalMinutes = Math.round(seconds / 60)
    let day = dayOfYear
    if (totalMinutes >= 1440) {
      if (day != null) day += Math.floor(totalMinutes / 1440)
      totalMinutes %= 1440
    }
    const hour = String(Math.floor(totalMinutes / 60)).padStart(2, '0')
    const minute = String(totalMinutes % 60).padStart(2, '0')
    return (day == null ? '' : 'DOY' + day + '_') + hour + '-' + minute
  }
  const configuredMeteo = String(meteo.path || '')
  const meteoFile = configuredMeteo && configuredMeteo !== 'defined/meteo.txt' && configuredMeteo !== 'HiStream 内置气象数据'
    ? (isAbsolute(configuredMeteo) ? configuredMeteo : resolve(dirname(inputPath), configuredMeteo))
    : join(dirname(executable()), 'defined', 'meteo.txt')
  // 读取气象文件（跳过首行表头）以取各节点的儒略时间（第 1 列）；读不到时按 节点×dTime 估算。
  const meteoRows = meteoFile && existsSync(meteoFile)
    ? readFileSync(meteoFile, 'utf8').split(/\r?\n/).slice(1).filter((line) => line.trim())
    : []
  const timeForNode = (node) => {
    if (step && node === step.node) return { token: step.token, julianTime: step.julianTime }
    const julianTime = Number(String(meteoRows[node] || '').trim().split(/\s+/)[0])
    if (Number.isFinite(julianTime)) {
      const day = Math.floor(julianTime)
      return { token: clockToken((julianTime - day) * 86400, day), julianTime }
    }
    return { token: clockToken(node * dTime), julianTime: node * dTime / 86400 }
  }
  // 该时间节点的太阳天顶/方位角：RT 模式用工程光照角；EB 模式优先用 step 提供值，
  // 否则读取该节点过程元数据（facetrt/faceteb_T=<时间>.json）里的 solarZenith/Azimuth；都没有则返回空对象。
  const solarAnglesForTime = (time) => {
    if (!isEnergyBalanceMode(mode)) return { solarZenith: sunAngles[0], solarAzimuth: sunAngles[1] }
    if (step && time.token === step.token && Number.isFinite(step.solarZenith)) {
      return { solarZenith: step.solarZenith, solarAzimuth: step.solarAzimuth }
    }
    for (const prefix of ['facetrt', 'faceteb']) {
      const processPath = join(dirname(jsonPath), 'process', `${prefix}_T=${time.token}.json`)
      if (!existsSync(processPath)) continue
      const processInfo = JSON.parse(readFileSync(processPath, 'utf8'))
      if (Number.isFinite(processInfo.solarZenith)) return { solarZenith: processInfo.solarZenith, solarAzimuth: processInfo.solarAzimuth }
    }
    return {}
  }
  const angleToken = (value) => {
    const number = Number(value)
    return Number.isFinite(number) ? number.toFixed(2) : '0.00'
  }
  const vza = angleToken(namingViewAngles[0])
  const vaa = angleToken(namingViewAngles[1])
  // 非耦合（静态辐射度）结果：每个波段取 bandRadiosity 中对应切片；只有没有 bandRadiosity 的旧宽波段文件才用 radiosity。
  const staticRadiosity = metrics.find(([id]) => id === 'radiosity')?.[2]
  const surfaceValueCount = facetCount * 2
  // Per-band FacetRT values must cover exactly this band list.  Only results
  // without any bandRadiosity (broadband legacy files) fall back to radiosity;
  // a present-but-mismatched array used to fall back silently and fill every
  // band with the broadband value.
  if (bandRadiosity.length && bandRadiosity.length !== surfaceValueCount * wavelengths.length) {
    throw new Error(`面元逐波段结果与波段数不一致：bandRadiosity 含 ${bandRadiosity.length / surfaceValueCount} 个波段，出图需要 ${wavelengths.length} 个；请用当前引擎重新运行模拟`)
  }
  const staticBandValues = (band) => bandRadiosity.length
    ? bandRadiosity.slice(band * surfaceValueCount, (band + 1) * surfaceValueCount)
    : staticRadiosity
  // RT-EB 耦合后端：每个节点的逐面元逐波段值不在结果 JSON 中，而在 .facet_steps/energy_T=<时间>.bin 的定长记录里。
  // 记录布局：新版每面（facet×2+side）11 个侧面字段（含 9=直射点温度、10=阴影点温度）+ 每波段 2 个（直射、阴影），
  // 旧版 9 个字段 + 每波段 1 个；两种大小不可能同时匹配，匹配不上即波段数与引擎不一致，报错而不盲目解码。
  const coupledFacetEb = String(backend || '').includes('RT-EB coupled')
  // FacetEB step files can be hundreds of MiB each.  Keep only the active
  // node: retaining all time nodes grows memory linearly with the simulation.
  let activeStep = null
  const stepData = (time) => {
    if (!coupledFacetEb) return null
    if (activeStep?.token === time.token) return activeStep.data
    activeStep = null
    const stepPath = join(dirname(jsonPath), '.facet_steps', 'energy_T=' + time.token + '.bin')
    if (!existsSync(stepPath)) throw new Error('缺少 FacetEB 耦合节点结果：' + basename(stepPath))
    const buffer = readFileSync(stepPath)
    const surfaceCount = facetCount * 2
    const recordFloats = buffer.length / (surfaceCount * 4)
    if (!Number.isInteger(recordFloats) || recordFloats < 9) {
      throw new Error('FacetEB 节点结果不完整：' + basename(stepPath))
    }
    // Current layout: 11 side fields (… sunlit part, sunlit T, shaded T) then a
    // sunlit-point and a shaded-point value per band; legacy: 9 + one per band.
    // For a given band count the two sizes can never coincide, so a record that
    // matches neither is a band-count mismatch (engine/server skew) and must not
    // be decoded with the legacy offsets (that used to misread every field).
    const stateLayout = recordFloats === 11 + 2 * wavelengths.length
    const legacyLayout = recordFloats === 9 + wavelengths.length
    if (!stateLayout && !legacyLayout) {
      const engineBands = (recordFloats - 11) % 2 === 0 ? (recordFloats - 11) / 2 : null
      throw new Error(`FacetEB 节点结果与波段数不一致：每个面 ${recordFloats} 个字段`
        + (engineBands !== null ? `（引擎按 ${engineBands} 个波段写出）` : '')
        + `，出图需要 ${wavelengths.length} 个波段；请用当前引擎重新运行模拟：` + basename(stepPath))
    }
    const data = { buffer, recordFloats, recordBytes: recordFloats * 4, path: stepPath, stateLayout }
    activeStep = { token: time.token, data }
    return data
  }
  const releaseStepData = (time) => {
    if (activeStep?.token !== time.token) return ''
    const stepPath = activeStep.data.path
    activeStep = null
    return stepPath
  }
  // 把节点 step 文件中某波段（或温度）的值栅格化：逐像元取对应面的直射/阴影值，无数据像元为 NaN。
  const rasterizeStepField = (time, band, lit) => {
    const { buffer, recordFloats, recordBytes, path, stateLayout } = stepData(time)
    const fieldIndex = (sunlitPoint) => stateLayout
      ? (temperatureOutput ? (sunlitPoint ? 9 : 10) : 11 + 2 * band + (sunlitPoint ? 0 : 1))
      : (temperatureOutput ? 0 : 9 + band)
    if (fieldIndex(true) >= recordFloats || fieldIndex(false) >= recordFloats) {
      throw new Error('FacetEB 节点结果缺少逐波段反射率/辐亮度，请重新运行模拟：' + basename(path))
    }
    const litOffset = fieldIndex(true) * 4
    const shadedOffset = fieldIndex(false) * 4
    const image = new Float32Array(pixelCount)
    image.fill(Number.NaN)
    for (let pixel = 0; pixel < pixelCount; pixel += 1) {
      const side = outputSurfaceIndexImage[pixel]
      if (side >= 0) image[pixel] = buffer.readFloatLE(side * recordBytes + (lit[pixel] ? litOffset : shadedOffset))
    }
    return image
  }
  // （旧版记录没有直射/阴影分项，沿用工程光照角的 outputLit。）
  // Sunlit/shaded state of each output pixel under this node's own sun.
  const stepOutputLit = (time) => {
    if (!stepData(time).stateLayout) return outputLit
    const solar = solarAnglesForTime(time)
    return outputLitFrom(computeObservedLit(
      Number.isFinite(solar.solarZenith) ? solar.solarZenith : sunAngles[0],
      Number.isFinite(solar.solarAzimuth) ? solar.solarAzimuth : sunAngles[1]))
  }
  // bandStates 布局：每波段 facetCount×4，每面元依次为 正面直射、正面阴影、背面直射、背面阴影。
  const rasterizeSurfaceStates = (band) => {
    const image = new Float32Array(pixelCount)
    image.fill(Number.NaN)
    const bandOffset = band * facetCount * 4
    for (let pixel = 0; pixel < pixelCount; pixel += 1) {
      const side = outputSurfaceIndexImage[pixel]
      if (side < 0) continue
      image[pixel] = bandStates[bandOffset + (side >> 1) * 4 + (side & 1) * 2 + (outputLit[pixel] ? 0 : 1)]
    }
    return image
  }
  // 把按 (面元,面) 排列的数值数组直接栅格化（无直射/阴影区分）。
  const rasterizeValues = (values) => {
    const image = new Float32Array(pixelCount)
    image.fill(Number.NaN)
    for (let pixel = 0; pixel < pixelCount; pixel += 1) {
      const side = outputSurfaceIndexImage[pixel]
      if (side >= 0) image[pixel] = Number(values[side])
    }
    return image
  }

  // 步骤 5（续）：逐节点逐波段生成影像并写 TIFF（sensor.image=false 时跳过）。
  const tifPaths = []
  if (sensor.image !== false) {
    const imageNodes = isEnergyBalanceMode(mode)
      ? Array.from({ length: endNode - startNode }, (_, index) => startNode + index)
      : [startNode]
    for (const node of imageNodes) {
      const time = timeForNode(node)
      let completed = false
      try {
        const imageValues = new Float32Array(pixelCount * imageBandCount)
        // 每个波段：栅格化 → 无面元的像元：透视投影下有天空则填天空值 → 有大气查找表时做大气修正：
        //   温度输出：B(T)·τ + 程辐射 再反演亮温；光学波段：反射率 × τ（无绝对辐照度，不叠加程辐射）；
        //   热红外辐亮度：L·τ + 程辐射。
        const nodeLit = coupledFacetEb ? stepOutputLit(time) : outputLit
        for (let band = 0; band < imageBandCount; band += 1) {
          const rasterized = coupledFacetEb
            ? rasterizeStepField(time, band, nodeLit)
            : useSurfaceStates ? rasterizeSurfaceStates(band) : rasterizeValues(staticBandValues(band))
          imageValues.set(rasterized, band * pixelCount)
          const wavelengthValue = temperatureOutput ? thermalOutputWavelength : wavelengths[band]
          const wavelengthNm = wavelengthValue > 0 && wavelengthValue <= 50 ? wavelengthValue * 1000 : wavelengthValue
          // 大气修正按观测天顶角（命名用 VZA）与传感器海拔取查找表值。
          const correction = atmosphereTable
            ? sampleAtmosphereLut(atmosphereTable, atmosphereModel, atmosphereAerosol,
              atmosphereWaterVapor, atmosphereVisibility,
              atmosphereAltitude, namingViewAngles[0], wavelengthNm)
            : null
          const offset = band * pixelCount
          for (let pixel = 0; pixel < pixelCount; pixel += 1) {
            const index = offset + pixel
            const value = imageValues[index]
            if (!Number.isFinite(value)) {
              if (Number.isFinite(skyZenithImage[pixel])) {
                imageValues[index] = skyOutputValue(
                  wavelengthNm, skyZenithImage[pixel], temperatureOutput)
              }
              continue
            }
            if (!correction) continue
            if (temperatureOutput) {
              const corrected = planckRadiance(wavelengthNm, value) * correction.transmittance + correction.pathRadiance
              imageValues[index] = corrected > 0 ? inversePlanckTemperature(wavelengthNm, corrected) : value
            } else if (wavelengthNm <= 2500) {
              // Reflection factors are dimensionless. There is no absolute
              // spectral irradiance here to normalize the LUT path radiance.
              imageValues[index] = value * correction.transmittance
            } else {
              imageValues[index] = value * correction.transmittance + correction.pathRadiance
            }
          }
        }
        // 写出 TIFF：文件名规则见 outputTiffName；finally 中释放节点 step 文件，
        // 成功且 removeStepFiles 时删除它（失败则保留以便重试）。
        const tifName = isEnergyBalanceMode(mode)
          ? outputTiffName(mode, { timeToken: time.token, vza, vaa, atmosphere: atmosphereEnabled })
          : outputTiffName(mode, { sza: sunAngles[0], saa: sunAngles[1], vza, vaa, atmosphere: atmosphereEnabled })
        const tifPath = join(dirname(jsonPath), tifName)
        writeFloatTiff(tifPath, width, height, imageBandCount, imageValues, bandNames, solarAnglesForTime(time), mode)
        tifPaths.push(tifPath)
        completed = true
      } finally {
        const stepPath = releaseStepData(time)
        if (completed && removeStepFiles && stepPath && existsSync(stepPath)) {
          try { unlinkSync(stepPath) } catch { /* TIFF 已生成，临时节点文件可稍后清理。 */ }
        }
      }
    }
  }

  // 步骤 6：EB 模式的过程量输出。耦合后端：收集引擎已写出的 facetrt/faceteb 过程元数据（兼容旧命名 radiation/energy_T=…_f.json）；
  // 非耦合（静态辐射度）：把结果 JSON 复制为 faceteb_T=*.json，并为每个节点写 .meta.json 指向它。
  const processPaths = []
  if (includeProcess && processOutputEnabled(project, mode) && isEnergyBalanceMode(mode)) {
    const processDirectory = join(dirname(jsonPath), 'process')
    mkdirSync(processDirectory, { recursive: true })
    if (coupledFacetEb) {
      const selected = processOutputSelection(project, mode)
      const processTypes = [selected.radiation && 'radiation', selected.energy && 'energy'].filter(Boolean)
      for (let node = startNode; node < endNode; node += 1) {
        for (const type of processTypes) {
          const processModel = type === 'radiation' ? 'facetrt' : 'faceteb'
          const metadataPath = join(processDirectory, processModel + '_T=' + timeForNode(node).token + '.json')
          const legacyPath = join(processDirectory, type + '_T=' + timeForNode(node).token + '_f.json')
          if (existsSync(metadataPath)) processPaths.push(metadataPath)
          else if (existsSync(legacyPath)) processPaths.push(legacyPath)
        }
      }
    } else {
      const startTime = timeForNode(startNode).token
      const endTime = timeForNode(endNode - 1).token
      const processDataPath = join(processDirectory, 'faceteb_T=' + (startTime === endTime ? startTime : startTime + '_to_' + endTime) + '.json')
      copyFileSync(jsonPath, processDataPath)
      for (let node = startNode; node < endNode; node += 1) {
        const time = timeForNode(node)
        const metadataPath = join(processDirectory, 'faceteb_T=' + time.token + '.meta.json')
        writeFileSync(metadataPath, JSON.stringify({
          kind: 'facet-energy-process',
          model: 'faceteb',
          node,
          julianTime: time.julianTime,
          time: time.token,
          facetCount,
          dataFile: basename(processDataPath),
          fields: metrics.map(([id]) => id),
          note: 'FacetRT is a static radiosity solve; requested times reference the same solved field.'
        }, null, 2) + '\n', 'utf8')
        processPaths.push(metadataPath)
      }
    }
  }
  return { tifPath: tifPaths[0] || '', tifPaths, processPaths, width, height, bands: imageBandCount, bandNames, visiblePixels: orthVisiblePixels }
}

// 作用：对工程配置的每个观测视角（透视投影只有一个）调用 writeRadiosityTiffForAngle，并汇总其产物。
// 说明：只有第一个视角写过程量；最后一个视角后清理逐步文件；逐步导出时同一节点的各视角共享几何与太阳阴影光栅（sharedResult）；
//       节点全部视角成功后才删除该节点的 energy_T=*.bin。
function writeRadiosityTiff(jsonPath, inputPath, mode, step = null, cachedGeometry = null) {
  if (!inputPath || !existsSync(inputPath)) throw new Error('找不到生成影像所需的 project.json')
  const project = projectFromSource(readFileSync(inputPath, 'utf8'))
  const { sensor, light } = project.configuration
  const perspective = ['perspective', 'central', 'center'].includes(String(sensor.projection || 'parallel').trim().toLowerCase())
  const configuredAngles = sensorViewAngles(sensor, light.azimuth)
  const angles = perspective ? [null] : (configuredAngles.length ? configuredAngles : [[0, 0]])
  const sharedResult = step ? { ...(cachedGeometry || readRadiosityResult(jsonPath, true)), sunRasters: new Map() } : null
  const results = angles.map((angle, index) => writeRadiosityTiffForAngle(
    jsonPath, inputPath, mode, angle, index === 0, !step && index === angles.length - 1, step, sharedResult))
  // Delete only this completed node, after every direction succeeds. On export
  // failure retain its binary so results can be recovered/retried externally.
  if (step) {
    const stepPath = join(dirname(jsonPath), '.facet_steps', 'energy_T=' + step.token + '.bin')
    if (existsSync(stepPath)) unlinkSync(stepPath)
  }
  const first = results[0]
  return {
    ...first,
    tifPath: results.flatMap((result) => result.tifPaths)[0] || '',
    tifPaths: results.flatMap((result) => result.tifPaths),
    processPaths: [...new Set(results.flatMap((result) => result.processPaths))],
    visiblePixels: results.reduce((sum, result) => sum + result.visiblePixels, 0),
    angleCount: results.length
  }
}

// 模块 8c：FacetEB 逐节点出图的 worker 调度（引擎每算完一个节点写 step 文件，服务端出图后再放行引擎）。
// FacetEB node images are produced by one worker per run that stays alive for
// all nodes and keeps the parsed facet geometry (tens of MB of JSON) in
// memory. The former one-worker-per-node scheme re-loaded server.mjs and
// re-parsed the geometry for every node while the engine waited for its ACK.
// Very large results (above FACET_STEP_RESIDENT_FACETS) still use a fresh
// worker per node so its heap is released before the engine continues.
const FACET_STEP_RESIDENT_FACETS = Number(process.env.STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS ?? 3_000_000)

// worker 启动参数：workerData 带任务类型与参数；去掉 --input-type 等不适用于 worker 的 execArgv。
function facetStepWorkerOptions(kind, extra = {}) {
  return {
    workerData: { kind, ...extra },
    execArgv: process.execArgv.filter(arg => !arg.startsWith('--input-type'))
  }
}

// 作用：为单个节点新建一次性 worker 出图（超大结果用，退出即释放整个堆）。
// 说明：等待 worker 的 exit 事件而不仅是 message，确保内存已释放后才让引擎继续。
function exportFacetStepOnce(running, jsonPath, inputPath, step) {
  return new Promise((resolveStep, rejectStep) => {
    const worker = new Worker(new URL(import.meta.url), facetStepWorkerOptions('facet-step', { jsonPath, inputPath, step }))
    running.facetWorker = worker
    let result, failure
    worker.on('message', (message) => { result = message })
    worker.on('error', (error) => { failure = error })
    // Wait for teardown, not just a message: the old node's JS heap and array
    // buffers must be gone before the engine is allowed to compute again.
    worker.on('exit', (code) => {
      if (running.facetWorker === worker) running.facetWorker = null
      if (failure || code !== 0 || !result) rejectStep(failure || new Error('节点出图线程异常结束'))
      else resolveStep(result)
    })
  })
}

// 只读前 4 KB 取 facetCount，用于决定走常驻 worker 还是一次性 worker；读不到返回 0。
function facetResultCount(jsonPath) {
  try {
    const fd = openSync(jsonPath, 'r')
    try {
      const buffer = Buffer.alloc(4096)
      const bytes = readSync(fd, buffer, 0, buffer.length, 0)
      return Number(buffer.toString('utf8', 0, bytes).match(/"facetCount"\s*:\s*(\d+)/)?.[1]) || 0
    } finally { closeSync(fd) }
  } catch { return 0 }
}

// 作用：节点出图入口。面元数 > FACET_STEP_RESIDENT_FACETS 走一次性 worker；
//       否则复用 running 上常驻的 facet-step-service worker（同一次运行内保持几何缓存），一次只处理一个请求。
// 输出：Promise，解析为 writeRadiosityTiff 的返回值；worker 异常退出则让挂起请求失败。
function exportFacetStep(running, jsonPath, inputPath, step) {
  if (facetResultCount(jsonPath) > FACET_STEP_RESIDENT_FACETS) return exportFacetStepOnce(running, jsonPath, inputPath, step)
  return new Promise((resolveStep, rejectStep) => {
    let service = running.facetStepService
    if (!service || service.dead) {
      const worker = new Worker(new URL(import.meta.url), facetStepWorkerOptions('facet-step-service'))
      service = { worker, pending: null, dead: false }
      running.facetStepService = service
      running.facetWorker = worker
      worker.on('message', (message) => {
        const pending = service.pending
        service.pending = null
        if (!pending) return
        if (message?.ok) pending.resolve(message.result)
        else pending.reject(new Error(message?.error || '节点出图失败'))
      })
      const fail = (error) => {
        service.dead = true
        if (running.facetWorker === worker) running.facetWorker = null
        const pending = service.pending
        service.pending = null
        pending?.reject(error)
      }
      worker.on('error', fail)
      worker.on('exit', (code) => fail(new Error(`节点出图线程已退出（${code}）`)))
    }
    service.pending = { resolve: resolveStep, reject: rejectStep }
    service.worker.postMessage({ jsonPath, inputPath, step })
  })
}

// 模块 9：工程文件读写。输入若不是 project.json 则取同目录的 project.json。
function projectJsonPath(inputPath) {
  return basename(inputPath).toLowerCase() === 'project.json' ? inputPath : join(dirname(inputPath), 'project.json')
}

// 作用：首次以统一格式覆盖旧版工程前，把旧文件备份为 project.v<schemaVersion>.json（已存在不覆盖）；失败不抛出。
// Unified projects keep the legacy file as project.v<schema>.json before the first rewrite.
function backupLegacyProject(path) {
  try {
    if (!existsSync(path)) return ''
    const source = readFileSync(path, 'utf8')
    const document = JSON.parse(source.replace(/^\uFEFF/, ''))
    if (isUnifiedProject(document)) return ''
    const version = Number(document?.schemaVersion) || 1
    const backup = join(dirname(path), `project.v${version}.json`)
    if (!existsSync(backup)) writeFileSync(backup, source, 'utf8')
    return backup
  } catch { return '' }
}

// 原子写统一工程：先写 .tmp 再 rename（同时解析两端 realpath，兼容中文目录的 junction）；失败清理临时文件并抛出。
function writeUnifiedProject(path, unified) {
  const temporaryPath = path + '.tmp'
  try {
    writeFileSync(temporaryPath, stringifyUnifiedProject(unified), 'utf8')
    renameSync(realpathSync(temporaryPath), join(realpathSync(dirname(path)), basename(path)))
  } catch (error) {
    try { if (existsSync(temporaryPath)) unlinkSync(temporaryPath) } catch {}
    throw error
  }
  return unified
}

// 读取磁盘上已有的工程 JSON（去 BOM），不存在或损坏返回 null。
function existingProjectDocument(path) {
  if (!existsSync(path) || !statSync(path).isFile()) return null
  try { return JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) } catch { return null }
}

// 作用：保存 HiStream 工程。规范化 value、路径转存储形式、刷新 updatedAt，再写入。
// 说明：已有统一工程只替换 HiStream 段；旧格式则先备份再整体转为统一格式。
function writeProject(path, value, previousDocument = null) {
  const project = normalizeStoredProjectPaths(normalizeProject(value), dirname(path))
  project.updatedAt = new Date().toISOString()
  const existing = previousDocument ?? existingProjectDocument(path)
  // 已有的统一工程只替换 HiStream 段，保留 STREAM 段与由 STREAM 写入的共享字段。
  if (isUnifiedProject(existing)) writeUnifiedProject(path, applyHistreamToUnified(normalizeUnified(existing), project))
  else {
    backupLegacyProject(path)
    writeUnifiedProject(path, histreamProjectToUnified(project))
  }
  return project
}

// 作用：读取并规范化工程。统一格式经 unifiedToHistreamProject 取出 HiStream 部分（同时返回 unified 原文）；
//       STREAM 工程（kind 为 stream-*-project）不能在此打开，抛出提示；不存在返回 { project:null }。
function readProject(inputPath) {
  const path = projectJsonPath(inputPath)
  if (!existsSync(path)) return { project: null, projectPath: '' }
  let document
  try {
    document = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
  } catch (error) {
    throw new Error(`project.json 格式错误：${error.message}`)
  }
  if (isUnifiedProject(document)) {
    const unified = normalizeUnified(document)
    return { project: normalizeProject(unifiedToHistreamProject(unified)), projectPath: path, unified }
  }
  if (document?.kind === 'stream-image-project' || document?.kind === 'stream-scene-project') {
    throw new Error('这是 STREAM 遥感影像模拟工程，请在“遥感影像模拟”工作台中打开')
  }
  return { project: normalizeProject(document), projectPath: path }
}

// 兼容旧工程：第一个没有 OBJ 的非植被对象补上默认房屋模型并复制到工程目录；参数化植被刻意没有 OBJ，不处理。
// 返回是否发生了迁移。
function ensureDefaultSceneAsset(project, baseDir) {
  const items = project?.configuration?.objects?.items
  if (!Array.isArray(items) || !items.length) return false
  // Parametric vegetation uses shape/dimensions and deliberately has no OBJ.
  // Opening such a project must not replace its canopy with a default house.
  const first = items.find((item) => item.type !== 'Vegetation' && !String(item.fileName || '').trim())
  if (!first || !existsSync(defaultSceneAsset)) return false
  first.fileName = DEFAULT_SCENE_MODEL
  const destination = join(baseDir, DEFAULT_SCENE_MODEL)
  mkdirSync(dirname(destination), { recursive: true })
  if (!existsSync(destination)) copyFileSync(defaultSceneAsset, destination)
  return true
}
// 作用：打开工程时的一次性迁移。补默认场景、修复被误写成“…/C:…”的气象路径（回落内置气象）、
//       把资源路径规范化为存储形式；仅当有实际变化时才回写 project.json，避免无谓改动文件。
function migrateProject(inputPath, project) {
  if (!project) return project
  const originalPaths = JSON.stringify({
    outDir: project.configuration?.outDir,
    meteo: project.configuration?.meteo?.path,
    dem: project.configuration?.scene?.demFile,
    objects: (project.configuration?.objects?.items || []).map((item) => [item.fileName, item.positionFile]),
    spectra: (project.configuration?.spectra || []).map((item) => [item.fileName, item.physicalTexture?.fileName, item.classTexture?.fileName, item.opennessTexture?.fileName])
  })
  const meteoPath = String(project.configuration?.meteo?.path || "")
  const meteoPathRepaired = /\/[A-Za-z]:/.test(meteoPath)
  const sceneMigrated = ensureDefaultSceneAsset(project, dirname(inputPath))
  if (meteoPathRepaired) project.configuration.meteo.path = "defined/meteo.txt"
  normalizeStoredProjectPaths(project, dirname(inputPath))
  const normalizedPaths = JSON.stringify({
    outDir: project.configuration?.outDir,
    meteo: project.configuration?.meteo?.path,
    dem: project.configuration?.scene?.demFile,
    objects: (project.configuration?.objects?.items || []).map((item) => [item.fileName, item.positionFile]),
    spectra: (project.configuration?.spectra || []).map((item) => [item.fileName, item.physicalTexture?.fileName, item.classTexture?.fileName, item.opennessTexture?.fileName])
  })
  if (!sceneMigrated && !meteoPathRepaired && originalPaths === normalizedPaths) return project
  return writeProject(projectJsonPath(inputPath), project)
}

// 作用：打开工程：解析路径 → 读取 → 迁移 → 记为当前工程（projectFile/projectDir）。
// 输出：前端所需的路径（Windows 形式）、工程内容、是否统一格式、USD 是否被外部改动。
function openProject(value) {
  const resolvedFile = resolveProjectFile(value)
  const stored = readProject(resolvedFile)
  const migrated = migrateProject(resolvedFile, stored.project)
  projectFile = resolvedFile
  projectDir = dirname(resolvedFile)
  return {
    path: windowsPath(resolvedFile),
    projectDir: windowsPath(dirname(resolvedFile)),
    content: stringifyProject(migrated || stored.project),
    project: migrated || stored.project,
    projectPath: windowsPath(stored.projectPath),
    unified: Boolean(stored.unified),
    usdChangedExternally: usdChangedExternally(dirname(resolvedFile))
  }
}

// 由 Electron 主进程注入原生“打开文件”对话框（函数或 null）；未注入时 chooseProjectFile 退回 PowerShell 对话框。
export function setProjectFileChooser(chooser) {
  if (chooser !== null && typeof chooser !== 'function') throw new TypeError('Project file chooser must be a function or null')
  projectFileChooser = chooser
}

// 作用：弹出文件选择框选择 project.json 或 OpenUSD 文件，返回所选路径（取消返回空串）。
// 说明：非 Windows 平台通过 WSL 调用 Windows PowerShell；对话框在 -STA 线程运行（WinForms 要求）。
function chooseProjectFile() {
  if (projectFileChooser) return projectFileChooser(projectFile)
  if (process.platform !== 'win32' && !existsSync('/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe')) throw new Error('找不到 Windows 文件选择器')
  return new Promise((resolveChoice, reject) => {
    const script = [
      "$ErrorActionPreference = 'Stop'",
      "Add-Type -AssemblyName System.Windows.Forms",
      "$dialog = New-Object System.Windows.Forms.OpenFileDialog",
      "$dialog.Title = '打开 Stream Studio 工程'",
      "$dialog.Filter = 'Stream Studio 工程或 OpenUSD (project.json;*.usd;*.usda;*.usdc;*.usdz)|project.json;*.usd;*.usda;*.usdc;*.usdz|Stream Studio 工程 (project.json)|project.json|OpenUSD (*.usd;*.usda;*.usdc;*.usdz)|*.usd;*.usda;*.usdc;*.usdz'",
      "$dialog.FileName = 'project.json'",
      "$dialog.Multiselect = $false",
      "$dialog.CheckFileExists = $true",
      "if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $dialog.FileName }"
    ].join('; ')
    execFile('powershell.exe', ['-NoProfile', '-STA', '-Command', script], {
      windowsHide: false, encoding: 'utf8', maxBuffer: 1024 * 1024
    }, (error, stdout, stderr) => {
      if (error) return reject(new Error(stderr.trim() || error.message))
      resolveChoice(String(stdout || '').trim())
    })
  })
}

// 模块 9b：USD 同步记录。工程目录下 usd/sync.json 记录最近由本软件写出或导入的 USD 文件摘要，
// 用于判断 project.usd 是否被外部工具改动（见 usdChangedExternally / usdNeedsBackup）。
const USD_EXTENSIONS = new Set(['.usd', '.usda', '.usdc', '.usdz'])
const isUsdPath = (path) => USD_EXTENSIONS.has(extname(String(path || '')).toLowerCase())

// 整文件读入并计算 SHA-256（十六进制）；USD 文件较小，直接同步读取。
function fileSha256(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex')
}

// usd/sync.json records the digest of every USD file Stream Studio last wrote or imported in a project folder.
function usdSyncPath(directory) { return join(directory, 'usd', 'sync.json') }

// 读取同步记录；不存在或损坏时返回 { files: {} }。files 键为小写文件名。
function readUsdSync(directory) {
  try { return { files: {}, ...JSON.parse(readFileSync(usdSyncPath(directory), 'utf8')) } } catch { return { files: {} } }
}

// 作用：登记某 USD 文件的摘要。imported=true 表示用户原始文件（导入而来），在被本软件覆盖前保留在 imported 中，
//       以便覆盖前自动备份；否则从 imported 中移除。
// Imported files are user originals: they stay listed under `imported` until Stream Studio overwrites them.
function recordUsdSync(directory, usdPath, digest, imported = false) {
  const sync = readUsdSync(directory)
  const name = basename(usdPath).toLowerCase()
  sync.files[name] = digest
  sync.imported = { ...(sync.imported || {}) }
  if (imported) sync.imported[name] = digest
  else delete sync.imported[name]
  mkdirSync(dirname(usdSyncPath(directory)), { recursive: true })
  writeFileSync(usdSyncPath(directory), `${JSON.stringify(sync, null, 2)}\n`, 'utf8')
}

// 作用：判断 <directory>/<name> 自上次由本程序写出/导入之后是否被外部工具改过。
// 依据：usd/sync.json 里记录的 SHA-256；没有记录（从未同步过）时返回 false。
function usdChangedExternally(directory, name = 'project.usd') {
  const path = join(directory, name)
  const recorded = readUsdSync(directory).files[name.toLowerCase()]
  return Boolean(recorded && existsSync(path) && fileSha256(path) !== recorded)
}

// 作用：覆盖 project.usd 之前是否需要先备份。以下任一情况视为“内容来自外部”，需要备份：
//   ① 没有同步记录；② 当前哈希与记录不一致（被外部改过）；
//   ③ 记录的哈希是“导入”时登记的（该 USD 是用户提供的，不是本程序导出的）。
function usdNeedsBackup(directory, name = 'project.usd') {
  const path = join(directory, name)
  if (!existsSync(path)) return false
  const sync = readUsdSync(directory)
  const recorded = sync.files[name.toLowerCase()]
  return !recorded || fileSha256(path) !== recorded || sync.imported?.[name.toLowerCase()] === recorded
}

// 模块：工程 → USD 导出。每次保存都把工程镜像为同目录的 project.usd。
// 输入：projectPath（project.json 路径）、project（已规范化的工程）。
// 输出：{ path, externalBackup?, prototypes, instances, skippedObjects, elapsedMs }。
// 副作用：写 project.usd 和 usd/ 资源目录；覆盖前如检测到外部内容则复制为
// project.external-<时间戳>.usd；最后把新文件哈希登记到 usd/sync.json。
// Every save mirrors the project into project.usd so other tools always see the current scene.
function exportProjectUsdFiles(projectPath, project) {
  const directory = dirname(projectPath)
  const outputPath = join(directory, 'project.usd')
  const result = { path: windowsPath(outputPath) }
  if (usdNeedsBackup(directory)) {
    const backup = join(directory, `project.external-${new Date().toISOString().replace(/[:.]/g, '-')}.usd`)
    copyFileSync(outputPath, backup)
    result.externalBackup = windowsPath(backup)
  }
  const startedAt = Date.now()
  const stats = exportProjectUsd(project, {
    outputPath,
    assetsDirectory: join(directory, 'usd'),
    resolveAssetPath: (value) => projectAssetPath(value, directory),
    heightAt: demHeightSampler(project.configuration, directory),
    sourceProjectFile: projectPath
  })
  recordUsdSync(directory, outputPath, fileSha256(outputPath))
  return { ...result, prototypes: stats.prototypes, instances: stats.instances, skippedObjects: stats.skippedObjects, elapsedMs: Date.now() - startedAt }
}

// 模块：打开 USD 文件。输入 USD 路径与（新建工程时使用的）计算模式 mode。
// 步骤 1：若同目录已有 project.json 且该 USD 的哈希与同步记录一致，说明 USD 就是本程序
//         写出/导入的版本，直接打开工程，不重复导入。
// 步骤 2：否则调用 histream-usd.exe 把网格导出为 OBJ 并生成 usd-import.json（runUsdReader）。
// 步骤 3：以已有工程（或清单中内嵌的工程、或默认工程）为底，按清单生成对象与位置文件，
//         得到新工程；已有工程先备份为 project.before-usd-import-<时间戳>.json。
// 步骤 4：写入 project.json 并把 USD 哈希登记为“已导入”，再按普通工程打开。
// 失败处理：写入工程之前出错会删除本次创建的 models/positions 导入目录；之后不删（已被引用）。
// Opening a USD file opens (or creates) project.json beside it; a scene Stream Studio did not write is imported.
async function openUsdProject(value, mode) {
  const source = resolve(normalizeHostPath(value).replace(/^"|"$/g, ''))
  if (!existsSync(source) || !statSync(source).isFile()) throw new Error(`找不到 USD 文件：${windowsPath(source)}`)
  const directory = dirname(source)
  const projectPath = join(directory, 'project.json')
  // 步骤 1：用文件哈希判断这个 USD 是否已与 project.json 同步。
  const digest = fileSha256(source)
  const hasProject = existsSync(projectPath)
  if (hasProject && readUsdSync(directory).files[basename(source).toLowerCase()] === digest) {
    return { ...openProject(projectPath), usdSource: windowsPath(source) }
  }
  const stamp = new Date().toISOString().replace(/[:.]/g, '-')
  // 导入产物放在带时间戳的新子目录，避免覆盖用户已有的 models/positions 文件。
  const objectDirectory = join(directory, 'models', `usd-import-${stamp}`)
  const positionDirectory = join(directory, 'positions', `usd-import-${stamp}`)
  let projectCommitted = false
  try {
    // 步骤 2：运行外部 USD 读取工具，取得导入清单（usd-import.json）。
    const manifest = JSON.parse(readFileSync(await runUsdReader(source, objectDirectory), 'utf8'))
    let base = hasProject ? readProject(projectPath).project : null
    if (!base) {
      try { base = manifest.histream?.project ? normalizeProject(JSON.parse(manifest.histream.project)) : null } catch { base = null }
      base = base || createDefaultProject({ mode, name: basename(directory) })
      base.configuration.outDir = 'output'
    }
    mkdirSync(positionDirectory, { recursive: true })
    // 步骤 3：位置文件名需按对象名清洗非法字符并保证不重名（重名时追加 _2、_3 …，不区分大小写）。
    const usedStems = new Set()
    const { project: imported, stats } = buildProjectFromUsd(base, manifest, {
      heightAt: demHeightSampler(base.configuration, directory),
      writePositionFile: (name, text) => {
        const stemBase = String(name).replace(/[\\/:*?"<>|\s]+/g, '_').replace(/^_+|_+$/g, '') || 'object'
        let stem = stemBase
        for (let suffix = 2; usedStems.has(stem.toLowerCase()); suffix += 1) stem = `${stemBase}_${suffix}`
        usedStems.add(stem.toLowerCase())
        const file = join(positionDirectory, `${stem}.txt`)
        writeFileSync(file, text, 'utf8')
        return file
      }
    })
    // 步骤 4：没有任何网格对象时视为失败，不改动工程；成功才备份并写入 project.json。
    if (!stats.objects) throw new Error('USD 场景中没有可导入的网格对象')
    if (hasProject) copyFileSync(projectPath, join(directory, `project.before-usd-import-${stamp}.json`))
    writeProject(projectPath, imported)
    projectCommitted = true
    recordUsdSync(directory, source, digest, true)
    return { ...openProject(projectPath), usdSource: windowsPath(source), usdImport: { createdProject: !hasProject, ...stats } }
  } catch (error) {
    // Once project.json refers to these assets, later receipt/open failures must preserve them.
    if (!projectCommitted) {
      for (const created of [objectDirectory, positionDirectory]) {
        try { if (existsSync(created)) rmSync(created, { recursive: true, force: true }) } catch {}
      }
    }
    throw error
  }
}

// 作用：“打开工程”入口的分流：.usd 系列文件走 USD 导入流程，其余按 project.json 打开。
function openProjectOrUsd(value, mode) {
  if (isUsdPath(value)) return openUsdProject(value, mode)
  // STREAM 影像模拟工程独立：三维“打开工程”只打开三维 project.json。
  return openProject(value)
}

// 模块：调用 histream-usd.exe read 读取 USD。输出：usd-import.json 的路径（Promise）。
// 约定：工具在 stdout 里以 `HISTREAM_USD<TAB>` 开头的一行给出清单路径；超时 30 分钟。
// histream-usd.exe ships next to histream.exe and writes OBJ files plus usd-import.json.
function runUsdReader(usdPath, outputDirectory) {
  const tool = join(dirname(executable()), 'histream-usd.exe')
  if (!existsSync(tool)) return Promise.reject(new Error(`找不到 OpenUSD 读取工具：${windowsPath(tool)}`))
  return new Promise((resolveManifest, reject) => {
    execFile(tool, ['read', usdPath, outputDirectory], {
      cwd: dirname(tool), windowsHide: true, encoding: 'utf8', timeout: 30 * 60 * 1000, maxBuffer: 16 * 1024 * 1024
    }, (error, stdout, stderr) => {
      const prefix = 'HISTREAM_USD\t'
      const line = String(stdout || '').split(/\r?\n/).find((item) => item.startsWith(prefix))
      if (error || !line) return reject(new Error(`读取 USD 失败：${String(stderr || error?.message || '').trim() || '工具无输出'}`))
      resolveManifest(line.slice(prefix.length).trim())
    })
  })
}

// 作用：新建工程。输入 data：directory（保存目录，须为绝对路径）、name、mode、场景尺寸/体元大小。
// 输出：{ path, projectDir, projectPath, project, content }。
// 副作用：创建 <directory>/<name>/ 及 models、positions 等标准子目录，写 project.json；
// 输出目录 outDir 设为绝对路径 <工程>/output。目标目录已存在且非空时拒绝。
function createProject(data) {
  const directory = normalizeHostPath(data.directory).replace(/^"|"$/g, '')
  const name = String(data.name || '').trim()
  if (!directory) throw new Error('请输入工程保存目录')
  if (!isAbsolute(directory)) throw new Error('工程保存目录必须是绝对路径')
  if (!name || name === '.' || name === '..' || /[<>:"/\\|?*\x00-\x1f]/.test(name)) throw new Error('工程名称包含无效字符')
  if (!modes.has(data.mode)) throw new Error('不支持的计算模式')

  const target = join(resolve(directory), name)
  if (existsSync(target) && readdirSync(target).length) throw new Error(`工程目录已存在且不为空：${target}`)
  mkdirSync(target, { recursive: true })
  for (const folder of ['models', 'positions', 'spectral', 'meteorology', 'terrain', 'images', 'output']) mkdirSync(join(target, folder), { recursive: true })

  const project = createDefaultProject({
    name,
    mode: data.mode,
    sceneX: data.sceneX,
    sceneY: data.sceneY,
    sceneHeight: data.sceneHeight,
    voxelSize: data.voxelSize
  })
  ensureDefaultSceneAsset(project, target)
  project.configuration.outDir = join(target, 'output')
  const jsonPath = join(target, 'project.json')
  const saved = writeProject(jsonPath, project)
  const content = stringifyProject(saved)
  return { path: jsonPath, projectDir: target, projectPath: jsonPath, project: saved, content }
}

// 作用：另存为。把当前工程连同输入资源复制到新目录，并重写其中的路径。
// 步骤 1：校验目录/名称，新目录不得位于当前工程目录内部（避免递归复制）。
// 步骤 2：校验并规范化 data.project，清空输出目录设置为相对的 output，重置创建/更新时间。
// 步骤 3：复制 models/positions/spectral/meteorology/terrain/images（不复制 output）。
// 步骤 4：rebasePath 重写路径：位于被复制目录内的改为相对路径，其余（含 ../ 外部输入）
//         保持指向原文件的绝对路径；同时处理 STREAM 场景路径与共享 DEM 路径。
// 步骤 5：以原统一工程文档为 previous 写出新工程，使 STREAM 部分也一起保留。
function saveProjectAs(data) {
  const directory = normalizeHostPath(data.directory).replace(/^"|"$/g, '')
  const name = String(data.name || '').trim()
  if (!directory) throw new Error('请输入工程保存目录')
  if (!isAbsolute(directory)) throw new Error('工程保存目录必须是绝对路径')
  if (!name || name === '.' || name === '..' || /[<>:"/\\|?*\x00-\x1f]/.test(name)) throw new Error('工程名称包含无效字符')

  const sourceFile = resolveProjectFile(data.sourcePath || projectFile)
  const sourceDirectory = dirname(sourceFile)
  const sourceDocument = existingProjectDocument(sourceFile)
  const target = join(resolve(directory), name)
  // relative 结果以 .. 开头或为绝对路径时说明 target 在源目录之外；为空则是同一目录，同样拒绝。
  const targetWithinSource = relative(sourceDirectory, target)
  if (!targetWithinSource || (!targetWithinSource.startsWith('..\\') && !targetWithinSource.startsWith('../') && !isAbsolute(targetWithinSource))) {
    throw new Error('新工程不能保存在当前工程目录内部')
  }
  if (existsSync(target)) {
    if (!statSync(target).isDirectory()) throw new Error(`目标路径不是目录：${target}`)
    if (readdirSync(target).length) throw new Error(`工程目录已存在且不为空：${target}`)
  }

  const validation = validateProject(data.project)
  if (!validation.valid) throw new Error(validation.errors.join('；'))
  const project = normalizeStoredProjectPaths(JSON.parse(JSON.stringify(validation.project)), sourceDirectory)
  const now = new Date().toISOString()
  project.name = name
  project.createdAt = now
  project.updatedAt = now
  project.configuration.outDir = 'output'

  mkdirSync(target, { recursive: true })
  const copiedFolders = ['models', 'positions', 'spectral', 'meteorology', 'terrain', 'images']
  for (const folder of copiedFolders) {
    const source = join(sourceDirectory, folder)
    const destination = join(target, folder)
    if (existsSync(source) && statSync(source).isDirectory()) cpSync(source, destination, { recursive: true, errorOnExist: true })
    else mkdirSync(destination, { recursive: true })
  }
  mkdirSync(join(target, 'output'), { recursive: true })

  const jsonPath = join(target, 'project.json')
  // Save As starts with both engines. Paths outside the copied folders must
  // keep pointing to their original inputs, including ../relative inputs.
  const previous = isUnifiedProject(sourceDocument) ? normalizeUnified(sourceDocument) : null
  // kind === 'output' 的路径指向输出目录：目录本身映射为新工程根下的 output。
  const rebasePath = (value, kind) => {
    if (!String(value || '').trim()) return ''
    const absolute = resolve(sourceDirectory, normalizeHostPath(value))
    const local = relative(sourceDirectory, absolute)
    const folder = local.split(/[\\/]/)[0].toLowerCase()
    if (!local) return windowsPath(kind === 'output' ? target : sourceDirectory)
    if (copiedFolders.includes(folder) || (kind === 'output' && folder === 'output')) return local.replaceAll('\\', '/')
    return windowsPath(absolute)
  }
  mapHistreamInputPaths(project.configuration, rebasePath)
  if (previous) {
    mapStreamScenePaths(previous.engines.stream, rebasePath)
    // DEM belongs to the shared section; rebase both representations so an
    // unchanged multi-band DEM also keeps its STREAM band on the next merge.
    previous.shared.location.dem.file = rebasePath(previous.shared.location.dem.file)
  }
  const saved = writeProject(jsonPath, project, previous)
  const content = stringifyProject(saved)
  return { path: jsonPath, projectDir: target, projectPath: jsonPath, project: saved, content }
}

// 示例工程目录：<资源根>/assets/examples，由 catalog.json 描述各示例及其 project 路径。
const EXAMPLES_ROOT = join(RESOURCE_ROOT, 'assets', 'examples')

// 返回示例条目对应的工程文件绝对路径；条目无 project 或文件缺失时返回空串（即不列出）。
function exampleSourceFile(entry) {
  return entry?.project && existsSync(resolve(RESOURCE_ROOT, entry.project)) ? resolve(RESOURCE_ROOT, entry.project) : ''
}

// 作用：读取 catalog.json，返回前端展示用的示例清单（只含元数据，缺省值：分类“其他案例”、tier featured）。
function listExamples() {
  const catalogPath = join(EXAMPLES_ROOT, 'catalog.json')
  if (!existsSync(catalogPath)) return []
  const catalog = JSON.parse(readFileSync(catalogPath, 'utf8').replace(/^\uFEFF/, ''))
  return (catalog.examples || [])
    .filter((entry) => exampleSourceFile(entry))
    .map(({ id, label, recommendedMode, focus, category, categoryLabel, tier, note }) => ({
      id, label, recommendedMode: recommendedMode || '', focus: focus || [], category: category || 'other',
      categoryLabel: categoryLabel || '其他案例', tier: tier || 'featured', note: note || ''
    }))
}

function defaultExampleDirectory() {
  return windowsPath(join(homedir(), 'Documents', 'Stream Studio', 'examples'))
}

// 作用：把内置示例复制到用户目录后打开（示例本身只读，运行结果写到副本的 output/）。
// 输入 data：id、directory（缺省为 文档/Stream Studio/examples）。输出：openProject 的结果 + overwritten。
// 同名副本再次打开时就地覆盖示例自带文件，但保留 output/ 与其他用户文件；
// 示例若是 STREAM 工程则由 streamService 打开，否则按三维工程打开。
function openExample(data) {
  const entry = JSON.parse(readFileSync(join(EXAMPLES_ROOT, 'catalog.json'), 'utf8').replace(/^\uFEFF/, ''))
    .examples?.find((item) => item.id === data.id && item.project)
  if (!entry) throw new Error(`示例不存在：${data.id}`)
  const sourceFile = resolve(RESOURCE_ROOT, entry.project)
  const sourceDirectory = dirname(sourceFile)
  if (relative(EXAMPLES_ROOT, sourceDirectory).startsWith('..')) throw new Error('示例路径无效')
  const directory = normalizeHostPath(data.directory || defaultExampleDirectory()).replace(/^"|"$/g, '')
  if (!isAbsolute(directory)) throw new Error('示例保存目录必须是绝对路径')
  const folder = basename(sourceDirectory)
  const target = join(resolve(directory), folder)
  // Opening the same example again overwrites that copy in place instead of piling up
  // <name>_2, <name>_3 folders. Only the files the example ships are replaced; anything else
  // in the folder — notably output/ with earlier runs — is left as it is. Runs write into
  // output/, so an example is never opened in place.
  const overwritten = existsSync(target) && (statSync(target).isFile() || readdirSync(target).length > 0)
  if (overwritten) {
    if (statSync(target).isFile()) rmSync(target, { force: true })
    else {
      for (const entry of readdirSync(sourceDirectory, { withFileTypes: true })) {
        if (entry.name === 'output') continue
        rmSync(join(target, entry.name), { recursive: true, force: true })
      }
    }
  }
  cpSync(sourceDirectory, target, { recursive: true, filter: (path) => relative(sourceDirectory, path).split(/[\\/]/)[0] !== 'output' })
  mkdirSync(join(target, 'output'), { recursive: true })
  const copiedProject = join(target, basename(sourceFile))
  const streamProject = streamService.openProjectFile(copiedProject)
  const opened = streamProject ? { path: streamProject.path, streamProject } : openProject(copiedProject)
  return { ...opened, overwritten }
}

// 作用：把资源路径解析为绝对路径；相对路径相对于当前工程目录 projectDir。
function resolveAsset(value) {
  const source = normalizeHostPath(value).replace(/^"|"$/g, '')
  if (!source) throw new Error('资源路径为空')
  return isAbsolute(source) ? resolve(source) : resolve(projectDir, source)
}

// 作用：读取 <directory>/run-manifest.json，仅当它确属该目录时返回（否则 null）：
// kind 须为已知运行类型，且 outputDir 解析后等于该目录，或无 outputDir 时 runId 等于目录名。
function resultRunManifest(directory) {
  try {
    const manifest = JSON.parse(readFileSync(join(directory, 'run-manifest.json'), 'utf8'))
    if (!['histream-run', 'stream-image-run', 'stream-scene-run'].includes(manifest?.kind)) return null
    if (manifest.outputDir && sameResolvedPath(directory, normalizeHostPath(manifest.outputDir))) return manifest
    if (!manifest.outputDir && manifest.runId && basename(directory) === manifest.runId) return manifest
  } catch {}
  return null
}

// 作用：计算“删除结果”时需要保护的范围：当前工程文件、各类输入文件及输入目录，
// 以及可删除的输出根目录 outputRoot。无工程时全部为空。
function resultDeletionScope() {
  if (!projectFile) return { outputRoot: '', inputs: [], inputDirectories: [] }
  const { project, unified } = readProject(projectFile)
  const base = dirname(projectFile)
  const inputs = [projectFile]
  const collect = (value, kind) => {
    if (kind !== 'output' && String(value || '').trim()) inputs.push(resolve(base, normalizeHostPath(value)))
    return value
  }
  mapHistreamInputPaths(project?.configuration, collect)
  if (unified?.engines?.stream) mapStreamScenePaths(unified.engines.stream, collect)
  return {
    outputRoot: project ? resolve(base, normalizeHostPath(project.configuration.outDir || 'output')) : '',
    inputs,
    inputDirectories: ['models', 'positions', 'spectral', 'meteorology', 'terrain', 'images', 'textures'].map(name => join(base, name))
  }
}

// 作用：判断一个目录能否被当作结果目录删除。输入保护优先：与任何输入文件/输入目录相交即拒绝；
// 正在运行的引擎输出目录也拒绝。允许：工程输出根、带有效 run-manifest 的目录，
// 或其 runs/ 下含有效运行目录的目录（runs 为符号链接则拒绝）。
// Only output roots and identified runs are deletable; project inputs always take precedence.
function resultDirectoryAllowed(directory, scope = resultDeletionScope()) {
  if (scope.inputs.some(path => realPathWithin(directory, path))
      || scope.inputDirectories.some(path => realPathWithin(path, directory))) return false
  if (child?.histreamRun?.outputDir && sameResolvedPath(directory, normalizeHostPath(child.histreamRun.outputDir))) return false
  if (scope.outputRoot && sameResolvedPath(scope.outputRoot, directory)) return true
  if (resultRunManifest(directory)) return true
  try {
    const runs = join(directory, 'runs')
    if (lstatSync(runs).isSymbolicLink()) return false
    for (const entry of readdirSync(runs, { withFileTypes: true })) {
      if (!entry.isDirectory() || entry.isSymbolicLink()) continue
      if (resultRunManifest(join(runs, entry.name))) return true
    }
  } catch {}
  return false
}

// CSV 单元格：始终加双引号，内部双引号按 RFC 4180 转义为两个双引号。
function csvCell(value) {
  return `"${String(value ?? '').replaceAll('"', '""')}"`
}

// 统计 CSV 中的模式标签：引擎模式枚举 → 文件名/列值使用的短名。
const STATISTICS_MODE_LABELS = { eFacetRT: 'FacetRT', eFacetEB: 'FacetEB', eVoxelRT: 'VoxelRT', eVoxelEB: 'VoxelEB', eRaytracing: 'Raytracing' }

// 模块：为刚导出的 TIFF 逐波段计算统计量，并写入同目录 result_statistics_<mode>.csv。
// 输入：values 为波段顺序（BSQ）排列的数组，下标 = band * width * height + pixel；
//       solar 含 solarZenith/solarAzimuth（度，可缺省）。非有限值（NaN/Inf）不计入统计。
// 输出：CSV 路径。每行：文件、模式、时间 T、VZA、VAA、SZA、SAA、波段号/名、count、
//       min、max、mean、stddev（总体标准差，除以 count）。
// 说明：均值与方差用 Welford 在线算法，避免大影像上的累加误差；
//       模式标签优先取 engineMode，否则由文件名推断（_f/_v 表示面元/体元，T= 开头表示时间序列）；
//       同名文件重新导出时替换其旧行，写临时文件后改名，避免留下半个 CSV。
function writeRasterStatistics(path, width, height, bands, values, bandNames = [], solar = {}, engineMode = '') {
  const name = basename(path)
  const isTimeSeries = name.startsWith('T=')
  // Atmosphere-corrected products end in _f_a / _v_a.
  // 无 engineMode 时的兜底：文件名以 _f / _v（或带大气校正的 _f_a / _v_a）结尾判断面元/体元。
  const geometry = /_(f|v)(?:_a)?\./i.exec(name)?.[1]?.toLowerCase()
  const mode = STATISTICS_MODE_LABELS[engineMode] || (geometry === 'f' ? (isTimeSeries ? 'FacetEB' : 'FacetRT')
    : geometry === 'v' ? (isTimeSeries ? 'VoxelEB' : 'VoxelRT') : 'Raytracing')
  const statisticsPath = join(dirname(path), `result_statistics_${mode.toLowerCase()}.csv`)
  const solarCell = (value) => Number.isFinite(value) ? value : ''
  const pixelCount = width * height
  const rows = []
  for (let band = 0; band < bands; band += 1) {
    let count = 0, mean = 0, moment = 0, minimum = Infinity, maximum = -Infinity
    const offset = band * pixelCount
    for (let pixel = 0; pixel < pixelCount; pixel += 1) {
      const value = Number(values[offset + pixel])
      if (!Number.isFinite(value)) continue
      count += 1
      const delta = value - mean
      mean += delta / count
      moment += delta * (value - mean)
      minimum = Math.min(minimum, value); maximum = Math.max(maximum, value)
    }
    rows.push([
      csvCell(name), mode, csvCell(filenameToken(name, 'T')), filenameToken(name, 'VZA'), filenameToken(name, 'VAA'),
      solarCell(solar.solarZenith), solarCell(solar.solarAzimuth),
      band, csvCell(bandNames[band] || `Band ${band + 1}`), count,
      count ? minimum : '', count ? maximum : '', count ? mean : '', count ? Math.sqrt(moment / count) : ''
    ].join(','))
  }
  // Re-exporting a TIFF replaces its band rows instead of duplicating the
  // same observation in the statistics table.
  // 表头不一致（旧格式或损坏）时丢弃旧内容重建，避免列错位。
  const header = 'file,mode,time,vza,vaa,sza,saa,band_index,band_name,count,min,max,mean,stddev'
  const prefix = csvCell(name) + ','
  const lines = existsSync(statisticsPath) ? readFileSync(statisticsPath, 'utf8').trimEnd().split(/\r?\n/) : []
  const existing = lines[0] === header ? lines.slice(1) : []
  writeFileSync(statisticsPath + '.tmp', header + '\n' +
    [...existing.filter(line => line && !line.startsWith(prefix)), ...rows].join('\n') + '\n', 'utf8')
  renameSync(statisticsPath + '.tmp', statisticsPath)
  return statisticsPath
}

// 模块：写多波段 float32 TIFF（经典 TIFF，小端，单条带，像素交错），并同时更新统计 CSV。
// 输入：values 为波段顺序（BSQ）数组 values[band * width * height + pixel]；
//       solar 若给出有限的天顶角/方位角（度）则写入 GDAL_METADATA 标签，供 GDAL/rasterio 读取。
// 输出：最终 TIFF 路径。非有限值统一写成 NaN。
// 步骤 1：校验尺寸与数据长度；步骤 2：规划 IFD 条目数与额外数据区（allocate 负责对齐）；
// 步骤 3：写文件头与 IFD；步骤 4：分块（64 KiB）把 BSQ 转为交错并写入 .partial；
// 步骤 5：完整写完后再改名发布，失败则删除临时文件；最后写统计 CSV。
// 限制：偏移为 32 位，文件不得超过 4 GiB。
function writeFloatTiff(path, width, height, bands, values, bandNames = [], solar = {}, mode = '') {
  if (![width, height, bands].every((value) => Number.isInteger(value) && value > 0)) throw new Error('TIFF 尺寸或波段数无效')
  const pixelCount = width * height
  if (!values || values.length < pixelCount * bands) throw new Error('TIFF 波段数据不完整')

  const hasSolar = Number.isFinite(solar.solarZenith) && Number.isFinite(solar.solarAzimuth)
  // IFD 条目数：基础 12 个；多波段多一个 ExtraSamples(338)；有太阳角时多一个 GDAL_METADATA。
  const entries = (bands > 1 ? 13 : 12) + (hasSolar ? 1 : 0)
  const ifdOffset = 8
  let dataOffset = ifdOffset + 2 + entries * 12 + 4
  const extraData = []
  const allocate = (buffer, alignment = 2) => {
    const padding = (alignment - dataOffset % alignment) % alignment
    if (padding) { extraData.push({ offset: dataOffset, buffer: Buffer.alloc(padding) }); dataOffset += padding }
    const offset = dataOffset
    extraData.push({ offset, buffer })
    dataOffset += buffer.length
    return offset
  }
  // SHORT 数组不超过 2 个时直接内联在条目的 4 字节值域里，否则写到额外数据区并返回偏移。
  const shortArrayValue = (items) => {
    if (items.length <= 2) return items.reduce((value, item, index) => value | ((item & 0xffff) << (index * 16)), 0) >>> 0
    const buffer = Buffer.alloc(items.length * 2)
    items.forEach((item, index) => buffer.writeUInt16LE(item, index * 2))
    return allocate(buffer)
  }
  // 每波段 32 位、SampleFormat=3（IEEE 浮点）；ExtraSamples 全 0（未指定），避免被当作 alpha 通道。
  const bits = Array(bands).fill(32)
  const sampleFormats = Array(bands).fill(3)
  const extraSamples = Array(Math.max(0, bands - 1)).fill(0)
  const bitsValue = shortArrayValue(bits)
  const sampleFormatsValue = shortArrayValue(sampleFormats)
  const extraSamplesValue = extraSamples.length ? shortArrayValue(extraSamples) : 0
  const names = Array.from({ length: bands }, (_, index) => String(bandNames[index] || 'Band ' + (index + 1)))
  // ImageDescription 里保存波段名列表，读取端（readTiff）据此恢复波段名。
  const description = Buffer.from('HiStream image bands: {' + names.join(', ') + '}\0', 'ascii')
  const descriptionOffset = allocate(description, 1)
  // GDAL_METADATA (tag 42112) is read by GDAL/rasterio as dataset metadata.
  const gdalMetadata = hasSolar ? Buffer.from('<GDALMetadata>' +
    `<Item name="HISTREAM_SOLAR_ZENITH_DEG">${solar.solarZenith}</Item>` +
    `<Item name="HISTREAM_SOLAR_AZIMUTH_DEG">${solar.solarAzimuth}</Item>` +
    '</GDALMetadata>\0', 'ascii') : null
  const gdalMetadataOffset = gdalMetadata ? allocate(gdalMetadata, 2) : 0
  // 影像数据起点按 4 字节对齐，便于按 float32 读取。
  const imagePadding = (4 - dataOffset % 4) % 4
  if (imagePadding) { extraData.push({ offset: dataOffset, buffer: Buffer.alloc(imagePadding) }); dataOffset += imagePadding }
  const imageOffset = dataOffset
  const imageBytes = pixelCount * bands * 4
  if (!Number.isSafeInteger(imageBytes) || imageOffset + imageBytes > 0xffffffff) throw new Error('TIFF 超过 4 GiB，请降低单幅影像分辨率或波段数')

  // 步骤 3：文件头（'II' 小端标记、魔数 42、IFD 偏移）+ IFD；条目必须按 tag 升序排列（TIFF 规范）。
  const header = Buffer.alloc(imageOffset)
  header.write('II', 0, 'ascii'); header.writeUInt16LE(42, 2); header.writeUInt32LE(ifdOffset, 4)
  header.writeUInt16LE(entries, ifdOffset)
  let entry = ifdOffset + 2
  const writeEntry = (tag, type, count, value) => {
    header.writeUInt16LE(tag, entry); header.writeUInt16LE(type, entry + 2); header.writeUInt32LE(count, entry + 4); header.writeUInt32LE(value >>> 0, entry + 8)
    entry += 12
  }
  // 条目含义：256 宽、257 高、258 位深、259 压缩=1（无）、262 光度=1（BlackIsZero）、
  // 270 描述、273 条带偏移、277 每像素样本数、278 每条带行数（=整幅一条带）、
  // 279 条带字节数、284 平面配置=1（交错）、338 额外样本、339 样本格式、42112 GDAL 元数据。
  writeEntry(256, 4, 1, width); writeEntry(257, 4, 1, height)
  writeEntry(258, 3, bands, bitsValue); writeEntry(259, 3, 1, 1); writeEntry(262, 3, 1, 1)
  writeEntry(270, 2, description.length, descriptionOffset)
  writeEntry(273, 4, 1, imageOffset); writeEntry(277, 3, 1, bands); writeEntry(278, 4, 1, height)
  writeEntry(279, 4, 1, imageBytes); writeEntry(284, 3, 1, 1)
  if (bands > 1) writeEntry(338, 3, extraSamples.length, extraSamplesValue)
  writeEntry(339, 3, bands, sampleFormatsValue)
  if (gdalMetadata) writeEntry(42112, 2, gdalMetadata.length, gdalMetadataOffset)
  header.writeUInt32LE(0, entry)
  for (const part of extraData) part.buffer.copy(header, part.offset)
  // 步骤 4：BSQ → 像素交错：输出序号 k 对应 像素 k / bands、波段 k % bands，内存占用与影像大小无关。
  // No full-image interleaved Buffer or Buffer.concat copy. Publish only a
  // complete TIFF; bounded chunks keep memory independent of file size.
  const temporaryPath = path + '.partial'
  const fd = openSync(temporaryPath, 'w')
  let writeFailed = false
  try {
    const writeAll = buffer => {
      let offset = 0
      while (offset < buffer.length) {
        const written = writeSync(fd, buffer, offset, buffer.length - offset)
        if (!written) throw new Error('TIFF 写入未完成')
        offset += written
      }
    }
    writeAll(header)
    const chunk = Buffer.allocUnsafe(Math.min(imageBytes, 64 * 1024))
    for (let first = 0; first < pixelCount * bands;) {
      const count = Math.min(chunk.length / 4, pixelCount * bands - first)
      for (let index = 0; index < count; index++) {
        const outputIndex = first + index
        const value = Number(values[(outputIndex % bands) * pixelCount + Math.floor(outputIndex / bands)])
        chunk.writeFloatLE(Number.isFinite(value) ? value : Number.NaN, index * 4)
      }
      writeAll(chunk.subarray(0, count * 4))
      first += count
    }
  } catch (error) {
    writeFailed = true
    throw error
  } finally {
    try { closeSync(fd) } catch {}
    if (writeFailed) { try { unlinkSync(temporaryPath) } catch {} }
  }
  // 步骤 5：用 realpath 规范化后再改名，避免目录为符号链接/联接点时 rename 出错；改名即发布。
  renameSync(realpathSync(temporaryPath), join(realpathSync(dirname(path)), basename(path)))
  writeRasterStatistics(path, width, height, bands, values, bandNames, solar, mode)
  return path
}

// 作用：从结果文件名中取出 `KEY=值` 形式的字段（如 T、VZA、VAA、SZA、SAA、H），不区分大小写。
// 值到下一个分隔标记（_H= _T= _TIME= _SZA= _SAA= _VZA= _VAA=，或 _F/_V/_A 后缀）为止；
// 找不到字段时返回空串。
function filenameToken(stem, key) {
  const source = String(stem || '')
  const marker = String(key || '').toUpperCase() + '='
  const upper = source.toUpperCase()
  const start = upper.indexOf(marker)
  if (start < 0) return ''
  const value = source.slice(start + marker.length)
  const boundaries = ['_H=', '_T=', '_TIME=', '_SZA=', '_SAA=', '_VZA=', '_VAA=', '_F', '_V', '_A']
  let end = value.length
  for (const boundary of boundaries) {
    const index = value.toUpperCase().indexOf(boundary)
    if (index >= 0) end = Math.min(end, index)
  }
  return value.slice(0, end)
}

// 模块：把一对 ENVI 文件（.hdr + .img）转成标准命名的 float32 TIFF。
// 支持范围：data type = 4（float32）、小端、BSQ、header offset = 0；其余一律抛出带原因的错误。
// 步骤 1：解析头文件并校验尺寸、数据类型、交错方式与 .img 大小。
// 步骤 2：读入全部数据；波段名取自 band names。
// 步骤 3：由文件名里的 T/VZA/VAA/SZA/SAA 生成标准 TIFF 名（能量平衡模式用时间 + 观测角，
//         辐射模式用太阳角 + 观测角）；字段不全则沿用原文件名并补几何后缀。
// 步骤 4：写 TIFF；Voxel 与 Raytracing 模式转换后删除 ENVI 原文件（面元模式保留，原因待核对）。
// Throws a descriptive Error when the ENVI pair cannot be converted; the caller
// (convertEnviResults) records it as a visible per-file failure.
function convertEnviToTiff(headerPath, mode) {
  const metadata = enviMetadata(readFileSync(headerPath, 'utf8'))
  const width = Number(metadata.samples), height = Number(metadata.lines), bands = Number(metadata.bands)
  const type = Number(metadata['data type'] || 4)
  const imagePath = headerPath.slice(0, -extname(headerPath).length) + '.img'
  if (![width, height, bands].every((value) => Number.isInteger(value) && value > 0)) throw new Error(`头文件尺寸无效（samples=${metadata.samples}, lines=${metadata.lines}, bands=${metadata.bands}）`)
  if (type !== 4) throw new Error(`不支持的数据类型 ${type}（只支持 4 = float32）`)
  if (!existsSync(imagePath)) throw new Error('缺少对应的 .img 数据文件')
  if (Number(metadata['byte order'] ?? 0) !== 0) throw new Error('只支持小端字节序（byte order = 0）')
  if (String(metadata.interleave || 'bsq').trim().toLowerCase() !== 'bsq') throw new Error(`只支持 BSQ 交错方式，实际为 ${metadata.interleave}`)
  if (Number(metadata['header offset'] || 0) !== 0) throw new Error('只支持 header offset = 0')
  // 步骤 2：一次性读入 .img 并逐个 float32 解码；注意这里整幅数据都在内存中。
  const source = readFileSync(imagePath)
  const expected = width * height * bands * 4
  if (source.length < expected) throw new Error(`.img 数据不完整：${source.length} 字节，应为 ${expected} 字节`)
  const values = new Float32Array(width * height * bands)
  for (let index = 0; index < values.length; index += 1) values[index] = source.readFloatLE(index * 4)
  const names = String(metadata['band names'] || '').replace(/[{}]/g, '').split(',').map((value) => value.trim()).filter(Boolean)
  const stem = basename(headerPath, extname(headerPath))
  const julianTime = Number(filenameToken(stem, 'T'))
  const vza = filenameToken(stem, 'VZA')
  const vaa = filenameToken(stem, 'VAA')
  const sza = filenameToken(stem, 'SZA')
  const saa = filenameToken(stem, 'SAA')
  // 步骤 3：确定输出文件名（见函数头说明）。
  let standardName = ''
  if (isEnergyBalanceMode(mode) && Number.isFinite(julianTime) && vza !== '' && vaa !== '') {
    standardName = outputTiffName(mode, { julianTime, vza, vaa })
  } else if (!isEnergyBalanceMode(mode) && sza !== '' && saa !== '' && vza !== '' && vaa !== '') {
    standardName = outputTiffName(mode, { sza, saa, vza, vaa })
  }
  if (!standardName) {
    const suffix = outputGeometrySuffix(mode)
    standardName = stem.endsWith(suffix) ? stem + '.tif' : stem + suffix + '.tif'
  }
  const tiffPath = join(dirname(headerPath), standardName)
  writeFloatTiff(tiffPath, width, height, bands, values, names,
    sza !== '' && saa !== '' ? { solarZenith: Number(sza), solarAzimuth: Number(saa) } : {}, mode)
  if (mode === "eVoxelRT" || mode === "eVoxelEB" || mode === "eRaytracing") {
    unlinkSync(imagePath)
    unlinkSync(headerPath)
  }
  return tiffPath
}

// 模块：转换目录下所有符合当前模式的 ENVI 头文件。
// 筛选：能量平衡模式只转文件名带 T= 的头文件，辐射模式只转不带 T= 的，避免两类产物混淆。
// Returns { converted: [tiffPath…], failures: [{ file, reason }…] }.  Failures
// used to be filtered out silently, leaving a "completed" run without images.
function convertEnviResults(directory, mode) {
  const converted = [], failures = []
  if (!existsSync(directory) || !statSync(directory).isDirectory()) return { converted, failures }
  const energyBalance = isEnergyBalanceMode(mode)
  const headers = readdirSync(directory, { withFileTypes: true })
    .filter((entry) => entry.isFile() && extname(entry.name).toLowerCase() === '.hdr')
    .filter((entry) => {
      const stem = basename(entry.name, extname(entry.name))
      const hasTime = filenameToken(stem, 'T') !== ''
      return energyBalance ? hasTime : !hasTime
    })
  for (const entry of headers) {
    try { converted.push(convertEnviToTiff(join(directory, entry.name), mode)) }
    catch (error) { failures.push({ file: entry.name, reason: error.message }) }
  }
  return { converted, failures }
}

// 作用：从 TIFF 文件名解析观测条件，供前端结果列表显示。
// 输出：输出高度 H、模拟时间 T（或 TIME）、太阳天顶/方位角、观测天顶/方位角（均为文件名中的原文字符串，
//       度）及合成标签 observationLabel（各项以 “ · ” 连接）。
function tiffObservation(name) {
  const stem = basename(name, extname(name))
  const outputHeight = filenameToken(stem, 'H')
  const time = filenameToken(stem, 'T') || filenameToken(stem, 'TIME')
  const sza = filenameToken(stem, 'SZA')
  const saa = filenameToken(stem, 'SAA')
  const vza = filenameToken(stem, 'VZA')
  const vaa = filenameToken(stem, 'VAA')
  const parts = [
    outputHeight && 'H=' + outputHeight,
    time && 'T=' + time,
    sza && 'SZA=' + sza + '°',
    saa && 'SAA=' + saa + '°',
    vza && 'VZA=' + vza + '°',
    vaa && 'VAA=' + vaa + '°'
  ].filter(Boolean)
  return {
    outputHeight,
    simulationTime: time,
    solarZenith: sza,
    solarAzimuth: saa,
    viewZenith: vza,
    viewAzimuth: vaa,
    observationLabel: parts.join(' · ')
  }
}

// 作用：列出某输出目录下“属于结果”的所有目录：自身、其 process/ 子目录、以及 runs/<runId>（递归）。
// 约束：不跟随符号链接/联接点；与输入文件相交的目录排除；正在运行的输出目录跳过；
//       仅由子运行识别出的容器目录不贡献其顶层无关文件。
// Same scope as listResults: the directory, its process/ folder and every
// runs/<runId> beneath it (symbolic links and junctions are not followed).
function resultDirectories(directory, scope = resultDeletionScope()) {
  if (!resultDirectoryAllowed(directory, scope)) return []
  const processDirectory = join(directory, 'process')
  // A container recognized only by child runs must not donate its unrelated top-level files.
  const ownsResults = Boolean(resultRunManifest(directory)) || (scope.outputRoot && sameResolvedPath(scope.outputRoot, directory))
  const directories = ownsResults ? [directory] : []
  if (ownsResults && existsSync(processDirectory) && !lstatSync(processDirectory).isSymbolicLink()
      && statSync(processDirectory).isDirectory() && realPathWithin(directory, processDirectory)
      && !scope.inputs.some(path => realPathWithin(processDirectory, path))) directories.push(processDirectory)
  const runsDirectory = join(directory, 'runs')
  if (existsSync(runsDirectory) && !lstatSync(runsDirectory).isSymbolicLink() && statSync(runsDirectory).isDirectory()) {
    const activeRun = child?.histreamRun?.outputDir ? resolve(normalizeHostPath(child.histreamRun.outputDir)) : ''
    for (const entry of readdirSync(runsDirectory, { withFileTypes: true })) {
      if (!entry.isDirectory() || entry.isSymbolicLink()) continue
      const runDirectory = join(runsDirectory, entry.name)
      if (activeRun && sameResolvedPath(runDirectory, activeRun)) continue
      directories.push(...resultDirectories(runDirectory, scope))
    }
  }
  return directories
}

// 作用：求“可删除的结果产物”文件集合（运行清单 products 里列出且位于结果目录中的文件）。
// 安全策略（保守）：
//   1. 只接受清单中的相对路径，且必须在本运行目录内（realPathWithin）；
//   2. 从清单中的源输入、运行时输入、气象文件，及运行目录里的工程快照
//      （source-project.json、input/project.json、run-config.json、scene-config.json）
//      收集全部输入文件，产物若与输入重合则不删；
//   3. 找不到任何输入快照时，宁可整个运行都不删（inputs 置为 null）；
//   4. 快照读取/解析失败直接抛错，不能因为来源不明而授权删除。
function resultProductsForDeletion(directory, scope) {
  const products = new Set()
  const inputsByRun = new Map()
  for (const current of resultDirectories(directory, scope)) {
    const owner = resultRunManifest(current) ? current : dirname(current)
    const manifest = resultRunManifest(owner)
    const listed = manifest?.completed?.products || manifest?.products
    if (!Array.isArray(listed)) continue
    if (!inputsByRun.has(owner)) {
      const inputs = [...scope.inputs]
      for (const identity of [manifest.sourceInput, manifest.runtimeInput, manifest.meteorology, manifest.meteorology?.snapshot]) {
        if (identity?.path) inputs.push(resolve(normalizeHostPath(identity.path)))
      }
      const sourceBase = manifest.sourceInput?.path ? dirname(resolve(normalizeHostPath(manifest.sourceInput.path))) : owner
      const snapshots = [
        [join(owner, 'source-project.json'), sourceBase],
        [join(owner, 'input', 'project.json'), join(owner, 'input')],
        [join(owner, 'run-config.json'), owner],
        [join(owner, 'scene-config.json'), owner]
      ]
      let inputSnapshotFound = false
      for (const [path, base] of snapshots) {
        if (!existsSync(path)) continue
        // Unreadable input provenance must never authorize a destructive operation.
        const document = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
        const collect = (value, kind) => {
          if (kind !== 'output' && String(value || '').trim()) inputs.push(resolve(base, normalizeHostPath(value)))
          return value
        }
        if (isUnifiedProject(document)) {
          const unified = normalizeUnified(document)
          mapHistreamInputPaths(unifiedToHistreamProject(unified).configuration, collect)
          if (unified.engines?.stream) mapStreamScenePaths(unified.engines.stream, collect)
          inputSnapshotFound = true
        } else if (document.configuration) {
          mapHistreamInputPaths(document.configuration, collect)
          inputSnapshotFound = true
        } else if (document.surface || document.inputs) {
          mapStreamScenePaths({ ...document, surface: document.surface || document.inputs }, collect)
          inputSnapshotFound = true
        }
      }
      inputsByRun.set(owner, inputSnapshotFound ? inputs : null)
    }
    const inputs = inputsByRun.get(owner)
    if (!inputs) continue
    for (const name of listed) {
      if (typeof name !== 'string' || isAbsolute(normalizeHostPath(name))) continue
      const path = resolve(owner, normalizeHostPath(name))
      if (!sameResolvedPath(dirname(path), current) || !realPathWithin(owner, path)) continue
      if (!existsSync(path) || !lstatSync(path).isFile()) continue
      if (inputs.some(input => sameResolvedPath(path, input))) continue
      products.add(path)
    }
  }
  return products
}

// 作用：删除指定结果目录中的 TIFF 影像及 result_statistics*.csv 统计表。
// 安全：先用 resultDirectoryAllowed 校验目录，再只取 resultProductsForDeletion 认定的产物；
//       拒绝磁盘根目录。输出：{ directory, deleted: 删除文件数 }。
function deleteTiffResults(value) {
  const directory = resolveAsset(value)
  if (!existsSync(directory) || !statSync(directory).isDirectory()) throw new Error(`找不到模拟结果目录：` + directory)
  if (dirname(directory) === directory) throw new Error(`不能删除磁盘根目录中的结果`)
  const scope = resultDeletionScope()
  if (!resultDirectoryAllowed(directory, scope)) throw new Error(`只能删除不含工程输入的输出目录或带运行清单的结果目录：` + windowsPath(directory))
  const paths = [...resultProductsForDeletion(directory, scope)]
    .filter(path => ['.tif', '.tiff'].includes(extname(path).toLowerCase()) || /^result_statistics(?:_[a-z0-9-]+)?\.csv$/i.test(basename(path)))
  for (const path of paths) unlinkSync(path)
  return { directory: windowsPath(directory), deleted: paths.length }
}

// 判断 .json 是否为三维结果元数据：面元/体元辐射结果文件，或带 kind 的过程结果。
function isThreeDimensionalResultFile(path) {
  if (extname(path).toLowerCase() !== ".json") return false
  if (isRadiosityResultFile(path)) return true
  return processResultMetadata(path) != null
}

// 作用：删除三维结果（面元/体元 JSON 与过程结果元数据）及其引用的数据文件 dataFile。
// dataFile 只在与元数据同目录、且本身属于可删除产物时才一并删除。校验流程同 deleteTiffResults。
function deleteThreeDimensionalResults(value) {
  const directory = resolveAsset(value)
  if (!existsSync(directory) || !statSync(directory).isDirectory()) throw new Error("找不到模拟结果目录：" + directory)
  if (dirname(directory) === directory) throw new Error("不能删除磁盘根目录中的结果")
  const scope = resultDeletionScope()
  if (!resultDirectoryAllowed(directory, scope)) throw new Error("只能删除不含工程输入的输出目录或带运行清单的结果目录：" + windowsPath(directory))
  const products = resultProductsForDeletion(directory, scope)
  const metadataPaths = [...products].filter(isThreeDimensionalResultFile)
  const paths = [...metadataPaths]
  for (const metadataPath of metadataPaths) {
    const metadata = processResultMetadata(metadataPath)
    if (!metadata?.dataFile) continue
    const dataPath = resolve(dirname(metadataPath), String(metadata.dataFile))
    if (dirname(dataPath) === dirname(metadataPath) && products.has(dataPath)) paths.push(dataPath)
  }
  for (const path of paths) unlinkSync(path)
  return { directory: windowsPath(directory), deleted: paths.length }
}

// 作用：取得某结果文件生成时的观测设置（用于历史结果作图）。
// 优先级：run-manifest.json 的 requested.observation → input/project.json 或 source-project.json 快照
//       （由光源方位角、传感器投影推出 planeAzimuth/principalPlane/hemisphere）→ 空对象。
// 透视投影（perspective/central/center）下不使用主平面与半球设置。
// Historical plots use the run's frozen settings, never the currently open
// project. Older isolated runs already contain the required project snapshot.
function resultObservationContext(resultPath) {
  const directory = dirname(resultPath)
  try {
    const manifest = JSON.parse(readFileSync(join(directory, 'run-manifest.json'), 'utf8'))
    if (manifest.requested?.observation) return manifest.requested.observation
  } catch {}
  for (const snapshot of [join(directory, 'input', 'project.json'), join(directory, 'source-project.json')]) {
    try {
      const { configuration } = JSON.parse(readFileSync(snapshot, 'utf8'))
      const { light = {}, sensor = {} } = configuration || {}
      if (light.azimuth == null || !Number.isFinite(Number(light.azimuth))) continue
      const perspective = ['perspective', 'central', 'center'].includes(String(sensor.projection))
      return {
        planeAzimuth: Number(light.azimuth),
        principalPlane: !perspective && Boolean(sensor.principalPlane),
        hemisphere: !perspective && Boolean(sensor.hemisphere),
        projection: sensor.projection
      }
    } catch {}
  }
  return {}
}

// 模块：列出结果目录中前端可展示的文件。输入：结果目录；输出：{ directory, files[] }。
// 步骤 1：收集本目录与 process/ 下的 .tif/.tiff/.json/.csv，读取运行清单（run-manifest.json）；
//         没有 runId 的旧版 FacetEB 运行改读 faceteb_run.json 的产物清单做过滤。
// 步骤 2：逐文件分类 kind：tiff / text（统计 CSV）/ facet（面元结果 JSON）/ process（过程结果）/ file，
//         并生成显示名、观测条件（tiffObservation）、过程模型等；kind 为 file 的丢弃。
// 步骤 3：若存在面元过程时间序列，则不再单独列出共享的 faceteb.json（避免重复的“最终时刻”）。
// 步骤 4：递归汇总 runs/<runId> 下的结果，合并后按修改时间降序返回，路径转成 Windows 形式。
function listResults(value) {
  const directory = resolveAsset(value)
  if (!existsSync(directory) || !statSync(directory).isDirectory()) throw new Error('找不到模拟结果目录：' + directory)
  const processDirectory = join(directory, 'process')
  const directories = [directory, ...(existsSync(processDirectory) && statSync(processDirectory).isDirectory() ? [processDirectory] : [])]
  let runMetadata = {}
  try { runMetadata = JSON.parse(readFileSync(join(directory, 'run-manifest.json'), 'utf8')) } catch {}
  let nativeFiles = null
  if (!runMetadata.runId) {
    try {
      const native = JSON.parse(readFileSync(join(directory, 'faceteb_run.json'), 'utf8'))
      nativeFiles = new Set(native.products)
      runMetadata.status = native.status
    } catch {}
  }
  // 步骤 2：逐文件分类并生成列表条目。
  const files = directories.flatMap((currentDirectory) => readdirSync(currentDirectory, { withFileTypes: true })
    .filter((entry) => entry.isFile())
    .filter((entry) => ['.tif', '.tiff', '.json', '.csv'].includes(extname(entry.name).toLowerCase()))
    .filter((entry) => !nativeFiles || nativeFiles.has(relative(directory, join(currentDirectory, entry.name)).replaceAll('\\', '/')))
    .map((entry) => {
      const path = join(currentDirectory, entry.name)
      const info = statSync(path)
      const extension = extname(entry.name).toLowerCase()
      let kind = ['.tif', '.tiff'].includes(extension) ? 'tiff' : extension === '.csv' ? 'text' : extension === '.json' && isRadiosityResultFile(path) ? 'facet' : 'file'
      let process = {}
      if (extension === '.json') {
        process = processResultMetadata(path) || {}
        if (process.kind) kind = 'process'
      }
      const observation = kind === 'tiff' ? tiffObservation(entry.name) : {}
      const resultType = kind === 'tiff' && /^fluid_wind_/i.test(entry.name) ? 'wind' : ''
      const inferredProcessModel = process.processType === 'fluid'
        ? 'voxelfluid'
        : process.processType === 'radiation'
          ? (process.geometry === 'voxel' ? 'voxelrt' : 'facetrt')
          : (process.geometry === 'voxel' ? 'voxeleb' : 'faceteb')
      // 兼容旧结果：旧流体过程曾错误写作 voxeleb，列表中仍按流体过程命名。
      const processModel = String(process.processType === 'fluid'
        ? inferredProcessModel : (process.model || inferredProcessModel)).toLowerCase()
      const processModelLabels = {
        facetrt: '面元辐射传输', voxelrt: '体元辐射传输',
        faceteb: '面元能量平衡', voxeleb: '体元能量平衡',
        voxelfluid: '体元流体力学'
      }
      const facetModelLabel = basename(path).toLowerCase().includes('faceteb')
        ? '面元能量平衡' : '面元辐射传输'
      const displayName = kind === 'text' && /^result_statistics(?:_[a-z0-9-]+)?\.csv$/i.test(entry.name)
        ? '统计结果 · ' + entry.name
        : kind === 'facet'
        ? facetModelLabel + ' · ' + (facetModelLabel.includes('能量平衡') ? '最终时刻' : '静态')
        : kind === 'process'
          ? (process.processType === 'photovoltaic' ? '光伏热电耦合' : (processModelLabels[processModel] || processModel)) + ' · ' + String(process.time || '静态')
          : entry.name
      return { name: displayName, path, size: info.size, modifiedAt: info.mtime.toISOString(), kind, resultType, ...observation, processTime: process.time, processType: process.processType, processModel, node: process.node,
        runId: runMetadata.runId || '', runStatus: runMetadata.status || '', converged: process.converged }
    }))
    .filter((entry) => entry.kind !== 'file')
    .sort((a, b) => b.modifiedAt.localeCompare(a.modifiedAt))
  // 步骤 3：判断是否存在面元过程时间序列。
  const hasFacetTimeSeries = files.some((entry) =>
    entry.kind === 'process' && ['facetrt', 'faceteb'].includes(entry.processModel) && entry.processTime)
  // faceteb.json is the shared full-resolution geometry and final-state
  // backing store for all per-time FacetRT/FacetEB process files. When the
  // time series exists, listing it as another "final time" is redundant.
  const latestFacet = files.find((entry) => entry.kind === 'facet' && !(
    hasFacetTimeSeries && basename(entry.path).toLowerCase() === 'faceteb.json'))
  const runsDirectory = join(directory, 'runs')
  // 步骤 4：递归列出 runs/<runId>（不跟随符号链接）。
  const nested = existsSync(runsDirectory) ? readdirSync(runsDirectory, { withFileTypes: true })
    .filter(entry => entry.isDirectory() && !entry.isSymbolicLink())
    .flatMap(entry => listResults(join(runsDirectory, entry.name)).files) : []
  return {
    directory: windowsPath(directory),
    files: [
      ...files.filter((entry) => entry.kind === 'tiff'),
      ...files.filter((entry) => entry.kind === 'text'),
      ...files.filter((entry) => entry.kind === 'process'),
      ...nested,
      ...(latestFacet ? [latestFacet] : [])
    ].sort((a, b) => b.modifiedAt.localeCompare(a.modifiedAt))
      .map((entry) => ({ ...entry, path: windowsPath(entry.path) }))
  }
}

// 作用：解析 ENVI .hdr 的 `key = value` 行，键转小写，值去首尾空白。
// 说明：多行花括号块（如 band names）只取同一行内容；`=` 后无空白且值以 { 开头时不会匹配（待核对）。
function enviMetadata(content) {
  const result = {}
  for (const match of content.matchAll(/^\s*([^=\r\n]+?)\s*=\s*([^\r\n{][^\r\n]*)/gm)) result[match[1].trim().toLowerCase()] = match[2].trim()
  return result
}

// 作用：按 ENVI data type 读取一个样本：1=uint8、2=int16、3=int32、4=float32、5=float64、
// 12=uint16、13=uint32；其他类型抛错。
function enviValue(buffer, offset, type, littleEndian) {
  if (type === 1) return buffer.readUInt8(offset)
  if (type === 2) return littleEndian ? buffer.readInt16LE(offset) : buffer.readInt16BE(offset)
  if (type === 3) return littleEndian ? buffer.readInt32LE(offset) : buffer.readInt32BE(offset)
  if (type === 4) return littleEndian ? buffer.readFloatLE(offset) : buffer.readFloatBE(offset)
  if (type === 5) return littleEndian ? buffer.readDoubleLE(offset) : buffer.readDoubleBE(offset)
  if (type === 12) return littleEndian ? buffer.readUInt16LE(offset) : buffer.readUInt16BE(offset)
  if (type === 13) return littleEndian ? buffer.readUInt32LE(offset) : buffer.readUInt32BE(offset)
  throw new Error(`暂不支持 ENVI data type ${type}`)
}

// ENVI data type → 每样本字节数；未知类型返回 0。
function enviTypeSize(type) {
  return ({ 1: 1, 2: 2, 3: 4, 4: 4, 5: 8, 12: 2, 13: 4 })[type] || 0
}

// TIFF 字段类型 → 每个值的字节数（1 BYTE、2 ASCII、3 SHORT、4 LONG、5 RATIONAL … 12 DOUBLE）。
function tiffTypeSize(type) {
  return ({ 1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8 })[type] || 0
}

// 模块：TIFF LZW 解压（MSB 优先位序，码长 9–12 位，256 = Clear，257 = EOI）。
// 输入：压缩条带/分块字节；输出：解压后的 Buffer。字典无效时抛错。
function unpackTiffLzw(source) {
  let bitOffset = 0
  let codeSize = 9
  let nextCode = 258
  let previous = null
  const dictionary = []
  const output = []
  const readCode = () => {
    if (bitOffset + codeSize > source.length * 8) return null
    let code = 0
    for (let bit = 0; bit < codeSize; bit += 1) code = (code << 1) | ((source[Math.floor((bitOffset + bit) / 8)] >> (7 - ((bitOffset + bit) % 8))) & 1)
    bitOffset += codeSize
    return code
  }
  const reset = () => { codeSize = 9; nextCode = 258; previous = null; dictionary.length = 0 }
  while (true) {
    const code = readCode()
    if (code == null || code === 257) break
    if (code === 256) { reset(); continue }
    let entry
    if (code < 256) entry = [code]
    else if (code < nextCode && dictionary[code]) entry = dictionary[code]
    else if (code === nextCode && previous) entry = previous.concat(previous[0])
    else throw new Error('TIFF LZW 字典无效')
    output.push(...entry)
    if (previous) {
      dictionary[nextCode] = previous.concat(entry[0])
      nextCode += 1
      // TIFF LZW switches code width one code early (unlike GIF).
      if (nextCode + 1 >= (1 << codeSize) && codeSize < 12) codeSize += 1
    }
    previous = entry
  }
  return Buffer.from(output)
}

// 模块：还原 TIFF predictor（解压后原地修改 decoded）。
// 参数：rowSamples = 每行样本数（含各波段）；stride = 每像素样本数（波段数）；format=3 表示浮点。
// predictor 2：水平差分，对整型样本按 stride 向前累加（8/16/32 位，模 2^bits）。
// predictor 3：浮点预测器（TIFF Technical Note 3）：先按字节做 stride 差分累加，
// 再把每行“按字节平面（高位在前）”存放的数据重排回逐样本字节顺序。
function undoTiffPredictor(decoded, { predictor, bits, format, rowSamples, stride, littleEndian }) {
  const sampleBytes = bits / 8
  const rowBytes = rowSamples * sampleBytes
  const rows = Math.floor(decoded.length / rowBytes)
  if (predictor === 2) {
    if (format === 3 || ![8, 16, 32].includes(bits)) throw new Error(`TIFF predictor 2 不支持 ${bits} 位${format === 3 ? '浮点' : ''}样本`)
    for (let row = 0; row < rows; row += 1) {
      const rowStart = row * rowBytes
      for (let index = stride; index < rowSamples; index += 1) {
        const offset = rowStart + index * sampleBytes
        const previous = offset - stride * sampleBytes
        if (bits === 8) decoded[offset] = (decoded[offset] + decoded[previous]) & 0xff
        else if (bits === 16) {
          const sum = ((littleEndian ? decoded.readUInt16LE(offset) : decoded.readUInt16BE(offset))
            + (littleEndian ? decoded.readUInt16LE(previous) : decoded.readUInt16BE(previous))) & 0xffff
          if (littleEndian) decoded.writeUInt16LE(sum, offset); else decoded.writeUInt16BE(sum, offset)
        } else {
          const sum = ((littleEndian ? decoded.readUInt32LE(offset) : decoded.readUInt32BE(offset))
            + (littleEndian ? decoded.readUInt32LE(previous) : decoded.readUInt32BE(previous))) >>> 0
          if (littleEndian) decoded.writeUInt32LE(sum, offset); else decoded.writeUInt32BE(sum, offset)
        }
      }
    }
    return decoded
  }
  if (predictor === 3) {
    // TIFF Technical Note 3: bytes are differenced with the pixel stride, and
    // each row stores its samples as byte planes, most significant first.
    if (format !== 3 || ![32, 64].includes(bits)) throw new Error(`TIFF predictor 3 仅适用于 32/64 位浮点样本`)
    const plane = Buffer.alloc(rowBytes)
    for (let row = 0; row < rows; row += 1) {
      const rowStart = row * rowBytes
      for (let offset = rowStart + stride; offset < rowStart + rowBytes; offset += 1) decoded[offset] = (decoded[offset] + decoded[offset - stride]) & 0xff
      decoded.copy(plane, 0, rowStart, rowStart + rowBytes)
      for (let sample = 0; sample < rowSamples; sample += 1) {
        for (let byte = 0; byte < sampleBytes; byte += 1) {
          const significance = littleEndian ? sampleBytes - 1 - byte : byte
          decoded[rowStart + sample * sampleBytes + byte] = plane[significance * rowSamples + sample]
        }
      }
    }
    return decoded
  }
  throw new Error(`暂不支持 TIFF predictor ${predictor}`)
}

// 作用：取有效样本的 2%–98% 分位数作为显示拉伸范围（线性插值分位数）。
// 说明：会对 samples 的前 count 项原地排序（调用方的缓冲区被重排）；
//       无有效样本或范围退化（max <= min）时返回给定的后备范围。
function twoPercentStretch(samples, count, fallbackMinimum, fallbackMaximum) {
  if (count < 1) return { minimum: fallbackMinimum, maximum: fallbackMaximum }
  const sorted = samples.subarray(0, count)
  sorted.sort()
  const percentile = (fraction) => {
    const position = (count - 1) * fraction
    const lower = Math.floor(position)
    const upper = Math.ceil(position)
    if (lower === upper) return sorted[lower]
    const weight = position - lower
    return sorted[lower] * (1 - weight) + sorted[upper] * weight
  }
  const minimum = percentile(.02)
  const maximum = percentile(.98)
  return Number.isFinite(minimum) && Number.isFinite(maximum) && maximum > minimum
    ? { minimum, maximum }
    : { minimum: fallbackMinimum, maximum: fallbackMaximum }
}

// 作用：热红外（温度）影像的显示拉伸：有限场景地平线附近常出现“无效 0 值 / 地表温度 /
// 较暖且很窄的天空温度平台”三群数据，直接用 2–98% 分位会把地表结构压到少数灰阶。
// 做法：零值占比 ≥ 1% 且有效样本 ≥ 64 时，在排序后 30%–92% 位置内找最大相邻间隙作为
// 地表/天空分界；间隙须 ≥ max(2, 0.12 × P10–P90 范围)（单位同样本，温度影像即 K，待核对），
// 否则退回普通分位拉伸（mode: 'percentile'）。
// 输出：{ minimum, maximum, mode, excludedMinimum?, excludedCeiling? }；
//       thermal-surface 模式只对地表群拉伸，天空群范围由 excluded* 给出供叠加显示。
// 原始数值范围不受影响，只改变显示灰阶。
function thermalSurfaceStretch(samples, count, zeroCount, totalCount, fallbackMinimum, fallbackMaximum) {
  const ordinary = twoPercentStretch(samples, count, fallbackMinimum, fallbackMaximum)
  if (count < 64 || zeroCount / Math.max(1, totalCount) < .01) return { ...ordinary, mode: 'percentile' }

  // A finite-scene horizon commonly contains three populations: invalid zero,
  // surface temperature and a narrow, warmer sky-temperature plateau. Locate
  // the strong surface/sky gap so surface structure is not compressed into a
  // few grey levels. The raw range and high-temperature overlay stay intact.
  const sorted = samples.subarray(0, count)
  sorted.sort()
  const first = Math.floor(count * .30)
  const last = Math.min(count - 2, Math.floor(count * .92))
  let split = -1
  let largestGap = 0
  for (let index = first; index <= last; index += 1) {
    const gap = sorted[index + 1] - sorted[index]
    if (gap > largestGap) { largestGap = gap; split = index }
  }
  const centralRange = sorted[Math.floor((count - 1) * .90)] - sorted[Math.floor((count - 1) * .10)]
  if (split < 0 || largestGap < Math.max(2, centralRange * .12)) {
    return { ...ordinary, mode: 'percentile' }
  }
  const surfaceCount = split + 1
  const upperCount = count - surfaceCount
  const detail = twoPercentStretch(sorted, surfaceCount, sorted[0], sorted[split])
  const upper98 = sorted[surfaceCount + Math.floor(Math.max(0, upperCount - 1) * .98)]
  return {
    ...detail,
    mode: 'thermal-surface',
    excludedMinimum: sorted[surfaceCount],
    excludedCeiling: upper98
  }
}

// 作用：把数值线性映射到 0–255 灰阶（inverted 时反相）；非有限值为 0，范围退化时取中灰。
function stretchedByte(value, minimum, maximum, inverted = false) {
  if (!Number.isFinite(value)) return 0
  const range = maximum - minimum
  const normalized = range > 0 ? (value - minimum) / range : .5
  const displayed = inverted ? 1 - normalized : normalized
  return Math.max(0, Math.min(255, Math.round(displayed * 255)))
}

// 模块：读盘缓存。以文件状态（fileStamp）为键放入 resultCache；读文件前后状态不一致
// （读的过程中被改写）或缓存拒绝时，该次结果不入缓存（key 置空），保证不会缓存到过期内容。
// 同一结果文件的多个波段（彩色合成连读三次）、变化分析与重复打开共用一次读盘和分块解码。
function cachedResultSource(path) {
  const before = fileStamp(path)
  const key = before ? 'source:' + before.key : ''
  const cached = key ? resultCache.get(key) : undefined
  if (cached) return cached
  const source = { key, data: readFileSync(path), chunks: new Map(), decodedBytes: 0 }
  const after = fileStamp(path)
  if (!before || after?.key !== before.key || !resultCache.set(key, source, source.data.length, [before])) source.key = ''
  return source
}

// 模块：TIFF 读取与预览（前端“结果/地形”查看的核心）。
// 输入：path；requestedBand（0 起的波段号，越界时夹到有效范围）；includeValues（是否返回整幅数值）。
// 输出：{ width, height, bands, bandNames, minimum, maximum, 拉伸范围, pixels(base64 灰度预览),
//       地理参考(pixelSize/origin/crs/unit), 热点叠加, terrainValues(<=96 像素的粗网格), values? }。
// 支持：经典 TIFF（II/MM 字节序）、条带或瓦片、压缩 1/5(LZW)/8/32946(Deflate)/32773(PackBits)、
//       predictor 2/3、planar 1/2、位深 1/8/16/32/64、Orientation 1–8；不支持 BigTIFF。
// 步骤 1：解析头与 IFD，读出所有标签；步骤 2：取出尺寸、位深、地理标签等并校验；
// 步骤 3：建立分块解码器（getChunk，解码结果缓存在 cachedSource 中）与取样函数；
// 步骤 4：按 Orientation 统一坐标；遍历全图得到 min/max/均值；
// 步骤 5：降采样（最长边 768）生成灰度预览，按 2–98% 或热红外策略拉伸；
// 步骤 6：热红外波段计算高温热点叠加；步骤 7：整理元数据并降采样（最长边 96）得到地形网格。
function readTiff(path, requestedBand = 0, includeValues = false) {
  const cachedSource = cachedResultSource(path)
  const data = cachedSource.data
  if (data.length < 8) throw new Error('TIFF 文件头不完整')
  const littleEndian = data.toString('ascii', 0, 2) === 'II'
  const bigEndian = data.toString('ascii', 0, 2) === 'MM'
  if (!littleEndian && !bigEndian) throw new Error('不是有效的 TIFF 文件')
  const u16 = (offset) => littleEndian ? data.readUInt16LE(offset) : data.readUInt16BE(offset)
  const u32 = (offset) => littleEndian ? data.readUInt32LE(offset) : data.readUInt32BE(offset)
  if (u16(2) !== 42) throw new Error('暂不支持 BigTIFF 格式，请导出标准 TIFF')
  const ifdOffset = u32(4)
  if (ifdOffset + 2 > data.length) throw new Error('TIFF IFD 偏移无效')
  const entryCount = u16(ifdOffset)
  const tags = new Map()
  // 读取一个 IFD 条目的值：总字节数 <= 4 时值内联在条目里，否则条目给出数据偏移；
  // ASCII 返回字符串，其余返回数值数组（RATIONAL 以 分子/分母 求值，分母 0 时按 1）。
  const readValue = (entryOffset, type, count) => {
    const size = tiffTypeSize(type)
    const total = size * count
    if (!size || !Number.isSafeInteger(total)) throw new Error(`不支持 TIFF 字段类型 ${type}`)
    const valueOffset = total <= 4 ? entryOffset + 8 : (entryOffset + 12 > data.length ? -1 : u32(entryOffset + 8))
    if (valueOffset < 0 || valueOffset + total > data.length) throw new Error('TIFF 字段数据越界')
    const readNumber = (offset) => {
      if (type === 1 || type === 7 || type === 2) return data[offset]
      if (type === 3) return u16(offset)
      if (type === 4) return u32(offset)
      if (type === 5) return u32(offset) / (u32(offset + 4) || 1)
      if (type === 6) return data.readInt8(offset)
      if (type === 8) return littleEndian ? data.readInt16LE(offset) : data.readInt16BE(offset)
      if (type === 9) return littleEndian ? data.readInt32LE(offset) : data.readInt32BE(offset)
      if (type === 10) {
        const denominator = littleEndian ? data.readInt32LE(offset + 4) : data.readInt32BE(offset + 4)
        return (littleEndian ? data.readInt32LE(offset) : data.readInt32BE(offset)) / (denominator || 1)
      }
      if (type === 11) return littleEndian ? data.readFloatLE(offset) : data.readFloatBE(offset)
      if (type === 12) return littleEndian ? data.readDoubleLE(offset) : data.readDoubleBE(offset)
      throw new Error(`不支持 TIFF 字段类型 ${type}`)
    }
    if (type === 2) return data.subarray(valueOffset, valueOffset + total).toString('ascii').replace(/\0+$/, '')
    return Array.from({ length: count }, (_, index) => readNumber(valueOffset + index * size))
  }
  for (let index = 0; index < entryCount; index += 1) {
    const offset = ifdOffset + 2 + index * 12
    if (offset + 12 > data.length) throw new Error('TIFF IFD 不完整')
    tags.set(u16(offset), readValue(offset, u16(offset + 2), u32(offset + 4)))
  }
  // 步骤 2：按 TIFF/GeoTIFF 标签取参数，缺省值遵循规范（压缩 1、光度 1、每像素样本 1 等）。
  //   273/279 条带偏移与字节数，322–325 瓦片，274 Orientation，317 Predictor，
  //   33550 ModelPixelScale，33922 ModelTiepoint，34735/34737 GeoKey 目录与 ASCII，42113 GDAL_NODATA。
  const tagValue = (tag, fallback) => tags.has(tag) ? tags.get(tag) : fallback
  const width = Number(tagValue(256, [0])[0])
  const height = Number(tagValue(257, [0])[0])
  const bitsPerSample = tagValue(258, [8]).map(Number)
  const compression = Number(tagValue(259, [1])[0])
  const photometric = Number(tagValue(262, [1])[0])
  const samples = Math.max(1, Number(tagValue(277, [1])[0]))
  const planar = Number(tagValue(284, [1])[0])
  const sampleFormats = tagValue(339, [1]).map(Number)
  const orientation = Number(tagValue(274, [1])[0])
  const predictor = Number(tagValue(317, [1])[0])
  const noDataText = String(tagValue(42113, '') || '').trim()
  const noData = noDataText && Number.isFinite(Number(noDataText)) ? Number(noDataText) : null
  // FIX(2026-11-24 S11): a float32 raster stores the nodata value as float32; the tag text (e.g. -3.4028235e+38) is a double that
  // is not the same number as the float32 pixel value, so compare through Math.fround or the fill value would not be masked.
  const noDataCompare = noData != null && Number(bitsPerSample[0]) === 32 && Number(sampleFormats[0]) === 3 ? Math.fround(noData) : noData
  const pixelScale = tagValue(33550, []).map(Number)
  const tiePoint = tagValue(33922, []).map(Number)
  const geoKeys = tagValue(34735, []).map(Number)
  const geoAscii = String(tagValue(34737, '') || '').replace(/\|+$/, '').trim()
  // GeoKeyDirectory 每 4 个 short 为一项 [键, 位置, 个数, 值]；只取位置为 0（值内联）的项。
  const geoKey = (key) => {
    for (let index = 4; index + 3 < geoKeys.length; index += 4) {
      if (geoKeys[index] === key && geoKeys[index + 1] === 0) return geoKeys[index + 3]
    }
    return null
  }
  const modelType = geoKey(1024)
  const projectedCode = geoKey(3072)
  const linearUnit = geoKey(3076)
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1) throw new Error('TIFF 缺少有效的宽度或高度')
  if (![1, 2].includes(planar)) throw new Error(`暂不支持 TIFF planar configuration ${planar}`)
  if (![1, 5, 8, 32773, 32946].includes(compression)) throw new Error(`暂不支持 TIFF compression ${compression}`)
  // 瓦片与条带统一成“分块”处理：条带宽度 = 影像宽度，高度取 RowsPerStrip。
  const tiled = tags.has(322) && tags.has(323) && tags.has(324) && tags.has(325)
  const tileWidth = tiled ? Number(tagValue(322, [width])[0]) : width
  const tileHeight = tiled ? Number(tagValue(323, [height])[0]) : Number(tagValue(278, [height])[0])
  const offsets = (tiled ? tagValue(324, []) : tagValue(273, [])).map(Number)
  const byteCounts = (tiled ? tagValue(325, []) : tagValue(279, [])).map(Number)
  if (!offsets.length || offsets.length !== byteCounts.length) throw new Error('TIFF 缺少有效的图像数据分块')
  if (!bitsPerSample.every((bits) => [1, 8, 16, 32, 64].includes(bits))) throw new Error(`暂不支持 TIFF bits per sample ${bitsPerSample.join(',')}`)
  if (!sampleFormats.every((format) => [1, 2, 3].includes(format))) throw new Error(`暂不支持 TIFF sample format ${sampleFormats.join(',')}`)

  // 步骤 3：分块几何。planar=2（每波段独立平面）时，分块序号 = 波段号 × 每平面分块数 + 平面内序号。
  const chunksPerRow = tiled ? Math.ceil(width / tileWidth) : 1
  const chunksPerColumn = Math.ceil(height / tileHeight)
  const chunksPerPlane = chunksPerRow * chunksPerColumn
  const bytesPerSample = Math.max(1, Math.ceil(Math.max(...bitsPerSample) / 8))
  const maxChunkBytes = Math.trunc(tileWidth * tileHeight * samples * bytesPerSample)
  const decodedChunks = cachedSource.chunks
  // 解码并缓存一个分块：先校验偏移越界，再按压缩方式解压（Deflate 限制最大输出长度防止解压炸弹），
  // 再还原 predictor；缓存增长后同步调整 resultCache 里该文件的占用大小。
  const getChunk = (index) => {
    if (decodedChunks.has(index)) return decodedChunks.get(index)
    if (!Number.isInteger(offsets[index]) || !Number.isInteger(byteCounts[index]) || offsets[index] < 0 || byteCounts[index] < 0 || offsets[index] + byteCounts[index] > data.length) throw new Error('TIFF 图像数据分块越界')
    const source = data.subarray(offsets[index], offsets[index] + byteCounts[index])
    let decoded = source
    if (compression === 5) decoded = unpackTiffLzw(source)
    if (compression === 8 || compression === 32946) decoded = Number.isSafeInteger(maxChunkBytes) && maxChunkBytes > 0 ? inflateSync(source, { maxOutputLength: maxChunkBytes }) : inflateSync(source)
    if (compression === 32773) {
      const output = []
      for (let offset = 0; offset < source.length;) {
        const control = source[offset++]
        if (control <= 127) for (let count = 0; count < control + 1 && offset < source.length; count += 1) output.push(source[offset++])
        else if (control >= 129 && offset < source.length) { const value = source[offset++]; for (let count = 0; count < 257 - control; count += 1) output.push(value) }
      }
      decoded = Buffer.from(output)
    }
    if (predictor !== 1) {
      const chunkBand = planar === 2 ? Math.floor(index / chunksPerPlane) : 0
      const bits = Number(bitsPerSample[chunkBand] ?? bitsPerSample[0])
      const format = Number(sampleFormats[chunkBand] ?? sampleFormats[0] ?? 1)
      if (planar !== 2 && bitsPerSample.some((value) => value !== bits)) throw new Error('TIFF predictor 要求各波段位深一致')
      const stride = planar === 2 ? 1 : samples
      if (decoded === source) decoded = Buffer.from(source)
      decoded = undoTiffPredictor(decoded, {
        predictor, bits, format, stride, littleEndian,
        rowSamples: (tiled ? tileWidth : width) * stride
      })
    }
    decodedChunks.set(index, decoded)
    if (decoded !== source && cachedSource.key) {
      cachedSource.decodedBytes += decoded.length
      resultCache.resize(cachedSource.key, data.length + cachedSource.decodedBytes)
    }
    return decoded
  }
  // 从分块按位偏移读取一个样本；format：1 无符号、2 有符号、3 浮点；越界或不支持的组合返回 NaN。
  const readSample = (chunk, bitOffset, bits, format) => {
    if (bits === 1) return (chunk[Math.floor(bitOffset / 8)] >> (7 - (bitOffset % 8))) & 1
    const offset = Math.floor(bitOffset / 8)
    if (offset + bits / 8 > chunk.length) return Number.NaN
    if (bits === 8) return format === 2 ? chunk.readInt8(offset) : chunk[offset]
    if (bits === 16) return format === 2 ? (littleEndian ? chunk.readInt16LE(offset) : chunk.readInt16BE(offset)) : (littleEndian ? chunk.readUInt16LE(offset) : chunk.readUInt16BE(offset))
    if (bits === 32) return format === 3 ? (littleEndian ? chunk.readFloatLE(offset) : chunk.readFloatBE(offset)) : format === 2 ? (littleEndian ? chunk.readInt32LE(offset) : chunk.readInt32BE(offset)) : (littleEndian ? chunk.readUInt32LE(offset) : chunk.readUInt32BE(offset))
    return format === 3 ? (littleEndian ? chunk.readDoubleLE(offset) : chunk.readDoubleBE(offset)) : Number.NaN
  }
  const sourceValueAt = (x, y, band) => {
    const bits = Number(bitsPerSample[band] || bitsPerSample[0])
    const format = Number(sampleFormats[band] || sampleFormats[0] || 1)
    const chunkColumn = tiled ? Math.floor(x / tileWidth) : 0
    const chunkRow = Math.floor(y / tileHeight)
    const chunkInPlane = chunkRow * chunksPerRow + chunkColumn
    const chunkIndex = planar === 2 ? band * chunksPerPlane + chunkInPlane : chunkInPlane
    const chunk = getChunk(chunkIndex)
    const localX = tiled ? x % tileWidth : x
    const localY = y % tileHeight
    const pixelSamples = planar === 2 ? 1 : samples
    const sampleIndex = (localY * (tiled ? tileWidth : width) + localX) * pixelSamples + (planar === 2 ? 0 : band)
    return readSample(chunk, sampleIndex * bits, bits, format)
  }
  // 步骤 4：Orientation 5–8 含转置，输出宽高互换；sourceCoordinates 把输出坐标换算回存储坐标。
  const rotated = [5, 6, 7, 8].includes(orientation)
  const outputWidth = rotated ? height : width
  const outputHeight = rotated ? width : height
  const sourceCoordinates = (x, y) => {
    if (orientation === 2) return [width - 1 - x, y]
    if (orientation === 3) return [width - 1 - x, height - 1 - y]
    if (orientation === 4) return [x, height - 1 - y]
    if (orientation === 5) return [y, x]
    if (orientation === 6) return [y, height - 1 - x]
    if (orientation === 7) return [width - 1 - y, height - 1 - x]
    if (orientation === 8) return [width - 1 - y, x]
    return [x, y]
  }
  const band = Math.max(0, Math.min(samples - 1, Number(requestedBand) || 0))
  // 波段名来源：ImageDescription 中的 `band names/image bands/metrics: {…}` 列表（writeFloatTiff 写入），
  // 否则 3 波段 RGB 用红/绿/蓝，其余用“波段 n”。
  // 热红外判定：波段名含 temperature/热红外/thermal，或名称中 @ 波长 >= 3000 nm。
  const description = typeof tagValue(270, '') === 'string' ? tagValue(270, '') : ''
  const describedBands = description.match(/(?:band names|image bands|metrics)\s*[:=]\s*\{?([^}\r\n]+)\}?/i)?.[1]
  const bandNames = describedBands ? describedBands.split(',').map((value) => value.trim()).filter(Boolean) : samples === 3 && photometric === 2 ? ['红色', '绿色', '蓝色'] : Array.from({ length: samples }, (_, index) => `波段 ${index + 1}`)
  const activeBandName = String(bandNames[band] || '')
  const wavelengthNanometres = Number(activeBandName.match(/@\s*([0-9.]+)\s*nm/i)?.[1])
  const thermalBand = /temperature|热红外|thermal/i.test(activeBandName) || (Number.isFinite(wavelengthNanometres) && wavelengthNanometres >= 3000)
  // 统计整幅有效值（NaN 与 NoData 排除）；elevationSum/Count 用于均值（地形高程影像常用）。
  let minimum = Infinity, maximum = -Infinity, elevationSum = 0, elevationCount = 0
  const values = includeValues ? new Float64Array(outputWidth * outputHeight) : null
  // NoData 比较：float32 影像用 Math.fround 后的 NoData（见上方 FIX 说明），命中则视为缺测 NaN。
  const valueAt = (x, y) => {
    const [sourceX, sourceY] = sourceCoordinates(x, y)
    const value = sourceValueAt(sourceX, sourceY, band)
    return noDataCompare != null && value === noDataCompare ? Number.NaN : value
  }
  for (let y = 0; y < outputHeight; y += 1) for (let x = 0; x < outputWidth; x += 1) {
    const value = valueAt(x, y)
    if (values) values[y * outputWidth + x] = value
    if (Number.isFinite(value)) {
      minimum = Math.min(minimum, value); maximum = Math.max(maximum, value)
      elevationSum += value; elevationCount += 1
    }
  }
  if (!Number.isFinite(minimum)) { minimum = 0; maximum = 0 }
  // 步骤 5：预览降采样（最近邻），最长边不超过 768 像素；不放大。
  const scale = Math.min(1, 768 / Math.max(outputWidth, outputHeight))
  const previewWidth = Math.max(1, Math.round(outputWidth * scale))
  const previewHeight = Math.max(1, Math.round(outputHeight * scale))
  const pixels = Buffer.alloc(previewWidth * previewHeight)
  const previewValues = new Float64Array(previewWidth * previewHeight)
  const finiteSamples = new Float64Array(previewWidth * previewHeight)
  let finiteSampleCount = 0
  let zeroSampleCount = 0
  for (let y = 0; y < previewHeight; y += 1) for (let x = 0; x < previewWidth; x += 1) {
    const value = valueAt(Math.min(outputWidth - 1, Math.floor(x / scale)), Math.min(outputHeight - 1, Math.floor(y / scale)))
    const index = y * previewWidth + x
    previewValues[index] = value
    if (Number.isFinite(value)) {
      if (thermalBand && value <= 0) zeroSampleCount += 1
      else finiteSamples[finiteSampleCount++] = value
    }
  }
  // 热红外波段把 <= 0 的值当作无效（计入 zeroSampleCount），不参与拉伸范围统计。
  const stretch = thermalBand
    ? thermalSurfaceStretch(finiteSamples, finiteSampleCount, zeroSampleCount,
      previewWidth * previewHeight, minimum, maximum)
    : twoPercentStretch(finiteSamples, finiteSampleCount, minimum, maximum)
  for (let index = 0; index < pixels.length; index += 1) {
    pixels[index] = stretchedByte(previewValues[index], stretch.minimum, stretch.maximum, photometric === 0)
  }
  // 步骤 6：热点叠加（仅热红外）。阈值 = max(拉伸上限 + max(2×拉伸范围, 5%×|上限|),
  // 天空平台上限 + max(5%×|上限|, 2))；超过阈值的像素按 sqrt 归一化映射到 48–255，
  // 其余为 0，用于在灰度预览上突出异常高温。
  let hotspotPixels = null
  let hotspotCount = 0
  let hotspotThreshold = null
  if (thermalBand && maximum > stretch.maximum) {
    const stretchRange = Math.max(0, stretch.maximum - stretch.minimum)
    const ordinaryThreshold = stretch.maximum + Math.max(stretchRange * 2, Math.abs(stretch.maximum) * .05, 1e-9)
    const excludedThreshold = Number.isFinite(stretch.excludedCeiling)
      ? stretch.excludedCeiling + Math.max(Math.abs(stretch.excludedCeiling) * .05, 2) : -Infinity
    hotspotThreshold = Math.max(ordinaryThreshold, excludedThreshold)
    if (maximum > hotspotThreshold) {
      hotspotPixels = Buffer.alloc(previewWidth * previewHeight)
      const hotspotRange = maximum - hotspotThreshold
      for (let index = 0; index < hotspotPixels.length; index += 1) {
        const value = previewValues[index]
        if (!Number.isFinite(value) || value <= hotspotThreshold) continue
        const normalized = Math.sqrt(Math.max(0, Math.min(1, (value - hotspotThreshold) / hotspotRange)))
        hotspotPixels[index] = Math.max(48, Math.round(normalized * 255))
        hotspotCount += 1
      }
    }
  }
  // 步骤 7：整理元数据。crs：投影代码有效时为 EPSG:<code>，否则用 GeoAscii 或坐标类型描述；
  // 单位：线性单位代码 9001 为 m，经纬度坐标为度。
  // origin = 左上角坐标：由 Tiepoint (i,j,*,X,Y,*) 与像元尺寸推出 originX = X − i·sx，originY = Y + j·sy。
  const bits = Number(bitsPerSample[0] || 8)
  const sampleFormat = Number(sampleFormats[0] || 1)
  const dataType = `${sampleFormat === 3 ? 'Float' : sampleFormat === 2 ? 'Int' : 'UInt'}${bits}`
  const coordinateType = modelType === 1 ? '投影坐标' : modelType === 2 ? '经纬度坐标' : '未声明坐标系'
  const crs = projectedCode && projectedCode !== 32767 ? `EPSG:${projectedCode}` : geoAscii || coordinateType
  const unit = linearUnit === 9001 ? 'm' : modelType === 2 ? '°' : ''
  // 地形粗网格：最长边不超过 96，等间距采样；缺测值输出 null，供前端三维地形显示。
  const terrainScale = Math.min(1, 96 / Math.max(outputWidth, outputHeight))
  const terrainWidth = Math.max(2, Math.round(outputWidth * terrainScale))
  const terrainHeight = Math.max(2, Math.round(outputHeight * terrainScale))
  const terrainValues = []
  for (let y = 0; y < terrainHeight; y += 1) for (let x = 0; x < terrainWidth; x += 1) {
    const sourceX = Math.min(outputWidth - 1, Math.round(x * (outputWidth - 1) / Math.max(1, terrainWidth - 1)))
    const sourceY = Math.min(outputHeight - 1, Math.round(y * (outputHeight - 1) / Math.max(1, terrainHeight - 1)))
    const value = valueAt(sourceX, sourceY)
    terrainValues.push(Number.isFinite(value) ? value : null)
  }
  return {
    kind: 'tiff', path: windowsPath(path), width: outputWidth, height: outputHeight,
    bands: samples, band, bandNames, previewWidth, previewHeight, minimum, maximum,
    stretchMinimum: stretch.minimum, stretchMaximum: stretch.maximum, stretchPercent: 2,
    stretchMode: stretch.mode || 'percentile',
    stretchExcludedMinimum: Number.isFinite(stretch.excludedMinimum) ? stretch.excludedMinimum : null,
    pixels: pixels.toString('base64'), noData, dataType, coordinateType, crs, unit,
    thermalBand, wavelengthNanometres: Number.isFinite(wavelengthNanometres) ? wavelengthNanometres : null,
    hotspotThreshold, hotspotCount,
    hotspotPixels: hotspotPixels && hotspotCount ? hotspotPixels.toString('base64') : null,
    pixelSizeX: Number.isFinite(pixelScale[0]) ? pixelScale[0] : null,
    pixelSizeY: Number.isFinite(pixelScale[1]) ? pixelScale[1] : null,
    originX: tiePoint.length >= 6 ? tiePoint[3] - tiePoint[0] * (pixelScale[0] || 0) : null,
    originY: tiePoint.length >= 6 ? tiePoint[4] + tiePoint[1] * (pixelScale[1] || 0) : null,
    georeferenced: pixelScale.length >= 2 && tiePoint.length >= 6,
    mean: elevationCount ? elevationSum / elevationCount : 0,
    terrainWidth, terrainHeight, terrainValues,
    ...(values ? { values } : {})
  }
}

// 作用：读取风场 TIFF 并抽样成箭头。要求 4 个波段：0 水平风速大小、1 X 分量、2 垂直分量、3 Y 分量；
// 箭头取 band 0 作风速、band 1 为 u、band 3 为 v（对应 [x, y, u, v, 风速]）。
// 抽样：约每行 28 支箭头，步长 = ceil(最长边 / 28)，取各步长格的中心；任一分量非有限则跳过。
function readWindTiff(path) {
  const speed = readTiff(path, 0, true)
  if (speed.bands < 4) throw new Error('风速 TIFF 必须包含速度、X、垂直和 Y 四个波段')
  const componentX = readTiff(path, 1, true)
  const componentY = readTiff(path, 3, true)
  const targetArrowsAcross = 28
  const step = Math.max(1, Math.ceil(Math.max(speed.width, speed.height) / targetArrowsAcross))
  const firstX = Math.min(speed.width - 1, Math.floor(step / 2))
  const firstY = Math.min(speed.height - 1, Math.floor(step / 2))
  const arrows = []
  for (let y = firstY; y < speed.height; y += step) {
    for (let x = firstX; x < speed.width; x += step) {
      const index = y * speed.width + x
      const u = Number(componentX.values[index])
      const v = Number(componentY.values[index])
      const magnitude = Number(speed.values[index])
      if (Number.isFinite(u) && Number.isFinite(v) && Number.isFinite(magnitude)) {
        arrows.push([x, y, u, v, magnitude])
      }
    }
  }
  const result = { ...speed }
  delete result.values
  return {
    ...result,
    kind: 'wind',
    band: 0,
    bandNames: ['水平风速大小', 'X 分量', '垂直分量', 'Y 分量'],
    arrows,
    arrowStep: step,
    adaptive: true
  }
}

// 模块：两幅 TIFF 的逐像元对比（B 相对 A，差值 = B − A）。
// 前置条件：尺寸一致；若都带地理参考，则 crs、原点、像元大小也必须一致（容差 1e-8）。
// 步骤 1：一遍扫描用 Welford 在线公式累计差值均值、MAE、RMSE、最值，以及 A/B 的协方差和方差，
//         用于相关系数；任一方为非有限值的像元不参与。
// 步骤 2：均匀抽取最多 requestedScatterPoints（夹到 100–10000）个 (A,B) 点作散点图，
//         并对差值排序取 2%、98% 分位。
// 步骤 3：生成差值预览（最长边 768）：蓝=负、白=0、红=正，颜色深度按 |差值| / 分位限幅 线性变化。
// 步骤 4：汇总波段名与统计量返回。
function compareTiffResults(firstPath, secondPath, requestedBand = 0, requestedScatterPoints = 4000) {
  const band = Math.max(0, Math.floor(Number(requestedBand) || 0))
  const first = readTiff(firstPath, band, true)
  const second = readTiff(secondPath, band, true)
  const commonBands = Math.min(first.bands, second.bands)
  if (band >= commonBands) throw new Error(`所选波段超出共同波段范围（共 ${commonBands} 个）`)
  if (first.width !== second.width || first.height !== second.height) {
    throw new Error(`图像尺寸不一致：A 为 ${first.width} × ${first.height}，B 为 ${second.width} × ${second.height}`)
  }
  if (first.georeferenced && second.georeferenced) {
    const different = first.crs !== second.crs || ['pixelSizeX', 'pixelSizeY', 'originX', 'originY']
      .some((key) => Math.abs(Number(first[key]) - Number(second[key])) > 1e-8)
    if (different) throw new Error('两幅图像的坐标系、原点或像元大小不一致，无法逐像元比较')
  }

  let validPixels = 0, meanDifference = 0, sumAbsolute = 0, sumSquared = 0
  let differenceMinimum = Infinity, differenceMaximum = -Infinity
  let meanFirst = 0, meanSecond = 0, covariance = 0, varianceFirst = 0, varianceSecond = 0
  for (let index = 0; index < first.values.length; index += 1) {
    const a = first.values[index], b = second.values[index]
    if (!Number.isFinite(a) || !Number.isFinite(b)) continue
    const difference = b - a
    validPixels += 1
    meanDifference += (difference - meanDifference) / validPixels
    sumAbsolute += Math.abs(difference)
    sumSquared += difference * difference
    differenceMinimum = Math.min(differenceMinimum, difference)
    differenceMaximum = Math.max(differenceMaximum, difference)
    const deltaFirst = a - meanFirst
    meanFirst += deltaFirst / validPixels
    const deltaSecond = b - meanSecond
    meanSecond += deltaSecond / validPixels
    covariance += deltaFirst * (b - meanSecond)
    varianceFirst += deltaFirst * (a - meanFirst)
    varianceSecond += deltaSecond * (b - meanSecond)
  }
  if (!validPixels) throw new Error('两幅图像没有可共同比较的有效像元')

  const maxScatterPoints = Math.max(100, Math.min(10000, Math.floor(Number(requestedScatterPoints) || 4000)))
  const scatterTarget = Math.min(validPixels, maxScatterPoints)
  const scatter = []
  const differenceSamples = new Float64Array(validPixels)
  let validIndex = 0
  for (let index = 0; index < first.values.length; index += 1) {
    const a = first.values[index], b = second.values[index]
    if (!Number.isFinite(a) || !Number.isFinite(b)) continue
    if (validIndex >= Math.floor(scatter.length * validPixels / scatterTarget)) scatter.push([a, b])
    differenceSamples[validIndex] = b - a
    validIndex += 1
  }
  differenceSamples.sort()
  const percentile = (fraction) => {
    const position = (differenceSamples.length - 1) * fraction
    const lower = Math.floor(position), upper = Math.ceil(position)
    if (lower === upper) return differenceSamples[lower]
    const weight = position - lower
    return differenceSamples[lower] * (1 - weight) + differenceSamples[upper] * weight
  }
  const differencePercentileMinimum = percentile(.02)
  const differencePercentileMaximum = percentile(.98)

  const previewScale = Math.min(1, 768 / Math.max(first.width, first.height))
  const previewWidth = Math.max(1, Math.round(first.width * previewScale))
  const previewHeight = Math.max(1, Math.round(first.height * previewScale))
  const differenceLimit = Math.max(Math.abs(differencePercentileMinimum), Math.abs(differencePercentileMaximum), Number.EPSILON)
  const differencePixels = Buffer.alloc(previewWidth * previewHeight * 4)
  const negative = [36, 111, 211], neutral = [255, 255, 255], positive = [227, 74, 66]
  for (let y = 0; y < previewHeight; y += 1) for (let x = 0; x < previewWidth; x += 1) {
    const sourceX = Math.min(first.width - 1, Math.floor(x / previewScale))
    const sourceY = Math.min(first.height - 1, Math.floor(y / previewScale))
    const sourceIndex = sourceY * first.width + sourceX
    const a = first.values[sourceIndex], b = second.values[sourceIndex]
    const offset = (y * previewWidth + x) * 4
    if (!Number.isFinite(a) || !Number.isFinite(b)) {
      differencePixels[offset + 3] = 0
      continue
    }
    const difference = b - a
    const ratio = differenceLimit > 0 ? Math.min(1, Math.abs(difference) / differenceLimit) : 0
    const target = difference < 0 ? negative : positive
    for (let channel = 0; channel < 3; channel += 1) differencePixels[offset + channel] = Math.round(neutral[channel] + (target[channel] - neutral[channel]) * ratio)
    differencePixels[offset + 3] = 255
  }

  const bandNames = Array.from({ length: commonBands }, (_, index) => {
    const firstName = first.bandNames[index] || `波段 ${index + 1}`
    const secondName = second.bandNames[index] || `波段 ${index + 1}`
    return firstName === secondName ? firstName : `${firstName} / ${secondName}`
  })
  const correlationDenominator = Math.sqrt(varianceFirst * varianceSecond)
  return {
    kind: 'change', firstPath: windowsPath(firstPath), secondPath: windowsPath(secondPath),
    width: first.width, height: first.height, band, bands: commonBands, bandNames,
    previewWidth, previewHeight, differencePixels: differencePixels.toString('base64'),
    differenceMinimum, differenceMaximum, differencePercentileMinimum, differencePercentileMaximum, differenceLimit,
    validPixels, meanDifference, mae: sumAbsolute / validPixels,
    rmse: Math.sqrt(sumSquared / validPixels),
    correlation: correlationDenominator > 0 ? covariance / correlationDenominator : null,
    meanFirst, meanSecond, scatter
  }
}

// 模块：解析 ESRI ASCII Grid（.asc）DEM。输入：文件字节；includeValues 为真时额外返回整幅 values
// （NoData 换成 NaN）。输出：与 inspectDemFile 对 GeoTIFF 返回的字段一致的描述对象。
// 格式：头部 key value（ncols nrows xll* yll* cellsize nodata_value）后接按行排列的数值，
//       行从北向南。单位假定为 m；ASC 不含 CRS，只给出局部/投影坐标的提示。
// 限制：像元数不超过 2500 万；数值个数必须恰为 ncols×nrows；NoData 按数值完全相等判定。
// terrainValues：最长边降采样到 <= 96 的粗网格，供前端三维地形预览（缺测为 null）。
function inspectAsciiGrid(data, includeValues = false) {
  const text = data.toString('utf8').replace(/^\uFEFF/, '')
  const lines = text.split(/\r?\n/)
  const header = new Map()
  let dataStart = 0
  const keys = new Set(['ncols', 'nrows', 'xllcorner', 'xllcenter', 'yllcorner', 'yllcenter', 'cellsize', 'nodata_value'])
  for (let index = 0; index < lines.length; index += 1) {
    const match = lines[index].trim().match(/^(\S+)\s+(.+)$/)
    const key = match?.[1]?.toLowerCase()
    if (!match || !keys.has(key)) { dataStart = index; break }
    header.set(key, Number(match[2]))
    dataStart = index + 1
  }
  const width = Number(header.get('ncols'))
  const height = Number(header.get('nrows'))
  const cellSize = Number(header.get('cellsize'))
  if (![width, height].every((value) => Number.isInteger(value) && value > 0) || !(cellSize > 0))
    throw new Error('ASC 缺少有效的 NCOLS、NROWS 或 CELLSIZE')
  if (width * height > 25_000_000) throw new Error('DEM 栅格超过 2500 万像元，请先裁剪或降采样')
  const values = lines.slice(dataStart).join(' ').trim().split(/\s+/).filter(Boolean).map(Number)
  if (values.length !== width * height || values.some((value) => !Number.isFinite(value)))
    throw new Error(`ASC 高程数据数量无效，应为 ${width * height} 个数值`)
  const noDataValue = header.has('nodata_value') ? Number(header.get('nodata_value')) : null
  let minimum = Infinity
  let maximum = -Infinity
  let elevationSum = 0
  let elevationCount = 0
  for (const value of values) {
    if (noDataValue != null && value === noDataValue) continue
    minimum = Math.min(minimum, value)
    maximum = Math.max(maximum, value)
    elevationSum += value
    elevationCount += 1
  }
  if (!Number.isFinite(minimum)) throw new Error('ASC 中没有有效高程像元')
  const terrainScale = Math.min(1, 96 / Math.max(width, height))
  const terrainWidth = Math.max(2, Math.round(width * terrainScale))
  const terrainHeight = Math.max(2, Math.round(height * terrainScale))
  const terrainValues = []
  for (let y = 0; y < terrainHeight; y += 1) for (let x = 0; x < terrainWidth; x += 1) {
    const sourceX = Math.min(width - 1, Math.round(x * (width - 1) / Math.max(1, terrainWidth - 1)))
    const sourceY = Math.min(height - 1, Math.round(y * (height - 1) / Math.max(1, terrainHeight - 1)))
    const value = values[sourceY * width + sourceX]
    terrainValues.push(noDataValue != null && value === noDataValue ? null : value)
  }
  return {
    format: 'ESRI ASCII Grid', width, height, bands: 1, dataType: '文本数值',
    pixelSizeX: cellSize, pixelSizeY: cellSize, unit: 'm', noData: noDataValue,
    minimum, maximum, relief: maximum - minimum, coordinateType: '局部/投影坐标',
    crs: 'ASC 未内嵌 CRS', georeferenced: header.has('xllcorner') || header.has('xllcenter'),
    originX: header.get('xllcorner') ?? header.get('xllcenter') ?? null,
    originY: header.get('yllcorner') ?? header.get('yllcenter') ?? null,
    mean: elevationCount ? elevationSum / elevationCount : 0,
    terrainWidth, terrainHeight, terrainValues,
    ...(includeValues ? { values: values.map((value) => noDataValue != null && value === noDataValue ? Number.NaN : value) } : {})
  }
}

// 模块：构造 DEM 高度采样器 (north, east) → 相对 DEM 最低点的高度（m）。
// 输入：configuration.scene（terrain 开关、demFile、场景尺寸 x、y）；无地形/无 DEM 时返回 null。
// 说明：north/east 为场景内的米制坐标；这里用 north/scene.x、east/scene.y 归一化
//       （即 x 方向对应南北、y 对应东西，待核对与引擎一致）。缺测像元按最低值处理，
//       坐标夹在场景范围内，并做双线性插值；与引擎的 demHeightAboveMinimum 保持一致。
// Full-resolution terrain height above the DEM minimum, sampled exactly like the
// engine's demHeightAboveMinimum (bilinear, row 0 = north edge, columns eastward).
function demHeightSampler(configuration, baseDir = projectDir) {
  const scene = configuration?.scene
  if (!scene?.terrain || !scene.demFile) return null
  const path = projectAssetPath(scene.demFile, baseDir)
  const grid = extname(path).toLowerCase() === '.asc' ? inspectAsciiGrid(readFileSync(path), true) : readTiff(path, 0, true)
  const { width, height } = grid
  let minimum = Infinity
  for (const value of grid.values) if (Number.isFinite(value)) minimum = Math.min(minimum, value)
  if (!Number.isFinite(minimum) || width < 2 || height < 2) return null
  const at = (column, row) => { const value = grid.values[row * width + column]; return Number.isFinite(value) ? value : minimum }
  const sizeX = Math.max(1, Number(scene.x)), sizeY = Math.max(1, Number(scene.y))
  return (north, east) => {
    const u = Math.min(1, Math.max(0, east / sizeY)) * (width - 1)
    const v = (1 - Math.min(1, Math.max(0, north / sizeX))) * (height - 1)
    const x0 = Math.floor(u), y0 = Math.floor(v), x1 = Math.min(width - 1, x0 + 1), y1 = Math.min(height - 1, y0 + 1)
    const top = at(x0, y0) * (1 - (u - x0)) + at(x1, y0) * (u - x0)
    const bottom = at(x0, y1) * (1 - (u - x0)) + at(x1, y1) * (u - x0)
    return top * (1 - (v - y0)) + bottom * (v - y0) - minimum
  }
}

// 作用：检查 DEM 文件并返回前端展示用的信息（尺寸、像元大小、NoData、高程范围、起伏、CRS 等）。
// 支持 .asc 与 GeoTIFF；GeoTIFF 必须为单波段，否则报错。
function inspectDemFile(path, extension) {
  if (extension === '.asc') return inspectAsciiGrid(readFileSync(path))
  const raster = readTiff(path, 0)
  if (raster.bands !== 1) throw new Error(`DEM 必须是单波段栅格，当前文件有 ${raster.bands} 个波段`)
  return {
    format: 'GeoTIFF', width: raster.width, height: raster.height, bands: raster.bands,
    dataType: raster.dataType, pixelSizeX: raster.pixelSizeX, pixelSizeY: raster.pixelSizeY,
    unit: raster.unit, noData: raster.noData, minimum: raster.minimum, maximum: raster.maximum,
    relief: raster.maximum - raster.minimum, coordinateType: raster.coordinateType,
    crs: raster.crs, georeferenced: raster.georeferenced,
    originX: raster.originX, originY: raster.originY, mean: raster.mean,
    terrainWidth: raster.terrainWidth, terrainHeight: raster.terrainHeight,
    terrainValues: raster.terrainValues
  }
}

// 模块：读取 ENVI 影像（.hdr + 同名 .img）的一个波段并生成灰度预览（不返回数值）。
// 支持 bsq / bil / bip 三种交错方式（头文件写 bsp 视为 bsq）与 enviValue 支持的数据类型；
// header offset 与 byte order 按头文件处理（缺省 0 与小端）。
// 预览与 readTiff 同样：最长边 <= 768 的最近邻降采样，按 2–98% 分位拉伸到 0–255。
// 数据经 cachedResultSource 读盘，期望字节数不足时报“数据不完整”。
function readEnvi(path, requestedBand = 0) {
  const header = readFileSync(path, 'utf8')
  const metadata = enviMetadata(header)
  const width = Number(metadata.samples)
  const height = Number(metadata.lines)
  const bands = Number(metadata.bands)
  const dataType = Number(metadata['data type'])
  const headerOffset = Number(metadata['header offset'] || 0)
  const interleave = String(metadata.interleave || 'bsq').toLowerCase().replace('bsp', 'bsq')
  const littleEndian = Number(metadata['byte order'] || 0) === 0
  const band = Math.max(0, Math.min(bands - 1, Number(requestedBand) || 0))
  const bandNamesText = header.match(/band names\s*=\s*\{([^}]*)\}/i)?.[1] || metadata['band names'] || ''
  const bandNames = String(bandNamesText).split(',').map((value) => value.trim()).filter(Boolean)
  if (![width, height, bands].every((value) => Number.isInteger(value) && value > 0)) throw new Error('ENVI 头文件缺少有效的 samples、lines 或 bands')
  const typeSize = enviTypeSize(dataType)
  if (!typeSize) throw new Error(`暂不支持 ENVI data type ${dataType}`)
  const imagePath = path.slice(0, -extname(path).length) + '.img'
  if (!existsSync(imagePath)) throw new Error(`找不到 ENVI 影像数据：${imagePath}`)
  const data = cachedResultSource(imagePath).data
  const expectedSize = headerOffset + width * height * bands * typeSize
  if (data.length < expectedSize) throw new Error(`ENVI 数据不完整：需要 ${expectedSize} 字节，实际 ${data.length} 字节`)

  const valueAt = (x, y) => {
    let index
    if (interleave === 'bil') index = y * width * bands + band * width + x
    else if (interleave === 'bip') index = (y * width + x) * bands + band
    else index = band * width * height + y * width + x
    return enviValue(data, headerOffset + index * typeSize, dataType, littleEndian)
  }
  let minimum = Infinity, maximum = -Infinity
  for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
    const value = valueAt(x, y)
    if (Number.isFinite(value)) { minimum = Math.min(minimum, value); maximum = Math.max(maximum, value) }
  }
  if (!Number.isFinite(minimum)) { minimum = 0; maximum = 0 }
  const scale = Math.min(1, 768 / Math.max(width, height))
  const previewWidth = Math.max(1, Math.round(width * scale))
  const previewHeight = Math.max(1, Math.round(height * scale))
  const pixels = Buffer.alloc(previewWidth * previewHeight)
  const previewValues = new Float64Array(previewWidth * previewHeight)
  const finiteSamples = new Float64Array(previewWidth * previewHeight)
  let finiteSampleCount = 0
  for (let y = 0; y < previewHeight; y += 1) for (let x = 0; x < previewWidth; x += 1) {
    const value = valueAt(Math.min(width - 1, Math.floor(x / scale)), Math.min(height - 1, Math.floor(y / scale)))
    const index = y * previewWidth + x
    previewValues[index] = value
    if (Number.isFinite(value)) finiteSamples[finiteSampleCount++] = value
  }
  const stretch = twoPercentStretch(finiteSamples, finiteSampleCount, minimum, maximum)
  for (let index = 0; index < pixels.length; index += 1) {
    pixels[index] = stretchedByte(previewValues[index], stretch.minimum, stretch.maximum)
  }
  return { kind: 'envi', path: windowsPath(path), width, height, bands, band, bandNames, previewWidth, previewHeight, minimum, maximum, stretchMinimum: stretch.minimum, stretchMaximum: stretch.maximum, stretchPercent: 2, pixels: pixels.toString('base64') }
}

// 模块：全部 /api/* 路由的分发函数，由 HTTP 服务器回调（见下方 createServer）调用。
// 约定：返回 false 表示没有路由匹配（调用方回 404“API 不存在”）；各路由以
//       if (method && pathname) 顺序匹配，响应一律经 json()，错误统一 { error }，
//       输入/状态错误多为 400（运行冲突等也走 400 或带 error 的 200，见各分支）。
//       会读写大文件的工作都交给 heavyTasks（后台线程池），避免阻塞 SSE 与其他请求。
// 路由分组（按出现顺序）：
//   1. STREAM 服务路由（streamService.handle，优先处理，未处理返回 false 才继续）
//   2. histream-proxy 代理：info / result 下载 / run（固定抓取帧，不调用原生引擎）
//   3. 默认值、帮助文档、计算设备、SSE 事件流
//   4. 示例与工程：打开/新建/另存/保存，导入 OBJ、气象、DEM、波谱、纹理，实例分布，资源读取
//   5. 运行：预检 run/preflight、启动 run、停止 stop、重置 reset
//   6. 结果：删除、列表、读取、变化分析；最后是在资源管理器中打开路径
async function api(request, response, url) {
  const streamHandled = await streamService.handle(request, response, url)
  if (streamHandled !== false) return streamHandled
  // 路由分组 2：histream-proxy 代理。info 返回部署与健康状态（含可选帧列表）。
  if (request.method === 'GET' && url.pathname === '/api/histream-proxy/info') {
    return json(response, 200, histreamProxyHealth(true))
  }
  // 下载代理数值结果（.npz）。安全：文件名必须匹配 `<名>_frame-NNNN.npz`，且结果目录与文件
  // 都必须位于部署清单声明的 artifactRoot 内（含符号链接检查）；响应为附件下载，不缓存。
  if (request.method === 'GET' && url.pathname === '/api/histream-proxy/result') {
    try {
      const active = histreamProxyDeployment()
      if (!active.deployment || !active.ready) throw new Error(active.reason || 'histream-proxy is not ready')
      const filename = String(url.searchParams.get('file') || '')
      if (!/^[A-Za-z0-9_.-]+_frame-\d{4}\.npz$/.test(filename) || filename !== basename(filename))
        throw new Error('Invalid proxy result identifier')
      const resultRoot = resolve(normalizeHostPath(active.deployment.result_root || ''))
      if (!pathWithin(active.paths.artifactRoot, resultRoot)
          || !realPathWithin(active.paths.artifactRoot, resultRoot)) throw new Error('Proxy result directory escapes artifact root')
      const resultPath = resolve(resultRoot, filename)
      if (!pathWithin(resultRoot, resultPath) || !existsSync(resultPath) || !statSync(resultPath).isFile()
          || !realPathWithin(resultRoot, resultPath))
        throw new Error(`Proxy numerical result was not found: ${filename}`)
      const safeResultPath = realpathSync(resultPath)
      response.writeHead(200, { 'Content-Type': 'application/octet-stream',
        'Content-Disposition': `attachment; filename="${filename}"`,
        'Content-Length': statSync(safeResultPath).size, 'Cache-Control': 'no-store' })
      createReadStream(safeResultPath).on('error', () => response.destroy()).pipe(response)
      return true
    } catch (error) {
      return json(response, 400, { error: error.message, backend: 'histream-proxy', nativeEngineInvoked: false })
    }
  }
  // 运行代理：只接受固定抓取（captureId + episodeId + frameIndex），拒绝普通工程/引擎命令行参数；
  // 同一时刻只允许一个渲染（histreamProxyRequestInFlight，冲突返回 409）；
  // 以 Python runner 子进程执行（超时 120 s），其输出必须是 backend=histream-proxy 且
  // native_engine_invoked=false 的 JSON，否则视为不安全响应；失败不回退到原生引擎（noFallback）。
  if (request.method === 'POST' && url.pathname === '/api/histream-proxy/run') {
    try {
      const data = await body(request)
      if (['inputPath', 'projectPath', 'mode', 'executable'].some((key) => data[key] !== undefined)) {
        return json(response, 400, { error: 'histream-proxy does not accept ordinary Stream Studio project.json or engine CLI inputs; select a frame from the fixed Aoyun capture.',
          backend: 'histream-proxy', supportedCaptureOnly: true, nativeEngineInvoked: false })
      }
      const active = histreamProxyDeployment()
      if (!active.deployment || !active.ready) {
        return json(response, 503, { error: active.reason || 'histream-proxy bundle is not ready',
          backend: 'histream-proxy', manifestPath: windowsPath(active.manifestPath), nativeEngineInvoked: false })
      }
      const deployment = active.deployment
      const episodeId = String(data.episodeId || '')
      const frameIndex = Number(data.frameIndex)
      if (data.captureId !== deployment.capture_id || !episodeId || !Number.isInteger(frameIndex)) {
        return json(response, 400, { error: 'Select an allowlisted captureId, episodeId and integer frameIndex.',
          backend: 'histream-proxy', captureId: deployment.capture_id,
          captureScope: deployment.capture_scope, nativeEngineInvoked: false })
      }
      const supported = deployment.supported_frames.some((item) =>
        item.episode_id === episodeId && item.frame_index === frameIndex)
      if (!supported) return json(response, 400, { error: 'Requested frame is outside the trained capture allowlist.',
        backend: 'histream-proxy', captureId: deployment.capture_id,
        captureScope: deployment.capture_scope, nativeEngineInvoked: false })
      if (histreamProxyRequestInFlight) return json(response, 409, { error: 'Another histream-proxy frame is being rendered.',
        backend: 'histream-proxy', nativeEngineInvoked: false })
      histreamProxyRequestInFlight = true
      const python = active.paths.python
      const runner = active.paths.runner
      const manifest = active.manifestPath
      return await new Promise((resolveProxy) => {
        execFile(python, [runner, '--deployment-manifest', manifest,
          '--episode-id', episodeId, '--frame-index', String(frameIndex)], {
          cwd: dirname(runner), windowsHide: true, encoding: 'utf8',
          timeout: 120000, maxBuffer: 8 * 1024 * 1024,
          env: { ...process.env, PYTHONUTF8: '1' },
        }, (error, stdout, stderr) => {
          histreamProxyRequestInFlight = false
          if (error) {
            const detail = String(stderr || error.message || '').trim()
            json(response, 502, { error: `histream-proxy runner failed: ${detail}`,
              backend: 'histream-proxy', noFallback: true, nativeEngineInvoked: false })
            resolveProxy(true)
            return
          }
          try {
            const result = JSON.parse(String(stdout || '').trim())
            if (result.status !== 'ok' || result.backend !== 'histream-proxy' || result.native_engine_invoked !== false) {
              throw new Error('Proxy runner returned an invalid or unsafe response')
            }
            result.downloadUrl = `/api/histream-proxy/result?file=${encodeURIComponent(result.download_id)}.npz`
            result.activeManifestPath = windowsPath(manifest)
            result.runnerPath = windowsPath(runner)
            result.pythonExecutable = windowsPath(python)
            result.fixedCaptureOnly = true
            json(response, 200, result)
          } catch (parseError) {
            json(response, 502, { error: `Cannot parse histream-proxy output: ${parseError.message}`,
              backend: 'histream-proxy', noFallback: true, nativeEngineInvoked: false })
          }
          resolveProxy(true)
        })
      })
    } catch (error) {
      histreamProxyRequestInFlight = false
      return json(response, 400, { error: error.message, backend: 'histream-proxy', nativeEngineInvoked: false })
    }
  }
  // 路由分组 3：默认值。返回引擎路径及存在性、当前工程文件、运行超时（秒）、应用版本、
  // STREAM 可执行文件、是否配置了代理清单等；桌面主进程也用它探测服务是否就绪（waitForServer）。
  if (request.method === 'GET' && url.pathname === '/api/defaults') {
    const path = executable()
    return json(response, 200, { executable: windowsPath(path), executableExists: existsSync(path), radiosityExecutable: windowsPath(path), radiosityExecutableExists: existsSync(path), projectFile: windowsPath(projectFile), platform: process.platform, histreamProxyConfigured: Boolean(String(envSetting('STREAM_STUDIO_PROXY_MANIFEST', 'HISTREAM_STUDIO_PROXY_MANIFEST') || '').trim()), runTimeoutSeconds: Math.round(RUN_TIMEOUT_MS / 1000), appVersion: APP_VERSION, streamExecutable: streamService.executable(), versions: { node: process.versions.node, three: '0.185.1' } })
  }
  // 打开帮助手册：只允许 HELP_DOCUMENTS 白名单中的文档，用资源目录或源码 docs/ 下的文件，Windows 上交给 explorer 打开。
  if (request.method === 'POST' && url.pathname === '/api/help/open') {
    try {
      const data = await body(request)
      const file = HELP_DOCUMENTS[data.document]
      if (!file) throw new Error('未知的帮助文档')
      const path = [join(RESOURCE_ROOT, 'docs', file), join(PROJECT_ROOT, 'docs', file)].find((candidate) => existsSync(candidate))
      if (!path) throw new Error(`找不到 ${file}，请运行 tools/build-core-manuals-pdf.py 生成手册`)
      if (process.platform === 'win32') spawn('explorer.exe', [path], { detached: true, windowsHide: true }).on('error', () => {}).unref()
      return json(response, 200, { path: windowsPath(path) })
    } catch (error) { return json(response, 400, { error: error.message }) }
  }
  // 列出计算设备（GPU）：拒绝跨站请求；引擎路径经 allowedEngine 白名单校验；refresh=1 强制重新枚举。
  if (request.method === 'GET' && url.pathname === '/api/devices') {
    if (request.headers['sec-fetch-site'] === 'cross-site') return json(response, 403, { error: '拒绝跨站请求' })
    try {
      const engine = allowedEngine(url.searchParams.get('executable'))
      return json(response, 200, await listComputeDevices(engine, url.searchParams.get('refresh') === '1'))
    } catch (error) {
      return json(response, 500, { error: error.message })
    }
  }
  // SSE 事件流：连接后立即推送一次当前运行状态，之后由 emit() 向所有 clients 广播进度。
  if (request.method === 'GET' && url.pathname === '/api/events') {
    response.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' })
    response.write(': connected\n\n')
    // EventSource reconnects silently; this lets the GUI resync a run whose
    // started/closed events it missed while disconnected or reloading.
    response.write(`data: ${JSON.stringify({ type: 'status', running: Boolean(child), pid: child?.pid ?? null, ...(child?.histreamRun || {}) })}\n\n`)
    clients.add(response)
    request.on('close', () => clients.delete(response))
    return
  }
  // 路由分组 4：示例与工程。列出示例；打开示例（运行中不允许更换工程）。
  if (request.method === 'GET' && url.pathname === '/api/examples') {
    try {
      return json(response, 200, { examples: listExamples(), directory: defaultExampleDirectory() })
    } catch (error) {
      return json(response, 500, { error: error.message })
    }
  }
  if (request.method === 'POST' && url.pathname === '/api/examples/open') {
    try {
      if (child) throw new Error('模拟运行中，不能更换工程')
      return json(response, 200, openExample(await body(request)))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 打开工程：按路径打开 project.json 或 USD；pick 先让用户在系统对话框中选文件（取消返回 cancelled）。
  if (request.method === 'POST' && url.pathname === '/api/project/open') {
    try {
      const data = await body(request)
      return json(response, 200, await openProjectOrUsd(data.path, data.mode))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  if (request.method === 'POST' && url.pathname === '/api/project/pick') {
    try {
      const data = await body(request)
      const selected = await chooseProjectFile()
      if (!selected) return json(response, 200, { cancelled: true })
      return json(response, 200, await openProjectOrUsd(selected, data.mode))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 新建工程：成功后把当前工程路径 projectFile/projectDir 切换到新工程，返回 201。
  if (request.method === 'POST' && url.pathname === '/api/project/create') {
    try {
      const result = createProject(await body(request))
      projectFile = result.path
      projectDir = result.projectDir
      return json(response, 201, { ...result, path: windowsPath(result.path), projectDir: windowsPath(result.projectDir), projectPath: windowsPath(result.projectPath) })
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 另存为：复制工程到新目录并切换当前工程；随后在后台线程导出 project.usd，导出失败只在 usd 字段里报告，不影响另存成功。
  if (request.method === 'POST' && url.pathname === '/api/project/save-as') {
    try {
      const result = saveProjectAs(await body(request))
      projectFile = result.path
      projectDir = result.projectDir
      let usd
      try { usd = await exportProjectUsdInWorker(result.path, result.project) } catch (error) { usd = { error: error.message } }
      return json(response, 201, { ...result, path: windowsPath(result.path), projectDir: windowsPath(result.projectDir), projectPath: windowsPath(result.projectPath), usd })
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 篷布覆盖：以目标 OBJ 为基础在后台线程生成篷布网格，写入 models/<name>.obj（名称只允许字母/数字/_-.）。
  if (request.method === 'POST' && url.pathname === '/api/project/tarp-cover') {
    try {
      const data = await body(request)
      if (!projectDir || !projectFile) throw new Error('请先新建或打开工程')
      const target = projectAssetPath(data.fileName, projectDir)
      if (extname(target).toLowerCase() !== '.obj' || !existsSync(target)) throw new Error(`找不到目标 OBJ：${data.fileName}`)
      const name = basename(String(data.name || '').trim())
      if (!/^[\p{L}\p{N}_\-.]+$/u.test(name)) throw new Error('篷布名称包含无效字符')
      // 读取目标 OBJ、生成篷布并写出在后台线程执行。
      const summary = await heavyTasks.run('tarp-cover', { target, outputPath: join(projectDir, 'models', `${name}.obj`), lift: data.lift, drape: data.drape })
      return json(response, 201, { ...summary, relativePath: `models/${name}.obj` })
    } catch (error) { return json(response, 400, { error: error.message }) }
  }
  // 导入 OBJ：只接受 .obj；清理并写出在后台线程完成，按工程目录分线程（key）串行，保证同一工程内命名不冲突。
  if (request.method === 'POST' && url.pathname === '/api/project/import-obj') {
    try {
      const data = await body(request)
      if (!projectDir || !projectFile) throw new Error('请先新建或打开工程')
      const sourceName = basename(String(data.name || '').trim())
      const extension = extname(sourceName).toLowerCase()
      if (!sourceName || extension !== '.obj') throw new Error('只能导入 OBJ 文件')
      // 清理与写出（可达 80 MB）在后台线程执行；同一工程固定到同一线程，命名与写入不会交错。
      return json(response, 201, await heavyTasks.run('import-obj', { projectDir, sourceName, extension, content: data.content }, { key: projectDir }))
    } catch (error) { return json(response, 400, { error: error.message }) }
  }
  // 导入气象驱动文件（TXT/METEO/DAT/CSV，<= 20MB）到 <工程>/meteorology。
  // 文件名清洗为安全字符，重名则追加 _2、_3 …；返回相对路径供工程引用。
  if (request.method === 'POST' && url.pathname === '/api/project/import-meteo') {
    try {
      const data = await body(request)
      if (!projectDir || !projectFile) throw new Error('请先新建或打开工程')
      const sourceName = basename(String(data.name || '').trim())
      const allowedExtensions = new Set(['.txt', '.meteo', '.dat', '.csv'])
      const extension = extname(sourceName).toLowerCase()
      if (!sourceName || !allowedExtensions.has(extension)) throw new Error('只能导入 TXT、METEO、DAT 或 CSV 气象文件')
      const content = String(data.content || '')
      if (!content.trim()) throw new Error('气象驱动文件为空')
      if (Buffer.byteLength(content, 'utf8') > 20 * 1024 * 1024) throw new Error('气象驱动文件超过 20MB')
      const meteorologyDir = join(projectDir, 'meteorology')
      mkdirSync(meteorologyDir, { recursive: true })
      const stem = sourceName.slice(0, -extension.length).replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '') || 'meteorology'
      let name = `${stem}${extension}`
      for (let suffix = 2; existsSync(join(meteorologyDir, name)); suffix += 1) name = `${stem}_${suffix}${extension}`
      const path = join(meteorologyDir, name)
      writeFileSync(path, content, 'utf8')
      return json(response, 201, { path: windowsPath(path), name, relativePath: join('meteorology', name).replaceAll('\\', '/') })
    } catch (error) { return json(response, 400, { error: error.message }) }
  }
  // 导入 DEM（单波段 GeoTIFF 或 ESRI ASC，<= 64MB，base64 上传）到 <工程>/terrain。
  // 写盘后在后台线程检查文件元数据，并给出警告：经纬度坐标、缺少地理参考、像元类型不是 Float32/Int16；
  // 检查失败时删除刚写入的文件，避免留下无效 DEM。
  if (request.method === 'POST' && url.pathname === '/api/project/import-dem') {
    let importedPath = ''
    try {
      const data = await body(request)
      const targetProjectFile = data.projectPath ? resolveProjectFile(data.projectPath) : projectFile
      const targetProjectDir = targetProjectFile ? dirname(targetProjectFile) : projectDir
      if (!targetProjectDir || !targetProjectFile) throw new Error('请先新建或打开工程')
      const sourceName = basename(String(data.name || '').trim())
      const extension = extname(sourceName).toLowerCase()
      if (!sourceName || !new Set(['.tif', '.tiff', '.asc']).has(extension))
        throw new Error('DEM 仅支持单波段 GeoTIFF（TIF/TIFF）或 ESRI ASCII Grid（ASC）')
      const encoded = String(data.contentBase64 || '')
      if (!encoded || encoded.length > 90 * 1024 * 1024) throw new Error('DEM 文件为空或超过 64MB')
      const content = Buffer.from(encoded, 'base64')
      if (!content.length || content.length > 64 * 1024 * 1024) throw new Error('DEM 文件为空或超过 64MB')
      const terrainDir = join(targetProjectDir, 'terrain')
      mkdirSync(terrainDir, { recursive: true })
      const stem = sourceName.slice(0, -extension.length).replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '') || 'terrain'
      let name = `${stem}${extension}`
      for (let suffix = 2; existsSync(join(terrainDir, name)); suffix += 1) name = `${stem}_${suffix}${extension}`
      importedPath = join(terrainDir, name)
      writeFileSync(importedPath, content)
      const metadata = await heavyTasks.run('dem-inspect', { path: importedPath, extension })
      const warnings = []
      if (metadata.coordinateType === '经纬度坐标') warnings.push('源文件为经纬度坐标；建议重投影到米制坐标后再模拟')
      if (!metadata.georeferenced) warnings.push('文件未包含完整地理参考；HiStream 仍会把整幅栅格映射到场景范围')
      if (!['Float32', 'Int16'].includes(metadata.dataType)) warnings.push(`当前像元类型为 ${metadata.dataType}；建议使用 Float32 或 Int16`)
      return json(response, 201, {
        path: windowsPath(importedPath), name,
        relativePath: join('terrain', name).replaceAll('\\', '/'), metadata, warnings
      })
    } catch (error) {
      if (importedPath && existsSync(importedPath)) unlinkSync(importedPath)
      return json(response, 400, { error: error.message })
    }
  }
  // 查询已有 DEM 的元数据：相对路径按工程目录解析，只接受 .tif/.tiff/.asc；以文件路径为 key 在后台线程串行读取。
  if (request.method === 'POST' && url.pathname === '/api/project/dem-info') {
    try {
      const data = await body(request)
      const targetProjectFile = data.projectPath ? resolveProjectFile(data.projectPath) : projectFile
      const targetProjectDir = targetProjectFile ? dirname(targetProjectFile) : projectDir
      if (!targetProjectDir) throw new Error('请先新建或打开工程')
      const storedPath = normalizeHostPath(data.path).replace(/^"|"$/g, '')
      if (!storedPath) throw new Error('DEM 路径为空')
      const demPath = isAbsolute(storedPath) ? resolve(storedPath) : resolve(targetProjectDir, storedPath)
      const extension = extname(demPath).toLowerCase()
      if (!new Set(['.tif', '.tiff', '.asc']).has(extension))
        throw new Error('DEM 仅支持 TIF、TIFF 或 ASC')
      if (!existsSync(demPath) || !statSync(demPath).isFile()) throw new Error(`找不到 DEM：${demPath}`)
      return json(response, 200, toJsonBytes(await heavyTasks.run('dem-info', { path: demPath, extension }, { key: demPath })))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 导入波谱文件（TXT/DAT/CSV，<= 5MB）到 <工程>/spectral；内容先经 normalizeSpectrumContent 规范化。
  if (request.method === 'POST' && url.pathname === '/api/project/import-spectrum') {
    try {
      const data = await body(request)
      if (!projectDir || !projectFile) throw new Error('请先新建或打开工程')
      const sourceName = basename(String(data.name || '').trim())
      const allowedExtensions = new Set(['.txt', '.dat', '.csv'])
      const extension = extname(sourceName).toLowerCase()
      if (!sourceName || !allowedExtensions.has(extension)) throw new Error('只能导入 TXT、DAT 或 CSV 波谱文件')
      if (Buffer.byteLength(String(data.content || ''), 'utf8') > 5 * 1024 * 1024) throw new Error('波谱文件超过 5MB')
      const content = normalizeSpectrumContent(data.content)
      const spectralDir = join(projectDir, 'spectral')
      mkdirSync(spectralDir, { recursive: true })
      const stem = sourceName.slice(0, -extension.length).replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '') || 'spectrum'
      let name = `${stem}${extension}`
      for (let suffix = 2; existsSync(join(spectralDir, name)); suffix += 1) name = `${stem}_${suffix}${extension}`
      const path = join(spectralDir, name)
      writeFileSync(path, content, 'utf8')
      return json(response, 201, { path: windowsPath(path), name, relativePath: join('spectral', name).replaceAll('\\', '/') })
    } catch (error) { return json(response, 400, { error: error.message }) }
  }
  // 只读取图像（PNG/JPG，<= 32MB）供编辑器纹理面板预览；路径必须在当前工程或内置 assets 目录内，且拒绝跨站请求。
  if (request.method === 'GET' && url.pathname === '/api/project/image') {
    try {
      // Read-only image access for the editor (texture palettes and previews).
      if (request.headers['sec-fetch-site'] === 'cross-site') throw new Error('拒绝跨站请求')
      const stored = String(url.searchParams.get('path') || '')
      const extension = extname(stored).toLowerCase()
      const types = { '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg' }
      if (!types[extension]) throw new Error('只能读取 PNG 或 JPG 图像')
      const path = projectAssetPath(stored, projectDir || PROJECT_ROOT)
      const allowed = (projectDir && realPathWithin(projectDir, path)) || realPathWithin(join(RESOURCE_ROOT, 'assets'), path)
      if (!allowed) throw new Error('图像必须位于当前工程或 Stream Studio assets 目录')
      if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`找不到图像：${stored}`)
      if (statSync(path).size > 32 * 1024 * 1024) throw new Error('图像超过 32MB')
      response.writeHead(200, { 'Content-Type': types[extension], 'Cache-Control': 'no-cache' })
      return createReadStream(path).on('error', () => response.destroy()).pipe(response)
    } catch (error) { return json(response, 400, { error: error.message }) }
  }
  // 导入纹理（PNG/JPG/TIF，<= 32MB，base64）到 <工程>/textures；
  // 除扩展名外还校验文件头魔数（PNG、JPEG、TIFF 的 II*\0 / MM\0*），防止伪装文件。同名文件直接覆盖。
  if (request.method === 'POST' && url.pathname === '/api/project/import-texture') {
    try {
      const data = await body(request)
      if (!projectDir || !projectFile) throw new Error('请先新建或打开工程')
      const sourceName = basename(String(data.name || '').trim())
      const extension = extname(sourceName).toLowerCase()
      if (!sourceName || !new Set(['.png', '.jpg', '.jpeg', '.tif', '.tiff']).has(extension)) throw new Error('纹理仅支持 PNG、JPG 或 TIF 图像')
      const content = Buffer.from(String(data.contentBase64 || ''), 'base64')
      if (!content.length || content.length > 32 * 1024 * 1024) throw new Error('纹理图像为空或超过 32MB')
      const signatures = { '.png': [0x89, 0x50, 0x4e, 0x47], '.jpg': [0xff, 0xd8, 0xff], '.jpeg': [0xff, 0xd8, 0xff] }
      const tiff = content.subarray(0, 4).equals(Buffer.from([0x49, 0x49, 0x2a, 0x00])) || content.subarray(0, 4).equals(Buffer.from([0x4d, 0x4d, 0x00, 0x2a]))
      const valid = signatures[extension] ? signatures[extension].every((byte, index) => content[index] === byte) : tiff
      if (!valid) throw new Error('文件内容与扩展名不符，请导入有效的 PNG/JPG/TIF 图像')
      const textureDir = join(projectDir, 'textures')
      mkdirSync(textureDir, { recursive: true })
      const stem = sourceName.slice(0, -extension.length).replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '') || 'texture'
      const name = `${stem}${extension}`
      const path = join(textureDir, name)
      writeFileSync(path, content)
      return json(response, 201, { path: windowsPath(path), name, relativePath: `textures/${name}` })
    } catch (error) { return json(response, 400, { error: error.message }) }
  }
  // 写实例分布文件：每行 `x y z scale rotation`（z 缺省 0、scale 缺省 1、rotation 缺省 0，旋转单位待核对），
  // 至多 100000 个实例，scale 必须 > 0；文件只能位于当前工程 positions/ 目录且为 .txt。
  if (request.method === 'POST' && url.pathname === '/api/project/distribution') {
    try {
      const data = await body(request)
      if (!projectDir || !projectFile) throw new Error('请先新建或打开工程')
      if (!Array.isArray(data.instances) || !data.instances.length) throw new Error('实例分布不能为空')
      if (data.instances.length > 100000) throw new Error('单个 OBJ 最多支持 100000 个实例')
      const positionsDir = resolve(projectDir, 'positions')
      mkdirSync(positionsDir, { recursive: true })
      const safeName = basename(String(data.name || 'object')).replace(/\.[^.]+$/, '').replace(/[^\p{L}\p{N}_-]+/gu, '_') || 'object'
      const path = data.path ? resolveAsset(data.path) : join(positionsDir, `${safeName}_position.txt`)
      if (!sameResolvedPath(dirname(path), positionsDir) || extname(path).toLowerCase() !== '.txt') throw new Error('实例分布文件必须位于当前工程 positions 目录，且为 .txt 文件')
      const lines = data.instances.map((item, index) => {
        const values = [item.x, item.y, item.z ?? 0, item.scale ?? 1, item.rotation ?? 0].map(Number)
        if (!values.every(Number.isFinite) || values[3] <= 0) throw new Error(`第 ${index + 1} 个实例参数无效`)
        return values.join(' ')
      })
      writeFileSync(path, `${lines.join('\n')}\n`, 'utf8')
      return json(response, 200, { path: windowsPath(path), relativePath: projectStoredPath(path, projectDir), count: lines.length })
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 读取场景资源（OBJ 等，<= 80MB）：路径限制在当前工程或内置 assets 内；读文件与 JSON 序列化在后台线程完成。
  if (request.method === 'POST' && url.pathname === '/api/project/read') {
    try {
      const data = await body(request)
      const path = projectAssetPath(data.path, projectDir)
      if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`找不到场景资源：${path}`)
      if (!((projectDir && realPathWithin(projectDir, path)) || realPathWithin(join(RESOURCE_ROOT, 'assets'), path)))
        throw new Error('只能读取当前工程或 Stream Studio assets 目录中的资源')
      if (statSync(path).size > 80 * 1024 * 1024) throw new Error('预览资源超过 80MB，请使用简化模型')
      // 读文件与 JSON 序列化（OBJ 可达 80 MB）在后台线程执行。
      return json(response, 200, toJsonBytes(await heavyTasks.run('project-read', { path })))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 保存工程：接受 data.project 对象或 data.content（JSON 文本），经 validateProject 校验后写盘，
  // 并把当前工程切换为该路径；随后导出 project.usd（失败只回报在 usd 字段里）。
  if (request.method === 'POST' && url.pathname === '/api/project/save') {
    try {
      const data = await body(request)
      const path = resolveProjectFile(data.path || projectFile)
      let candidate = data.project
      if (!candidate) {
        try { candidate = JSON.parse(String(data.content || '')) }
        catch (error) { throw new Error(`project.json 格式错误：${error.message}`) }
      }
      const validation = validateProject(candidate)
      if (!validation.valid) throw new Error(validation.errors.join('；'))
      let project = validation.project
      ensureDefaultSceneAsset(project, dirname(path))
      project = writeProject(path, project)
      const content = stringifyProject(project)
      projectFile = path
      projectDir = dirname(path)
      let usd
      try { usd = await exportProjectUsdInWorker(path, project) } catch (error) { usd = { error: error.message } }
      return json(response, 200, { path: windowsPath(path), content, project, projectPath: windowsPath(path), usd })
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 运行前预检：读取工程并在后台线程统计三角面数、估算耗时等（runPreflight），不启动引擎。
  if (request.method === 'POST' && url.pathname === '/api/run/preflight') {
    try {
      const data = await body(request)
      if (!modes.has(data.mode)) throw new Error('不支持的计算模式')
      const input = resolveProjectFile(data.inputPath || projectFile)
      const stored = readProject(input)
      const project = stored.project ? normalizeStoredProjectPaths(stored.project, dirname(input)) : null
      if (!project) throw new Error('无法读取工程配置')
      // 三角面统计要整读 OBJ，历史耗时要遍历 runs/：在后台线程执行。
      json(response, 200, toJsonBytes(await heavyTasks.run('run-preflight', { input, project, mode: data.mode })))
    } catch (error) {
      json(response, 400, { error: error.message })
    }
    return
  }
  // 路由：启动一次 histream 引擎模拟（本文件最核心的分支）。输入：mode（引擎模式枚举）、
  // inputPath（工程 project.json，缺省为当前工程）、executable（引擎路径，经白名单校验）。
  // 步骤 1：互斥检查：正在重置/正在运行/正在准备时拒绝（runPreparing 覆盖异步准备阶段）。
  // 步骤 2：读取并规范化工程（不改写源工程），校验引擎存在。
  // 步骤 3：createIsolatedRun 建立隔离运行目录与 run-manifest，冻结输入与气象文件。
  // 步骤 4：后台线程生成运行时场景（实例边界筛选、预裁切、DEM 落地），复制到运行目录 input/。
  // 步骤 5：面元模式做规模预检；组装引擎命令行；spawn 引擎并建立超时、stdout/stderr 转发。
  // 步骤 6：FacetEB 逐节点出图握手（FACET_STEP/FACET_ACK）；引擎退出后 completeHistreamRun 做
  //         结果后处理（面元 JSON → TIFF 或 ENVI → TIFF）、写最终状态并发 closed 事件。
  // 失败：任何一步抛错都会清理运行时场景、终止已启动的进程，并把清单标为 failed。
  if (request.method === 'POST' && url.pathname === '/api/run') {
    let runtimeScene = null
    let run = null
    let ownsPreparation = false
    try {
      const data = await body(request)
      // 步骤 1：互斥检查。
      if (resettingProcesses) throw new Error('模拟环境正在重置，请稍后再运行')
      if (child) throw new Error('已有模拟任务正在运行')
      // 准备阶段含异步哈希/复制，期间第二个 /api/run 不能穿过上面的 child 检查。
      if (runPreparing) throw new Error('已有模拟任务正在准备')
      runPreparing = ownsPreparation = true
      if (!modes.has(data.mode)) throw new Error('不支持的计算模式')
      const input = resolveProjectFile(data.inputPath || projectFile)
      projectFile = input
      projectDir = dirname(input)
      const stored = readProject(input)
      // Running must not migrate or rewrite the canonical source project.
      // Normalize path storage only in this invocation's in-memory copy.
      const project = stored.project ? normalizeStoredProjectPaths(stored.project, projectDir) : null
      if (!project) throw new Error('无法读取工程配置')
      // 步骤 2：引擎路径只允许白名单内的 histream 可执行文件。
      const engine = allowedEngine(data.executable)
      if (!existsSync(engine)) throw new Error(`找不到 HiStream：${windowsPath(engine)}`)
      // 步骤 3：为本次运行创建独立运行目录并写 run-manifest（输入文件身份/哈希等细节见 createIsolatedRun）；
      // 随后把运行用工程副本的 outDir 与气象路径改写为该运行目录内的路径，使每次运行的输出互不覆盖。
      run = await createIsolatedRun(input, project, data.mode, engine)
      const runProject = structuredClone(project)
      runProject.configuration.outDir = run.outputDir
      if (run.manifest.meteorology) runProject.configuration.meteo.path = run.meteoPath
      // 读 OBJ、实例筛选与预裁切、DEM 高程在后台线程执行（暂存目录由线程写出，cleanup 在本线程删除）。
      // 步骤 4：生成运行时场景；暂存目录名必须以 .histream-runtime- 开头且不在目标目录内，防止误复制。
      runtimeScene = await prepareRuntimeSceneInWorker(input, runProject, projectDir)
      // Preserve generated placements and clipped OBJ files with the input;
      // cleanup removes only the temporary staging directory.
      const inputDirectory = join(run.outputDir, 'input')
      const stagingDirectory = dirname(runtimeScene.inputPath)
      if (!basename(stagingDirectory).startsWith('.histream-runtime-') || pathWithin(stagingDirectory, inputDirectory)) {
        throw new Error('运行输入暂存目录无效或复制目标位于暂存目录内')
      }
      // 裁切后的 OBJ 与纹理可能很大：异步复制，避免长时间阻塞事件循环（SSE 进度、其他请求）。
      await cpAsync(stagingDirectory, inputDirectory, { recursive: true })
      const runInput = join(inputDirectory, 'project.json')
      if (run.manifest.meteorology) {
        const frozenMeteo = join(inputDirectory, 'meteo.txt')
        await copyFileAsync(run.meteoPath, frozenMeteo)
        const frozenProject = JSON.parse(readFileSync(runInput, 'utf8'))
        frozenProject.configuration.meteo.path = frozenMeteo
        writeFileSync(runInput, stringifyProject(frozenProject), 'utf8')
        run.manifest.meteorology.snapshot = await fileIdentityAsync(frozenMeteo)
        if (run.manifest.meteorology.snapshot?.sha256 !== run.manifest.meteorology.sha256) {
          throw new Error(`气象文件在准备运行期间被修改，本轮已取消：${windowsPath(run.meteoPath)}`)
        }
      }
      writeRunManifest(run, { runtimeInput: await fileIdentityAsync(runInput), inputDirectory })
      emit({
        type: 'stdout',
        text: `实例边界筛选：原始 ${runtimeScene.total.toLocaleString('zh-CN')} 个，保留 ${runtimeScene.kept.toLocaleString('zh-CN')} 个，完整包围盒在域外跳过 ${runtimeScene.excluded.toLocaleString('zh-CN')} 个；跨界对象仅计算域内部分${runtimeScene.clippedObjects ? `；预裁切 ${runtimeScene.clippedObjects} 个单实例 OBJ，三角面 ${runtimeScene.sourceTriangles.toLocaleString('zh-CN')} → ${runtimeScene.retainedTriangles.toLocaleString('zh-CN')}` : ''}${runtimeScene.invalid ? `，忽略无效记录 ${runtimeScene.invalid.toLocaleString('zh-CN')} 条` : ''}${runtimeScene.grounded ? `；${runtimeScene.grounded} 个车辆实例已落地` : ''}`
      })
      // 步骤 5：向前端输出准备阶段摘要；面元模式（FacetRT/FacetEB）另做三角面规模预检。
      const facetMode = data.mode === 'eFacetRT' || data.mode === 'eFacetEB'
      for (const line of runtimeScene.textureSummary || []) emit({ type: 'stdout', text: `表面分类/开孔率纹理：${line}` })
      if (facetMode) {
        // 统计三角面数与面元/像元比需要整读 OBJ，在后台线程执行。
        const { estimate, pixelNote } = await heavyTasks.run('facet-scale', { project: runtimeScene.project, inputDirectory, mode: data.mode })
        emit({ type: 'stdout', text: `面元规模预检：约 ${estimate.total.toLocaleString('zh-CN')} 个三角面` })
        if (pixelNote) emit({ type: 'stdout', text: pixelNote })
      }
      const projectSource = stringifyProject(project)
      const referencedMaterials = new Set([
        project.configuration.scene?.background?.materialName,
        ...(project.configuration.objects?.items || []).flatMap(object =>
          [object.materialName, ...(object.meshes || []).map(mesh => mesh.materialName)])
      ])
      // 是否保留三维结果 JSON（facetrt.json/faceteb.json）：用户启用了过程输出，或 FacetEB 中含被引用的
      // 光伏材质时保留；否则面元结果写到 .histream-transient-<mode>.json，出图后删除。
      // PV process files are unconditional and also need persistent geometry,
      // even when the optional radiation/energy process switches are off.
      const keepThreeDimensionalResults = processOutputEnabled(projectSource, data.mode)
        || (data.mode === 'eFacetEB' && project.configuration.materials.some(material =>
          material.energyModel === 'photovoltaic' && referencedMaterials.has(material.name)))
      // 引擎命令行：面元模式 = [mode, 工程, 结果 JSON]，其余模式 = [mode, 工程]；
      // 路径经 histreamRuntimePath 转成引擎能识别的形式。
      let args, radiosityJsonPath = ''
      if (facetMode) {
        const outputDirectory = run.outputDir
        mkdirSync(outputDirectory, { recursive: true })
        const resultName = data.mode === "eFacetRT" ? "facetrt.json" : "faceteb.json"
        radiosityJsonPath = join(outputDirectory, keepThreeDimensionalResults ? resultName : ".histream-transient-" + data.mode + ".json")
        args = [data.mode, histreamRuntimePath(runInput, engine),
          histreamRuntimePath(radiosityJsonPath, engine)]
      } else {
        args = [data.mode, histreamRuntimePath(runInput, engine)]
      }
      const startedAt = Date.now()
      // 启动引擎。FacetEB 使用管道 stdin 与引擎握手（HISTREAM_FACET_STEP_SYNC=1，引擎每个节点后等待 ACK），
      // 其他模式不需要 stdin。running.histreamRun 保存运行标识，供 /api/events 状态与停止/重置使用。
      const workingDirectory = facetMode ? dirname(engine) : projectDir
      const streamFacetSteps = data.mode === 'eFacetEB'
      child = spawn(engine, args, { cwd: workingDirectory, windowsHide: true,
        env: { ...process.env, HISTREAM_FACET_STEP_SYNC: streamFacetSteps ? '1' : '0' },
        stdio: [streamFacetSteps ? 'pipe' : 'ignore', 'pipe', 'pipe'] })
      childExecutable = engine
      const running = child
      running.histreamRun = { runId: run.runId, outputDir: windowsPath(run.outputDir), startedAt, mode: data.mode }
      writeRunManifest(run, { status: 'running', pid: running.pid, command: [engine, ...args] })
      // 超时计时器：runTimeRemaining 记录剩余毫秒；pauseRunTimer/resumeRunTimer 在“节点出图”期间暂停计时，
      // 使超时只统计引擎自身执行时间。到点用 taskkill /T /F 终止整棵进程树。
      // 单次运行硬超时：到点终止整棵进程树，并按超时结束（结果不完整）。
      // 只计引擎执行时间：FacetEB 逐节点出图期间引擎阻塞等待 FACET_ACK，
      // 这段结果后处理时间暂停计时（与用户手册“结果后处理不计入”一致）。
      const runTimeoutMs = projectRunTimeoutMs(project)
      let runTimer = null, runTimerStartedAt = 0, runTimeRemaining = runTimeoutMs
      const onRunTimeout = () => {
        runTimer = null
        if (running.histreamTimedOut) return
        running.histreamTimedOut = true
        emit({ type: 'error', text: `运行超过 ${runTimeoutLabel(runTimeoutMs)}，已终止` })
        try { spawn('taskkill', ['/pid', String(running.pid), '/T', '/F'], { windowsHide: true }).on('error', () => running.kill('SIGKILL')) }
        catch { try { running.kill('SIGKILL') } catch {} }
      }
      const resumeRunTimer = () => {
        if (runTimer || running.histreamTimedOut || running.histreamFinished) return
        runTimerStartedAt = Date.now()
        runTimer = setTimeout(onRunTimeout, Math.max(0, runTimeRemaining))
        if (typeof runTimer.unref === 'function') runTimer.unref()
      }
      const pauseRunTimer = () => {
        if (!runTimer) return
        clearTimeout(runTimer)
        runTimer = null
        runTimeRemaining -= Date.now() - runTimerStartedAt
      }
      resumeRunTimer()
      emit({ type: 'started', pid: running.pid, runId: run.runId, outputDir: windowsPath(run.outputDir), command: `"${windowsPath(engine)}" ${args.map((item) => `"${windowsPath(item)}"`).join(' ')}` })
      running.stdout.setEncoding('utf8')
      running.stderr.setEncoding('utf8')
      // engine:true marks raw engine stdout; the client re-assembles lines
      // split across chunks before parsing PROGRESS / diagnostics records.
      running.stdout.on('data', (chunk) => emit({ type: 'stdout', text: chunk.toString(), engine: true }))
      running.stderr.on('data', (chunk) => emit({ type: 'stderr', text: chunk.toString() }))
      // FacetEB 逐节点出图状态：已出图节点数、第一个出图错误、是否有节点正在处理（不允许并发）。
      let streamedSteps = 0, stepOutputError = null, stepPending = false
      const nodeDiagnostics = new Map()
      // FacetEB 握手协议（逐行读 stdout）：
      //   FACET_CONVERGENCE<TAB>节点<TAB>是否收敛<TAB>迭代数<TAB>温度变化<TAB>能量残差<TAB>等效温度残差
      //   FACET_STEP<TAB>节点<TAB>时间标记(DOYddd_hh-mm)<TAB>儒略时间[<TAB>太阳天顶角<TAB>太阳方位角]
      // 收到 FACET_STEP 后：暂停超时计时 → 校验冻结输入 → exportFacetStep 逐方向写 TIFF →
      // 更新清单 → 恢复计时 → 向 stdin 写 FACET_ACK<TAB>节点 让引擎继续；
      // 出错则写 FACET_ERROR 让引擎中止（已写出的节点文件保留）。
      if (streamFacetSteps) {
        // readline handles split/coalesced stdout chunks and CRLF safely.
        const lines = createInterface({ input: running.stdout, crlfDelay: Infinity })
        running.stdin.on('error', () => { /* Engine may be stopped during output. */ })
        lines.on('line', async (line) => {
          if (line.startsWith('FACET_CONVERGENCE\t')) {
            const [, node, converged, iterations, temperatureDelta, energyResidual, energyResidualTemperatureEquivalent] = line.split('\t')
            nodeDiagnostics.set(Number(node), { node: Number(node), converged: converged === '1', iterations: Number(iterations),
              temperatureDelta: Number(temperatureDelta), energyResidual: Number(energyResidual), energyResidualTemperatureEquivalent: Number(energyResidualTemperatureEquivalent) })
            return
          }
          if (!line.startsWith('FACET_STEP\t') || running.histreamReset || running.histreamStopped) return
          // The engine now blocks until FACET_ACK: pause the run timeout while
          // this node is post-processed and resume it once the engine continues.
          pauseRunTimer()
          try {
            const match = /^FACET_STEP\t(\d+)\t(DOY\d+_\d{2}-\d{2})\t([\d.eE+-]+)(?:\t([\d.eE+-]+)\t([\d.eE+-]+))?$/.exec(line)
            if (!match || stepPending) throw new Error('面元节点输出协议异常')
            const step = { node: Number(match[1]), token: match[2], julianTime: Number(match[3]) }
            if (match[4] !== undefined) Object.assign(step, { solarZenith: Number(match[4]), solarAzimuth: Number(match[5]) })
            if (!Number.isFinite(step.julianTime)) throw new Error('面元节点时间无效')
            stepPending = true
            emit({ type: 'stdout', text: `节点 ${step.node + 1} 已计算，正在逐方向保存图像：${step.token}` })
            verifyFrozenRunInputs(run.manifest, runInput)
            const exportStartedAt = Date.now()
            const result = await exportFacetStep(running, radiosityJsonPath, runInput, step)
            if (running.histreamReset || running.histreamStopped || running.exitCode !== null) return
            streamedSteps += 1
            stepPending = false
            run.manifest.completed.nodes.push({ ...step, ...nodeDiagnostics.get(step.node) })
            run.manifest.completed.nodeCount = run.manifest.completed.nodes.length
            writeRunManifest(run, { completed: { ...run.manifest.completed, products: runProductPaths(run.outputDir) } })
            emit({ type: 'stdout', text: `节点 ${step.node + 1} 已保存 ${result.tifPaths.length} 个 TIFF（出图 ${((Date.now() - exportStartedAt) / 1000).toFixed(2)} s），输出缓存已释放：${step.token}` })
            resumeRunTimer()
            running.stdin.write(`FACET_ACK\t${step.node}\n`)
          } catch (error) {
            if (running.histreamReset || running.histreamStopped) return
            stepOutputError = error
            emit({ type: 'error', text: `节点影像生成失败（已保留节点文件）：${error.message}` })
            // The engine aborts on FACET_ERROR; keep the timeout armed for its shutdown.
            resumeRunTimer()
            running.stdin.end('FACET_ERROR\n')
          }
        })
      }
      running.on('error', (error) => {
        runtimeScene?.cleanup()
        try { writeRunManifest(run, { status: 'failed', finishedAt: new Date().toISOString(), error: error.message }) } catch {}
        if (!running.histreamReset) emit({ type: 'error', text: error.message })
        if (!running.pid && child === running) { child = null; childExecutable = '' }
      })
      // 引擎退出后的收尾流程（close 事件触发，异步执行；期间 child 仍指向本次运行，界面保持“运行中”）：
      // 步骤 1：被“重置”终止则只写 stopped 并清理。
      // 步骤 2：面元模式且无逐节点出图时，在一次性后台线程把面元 JSON 转成 TIFF；
      // 步骤 3：其他模式把 ENVI 结果转成 TIFF（逐文件报告失败，全部失败才算失败）；
      // 步骤 4：汇总节点完成情况，决定最终状态 completed / non_converged / stopped / timeout / failed，
      //         写入清单并发 closed 事件，最后清理运行时场景。
      const completeHistreamRun = async (code, signal) => {
        running.histreamFinished = true
        pauseRunTimer()
        if (running.facetWorker) void running.facetWorker.terminate()
        const finishReset = () => {
          writeRunManifest(run, { status: 'stopped', finishedAt: new Date().toISOString(), completed: { ...run.manifest.completed, products: runProductPaths(run.outputDir) } })
          runtimeScene?.cleanup()
          if (child === running) child = null
          if (!child) childExecutable = ''
        }
        if (running.histreamReset) return finishReset()
        let finalCode = stepOutputError ? 1 : code
        if (running.histreamTimedOut && (finalCode === null || finalCode === undefined)) finalCode = 1
        // 退出码语义见 HISTREAM_EXIT 与 histreamResultCodes；有结果的退出码才做后处理。
        // Codes that still carry results: 0, plus 2 for FacetEB only (non-converged).
        const resultCodes = histreamResultCodes(data.mode)
        if (code === HISTREAM_EXIT.usage && !stepOutputError) {
          stepOutputError = new Error(`引擎参数用法错误（退出码 ${HISTREAM_EXIT.usage}）：请检查引擎版本是否支持模式 ${data.mode}`)
          emit({ type: 'error', text: stepOutputError.message })
        }
        if (resultCodes.includes(code) && radiosityJsonPath && !streamedSteps && !stepOutputError) {
          try {
            verifyFrozenRunInputs(run.manifest, runInput)
            // 读面元 JSON 与出图在一次性后台线程中执行，期间其他请求照常响应；“停止”会终止该线程。
            const tif = await runPostprocessTask(running, 'facet-tiff', { jsonPath: radiosityJsonPath, inputPath: runInput, mode: data.mode })
            if (tif.tifPaths.length) {
              const timing = isEnergyBalanceMode(data.mode) ? ' 个逐时间 TIFF' : ' 个 TIFF'
              emit({ type: 'stdout', text: '已生成 ' + tif.tifPaths.length + timing + '（' + tif.width + ' × ' + tif.height + '，' + tif.bands + ' 波段）' })
            }
            if (tif.processPaths.length) emit({ type: 'stdout', text: '已保存 ' + tif.processPaths.length + ' 个时间节点索引' })
          } catch (error) {
            finalCode = 1
            // 结果处理失败也要写进清单，否则运行只显示 failed 而没有原因。
            stepOutputError = running.histreamStopped ? new Error('结果影像生成已被停止') : error
            if (!running.histreamReset) emit({ type: 'error', text: running.histreamStopped ? stepOutputError.message : `结果影像生成失败：${stepOutputError.message}` })
          } finally {
            if (!keepThreeDimensionalResults && existsSync(radiosityJsonPath)) unlinkSync(radiosityJsonPath)
          }
          if (running.histreamReset) return finishReset()
        }
        if (streamedSteps && resultCodes.includes(code) && !keepThreeDimensionalResults && existsSync(radiosityJsonPath)) unlinkSync(radiosityJsonPath)
        // 步骤 3：非面元模式（体元/射线追踪）的结果为 ENVI，需要转换为 TIFF。
        let conversionWarnings = []
        if (resultCodes.includes(finalCode) && !radiosityJsonPath) {
          try {
            // ENVI→TIFF failures are reported per file instead of being dropped
            // silently; the run keeps its status unless no image converted at all.
            // 转换在一次性后台线程中执行。
            const conversion = await runPostprocessTask(running, 'envi-convert', { directory: run.outputDir, mode: data.mode })
            conversionWarnings = conversion.failures.map(({ file, reason }) => `${file}：${reason}`)
            for (const warning of conversionWarnings) emit({ type: 'error', text: `ENVI 结果转换 TIFF 失败（原始文件已保留）：${warning}` })
            if (conversion.failures.length && !conversion.converted.length) {
              throw new Error(`全部 ${conversion.failures.length} 个 ENVI 结果均未能转换为 TIFF`)
            }
          }
          catch (error) { finalCode = 1; stepOutputError = running.histreamStopped ? new Error('ENVI 结果转换已被停止') : error }
          if (running.histreamReset) return finishReset()
        }
        // 步骤 4：引擎自己写的 faceteb_run.json（已完成节点）优先；
        // 没有时，非逐节点握手且退出码为 0 的运行，把请求的节点全部视为由退出码确认完成。
        let nativeRun = {}
        try { nativeRun = JSON.parse(readFileSync(join(run.outputDir, 'faceteb_run.json'), 'utf8')) } catch {}
        if (nativeRun.completed && resultCodes.includes(finalCode)) run.manifest.completed.nodes = nativeRun.completed
        if (nativeRun.completed) run.manifest.computedNodes = nativeRun.completed
        if (!nativeRun.completed && finalCode === 0 && !streamFacetSteps) run.manifest.completed.nodes = run.manifest.requested.nodes.map(node => ({ node, completionSource: 'engine-exit' }))
        run.manifest.completed.nodeCount = run.manifest.completed.nodes.length || (finalCode === 0 && !isEnergyBalanceMode(data.mode) ? 1 : 0)
        // 状态优先级：超时 > 用户停止 > FacetEB 未收敛（退出码 2，结果仍有效）> 成功 > 失败。
        const nonConverged = data.mode === 'eFacetEB' && finalCode === HISTREAM_EXIT.facetNonConverged
        const status = running.histreamTimedOut ? 'timeout' : running.histreamStopped ? 'stopped' : nonConverged ? 'non_converged' : finalCode === 0 ? 'completed' : 'failed'
        writeRunManifest(run, { status, finishedAt: new Date().toISOString(), exitCode: finalCode, signal,
          ...(running.histreamTimedOut ? { error: `运行超过 ${runTimeoutLabel(runTimeoutMs)}，已终止` } : {}),
          ...(conversionWarnings.length ? { warnings: conversionWarnings } : {}),
          ...(stepOutputError ? { error: stepOutputError.message } : {}), completed: { ...run.manifest.completed, products: runProductPaths(run.outputDir) } })
        emit({ type: 'closed', code: finalCode, signal, status, runId: run.runId, outputDir: windowsPath(run.outputDir), elapsed: Date.now() - startedAt,
          ...(conversionWarnings.length ? { warnings: conversionWarnings } : {}) })
        runtimeScene?.cleanup()
        if (child === running) { child = null; childExecutable = '' }
      }
      // close 在所有 stdio 关闭后触发；后处理异常统一转成 error 事件，并保证 child 引用被释放。
      running.on('close', (code, signal) => {
        // 结果后处理在后台线程中进行；完成前 child 仍指向本次运行（界面显示运行中，拒绝新的运行与删除结果）。
        completeHistreamRun(code, signal)
          .catch((error) => { try { emit({ type: 'error', text: `结果处理失败：${error.message}` }) } catch {} })
          .finally(() => { if (child === running) { child = null; childExecutable = '' } })
      })
      return json(response, 200, { pid: running.pid, inputPath: windowsPath(input), runId: run.runId, outputDir: windowsPath(run.outputDir) })
    } catch (error) {
      runtimeScene?.cleanup()
      // 启动流程中途失败：若本次运行的引擎已经起来，先终止其进程树并标记 stopped，避免遗留后台进程。
      if (child && run && child.histreamRun?.runId === run.runId) {
        const stale = child
        child = null
        childExecutable = ''
        stale.histreamStopped = true
        try { spawn('taskkill', ['/pid', String(stale.pid), '/T', '/F'], { windowsHide: true }).on('error', () => stale.kill()) } catch {}
      }
      if (run) try { writeRunManifest(run, { status: 'failed', finishedAt: new Date().toISOString(), error: error.message }) } catch {}
      return json(response, 400, { error: error.message })
    } finally {
      if (ownsPreparation) runPreparing = false
    }
  }
  // 停止当前运行：标记 histreamStopped（后处理据此跳过/报告“已停止”）并终止面元出图线程；
  // 引擎已退出、只剩后处理时不再按 PID 杀进程（PID 可能已被复用）；否则 Windows 用 taskkill /T /F。
  if (request.method === 'POST' && url.pathname === '/api/stop') {
    if (!child) return json(response, 200, { stopped: false })
    child.histreamStopped = true
    if (child.facetWorker) void child.facetWorker.terminate()
    const stopping = child
    // 引擎已退出、只剩后台结果处理：终止处理线程即可，不再按（可能已被复用的）PID 结束进程。
    if (stopping.histreamFinished) return json(response, 200, { stopped: true })
    if (process.platform === 'win32') {
      spawn('taskkill', ['/pid', String(stopping.pid), '/T', '/F'], { windowsHide: true })
        .on('error', () => stopping.kill())
        .on('close', (code) => { if (code !== 0) stopping.kill() })
    } else stopping.kill('SIGTERM')
    return json(response, 200, { stopped: true })
  }
  // 重置模拟环境：终止遗留的引擎进程（见 resetSimulationProcesses）。
  if (request.method === 'POST' && url.pathname === '/api/reset') {
    try {
      return json(response, 200, await resetSimulationProcesses())
    } catch (error) {
      return json(response, 500, { error: error.message })
    }
  }
  // 删除结果：data.kind === '3d' 删三维结果，否则删 TIFF 与统计表。
  // 运行中拒绝；先清空结果缓存并通知各后台线程清缓存，避免继续引用已删除文件。
  if (request.method === 'POST' && url.pathname === '/api/results/delete-all') {
    try {
      if (child) throw new Error('模拟运行中，不能删除结果')
      resultCache.clear()
      heavyTasks.broadcast({ type: 'clear' })
      const data = await body(request)
      return json(response, 200, data.kind === "3d" ? deleteThreeDimensionalResults(data.path) : deleteTiffResults(data.path))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 列出结果目录内容（listResults，递归 runs/），在后台线程执行。
  // DEFERRED[低] 安全: 可以列出任意目录（并递归 runs/），会泄露文件名和元数据；建议：和读取接口一样限制在工程/运行结果目录内。
  if (request.method === 'POST' && url.pathname === '/api/results/list') {
    try {
      const data = await body(request)
      // 递归遍历 runs/ 并读取每个结果文件头，在后台线程执行。
      return json(response, 200, toJsonBytes(await heavyTasks.run('results-list', { path: resolveAsset(data.path) })))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 读取一个结果文件，按扩展名分派：
  //   .hdr → ENVI；.tif/.tiff → TIFF（fluid_wind_ 开头的按风场读取）；
  //   面元结果 .json → facet；过程结果 .json → process（依赖多个文件）；
  //   图像 → 以 base64 返回（<= 32MB）；文本类（<= 4MB）→ 文本，统计 CSV 附带观测条件；其余只回路径。
  // TIFF/ENVI/面元/过程经 sendCachedResult 走结果缓存，variant 区分波段或类型。
  // DEFERRED[中] 安全: resolveAsset 接受任意绝对路径，这里（以及 /api/results/change）能读取磁盘上任何 .txt/.csv/.json/.log/.dat/图像/TIFF，不像 /api/project/read 那样用 realPathWithin 限制范围；建议：限制在 projectDir 或 resultDirectoryAllowed 认可的目录内。（大面元 JSON/TIFF 的读取解析已于 2026-11-23 移到后台计算线程。）
  if (request.method === 'POST' && url.pathname === '/api/results/read') {
    try {
      const data = await body(request)
      const path = resolveAsset(data.path)
      if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`找不到模拟结果：${path}`)
      const extension = extname(path).toLowerCase()
      const band = Math.max(0, Math.floor(Number(data.band) || 0))
      if (extension === '.hdr') {
        return await sendCachedResult(request, response, [path, path.slice(0, -extension.length) + '.img'], `envi:${band}`, { kind: 'envi', path, band })
      }
      if (extension === '.tif' || extension === '.tiff') {
        const wind = /^fluid_wind_/i.test(basename(path))
        return await sendCachedResult(request, response, [path], wind ? 'wind' : `tiff:${band}`, { kind: wind ? 'wind' : 'tiff', path, band })
      }
      if (extension === '.json' && isRadiosityResultFile(path)) {
        return await sendCachedResult(request, response, [path], 'facet', { kind: 'facet', path, band })
      }
      const processMetadata = extension === '.json' ? processResultMetadata(path) : null
      if (processMetadata) {
        return await sendCachedResult(request, response, processResultDependencies(path, processMetadata), 'process', { kind: 'process', path, band })
      }
      if (resultImageMime[extension]) {
        if (statSync(path).size > 32 * 1024 * 1024) throw new Error('图像结果超过 32MB，请在资源管理器中打开')
        return json(response, 200, toJsonBytes(await heavyTasks.run('result-image', { path, mimeType: resultImageMime[extension] })))
      }
      if (['.txt', '.csv', '.dat', '.log', '.json'].includes(extension)) {
        if (statSync(path).size > 4 * 1024 * 1024) throw new Error('文本结果超过 4MB，请在外部程序中打开')
        return json(response, 200, { kind: 'text', path: windowsPath(path), content: readFileSync(path, 'utf8'),
          ...(/^result_statistics(?:_[a-z0-9-]+)?\.csv$/i.test(basename(path))
            ? { observation: resultObservationContext(path) } : {}) })
      }
      return json(response, 200, { kind: 'file', path: windowsPath(path) })
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 变化分析：对两幅 TIFF 做逐像元差异（compareTiffResults），在后台线程执行，以第一幅路径为 key 串行。
  if (request.method === 'POST' && url.pathname === '/api/results/change') {
    try {
      const data = await body(request)
      const firstPath = resolveAsset(data.firstPath)
      const secondPath = resolveAsset(data.secondPath)
      for (const path of [firstPath, secondPath]) {
        if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`找不到模拟结果：${path}`)
        if (!['.tif', '.tiff'].includes(extname(path).toLowerCase())) throw new Error('变化分析目前仅支持 TIFF 图像')
      }
      return json(response, 200, toJsonBytes(await heavyTasks.run('results-change',
        { firstPath, secondPath, band: data.band, maxScatterPoints: data.maxScatterPoints }, { key: firstPath })))
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }
  // 在资源管理器中打开路径（文件则选中）；拒绝 UNC 网络路径；仅 Windows 实际打开。
  if (request.method === 'POST' && url.pathname === '/api/open') {
    const data = await body(request)
    const requested = normalizeHostPath(data.path || projectDir || envSetting('STREAM_STUDIO_DATA_ROOT', 'HISTREAM_STUDIO_DATA_ROOT') || PROJECT_ROOT)
    if (/^[\\/]{2}/.test(requested)) return json(response, 400, { error: '不支持打开网络（UNC）路径' })
    const path = resolve(requested)
    if (/^[\\/]{2}/.test(path)) return json(response, 400, { error: '不支持打开网络（UNC）路径' })
    if (!existsSync(path)) return json(response, 400, { error: `找不到路径：${path}` })
    if (process.platform === 'win32') spawn('explorer.exe', statSync(path).isFile() ? ['/select,', path] : [path], { detached: true, windowsHide: true }).on('error', () => {}).unref()
    return json(response, 200, { ok: true, path: windowsPath(path) })
  }
  return false
}

// ---------- 后台计算任务（在线程池/一次性线程中执行；线程不可用时在服务线程内同步执行） ----------
// 后台任务的结果包装：工作线程把结果预先序列化成 JSON 字节返回（{ bytes }），
// 主线程用 toJsonBytes 包成 JsonBytes 由 json() 直接发送，避免在主线程再次 JSON.stringify 大对象。
function jsonPayload(value) {
  return { bytes: Buffer.from(JSON.stringify(value), 'utf8') }
}

function toJsonBytes(value) {
  return new JsonBytes(receivedBytes(value?.bytes))
}

let workerRasterTools = null
const rasterTools = () => (workerRasterTools ||= createRasterTools({ readTiff, normalizeHostPath, windowsPath }))

// 模块：后台任务表。key 是 heavyTasks.run(名称, 参数) 的任务名，值是在线程池（task-service）或
// 一次性线程（task-once）里执行的函数；线程不可用时 runHeavyTaskInline 在服务线程内同步执行。
// 约定：参数与返回值都必须可被结构化克隆（不含函数）；大结果用 jsonPayload 预序列化。
// 分类：结果读取/解码（result-read、result-image、results-change、results-list）、DEM 检查、
//       面元出图与 ENVI 转换、运行时场景准备、USD 导出、OBJ 导入、STREAM 栅格统计等。
const HEAVY_TASKS = {
  'parse-json': ({ bytes }) => JSON.parse(receivedBytes(bytes).toString('utf8') || '{}'),
  'result-read': ({ kind, path, band, binary }) => encodeResultPayload(produceResult(kind, path, band), binary),
  'result-image': ({ path, mimeType }) => jsonPayload({ kind: 'image', path: windowsPath(path), mimeType, data: readFileSync(path).toString('base64') }),
  'results-change': ({ firstPath, secondPath, band, maxScatterPoints }) => jsonPayload(compareTiffResults(firstPath, secondPath, band, maxScatterPoints)),
  'project-read': ({ path }) => jsonPayload({ path: windowsPath(path), content: readFileSync(path, 'utf8') }),
  'dem-info': ({ path, extension }) => jsonPayload({ path: windowsPath(path), metadata: inspectDemFile(path, extension) }),
  'dem-inspect': ({ path, extension }) => inspectDemFile(path, extension),
  'tarp-cover': ({ target, outputPath, lift, drape }) => {
    const cover = generateTarpCover(readFileSync(target, 'utf8'), { lift, drape })
    mkdirSync(dirname(outputPath), { recursive: true })
    writeFileSync(outputPath, cover.obj, 'utf8')
    const { obj, ...summary } = cover
    return summary
  },
  'facet-tiff': ({ jsonPath, inputPath, mode }) => writeRadiosityTiff(jsonPath, inputPath, mode),
  'envi-convert': ({ directory, mode }) => convertEnviResults(directory, mode),
  'stream-raster': ({ path, band }) => jsonPayload(rasterTools().rasterBand(path, band)),
  'stream-class-stats': ({ outputDir, typeFile, typeBand }) => jsonPayload(rasterTools().classStatistics(outputDir, typeFile, typeBand)),
  'run-preflight': ({ input, project, mode }) => jsonPayload(runPreflight(input, project, mode)),
  'results-list': ({ path }) => jsonPayload(listResults(path)),
  'runtime-scene': ({ input, project, baseDir }) => {
    const { cleanup, ...prepared } = prepareRuntimeSceneProject(input, project, {
      resolveAssetPath: (value) => projectAssetPath(value, baseDir),
      terrainHeight: demHeightSampler(project.configuration, baseDir)
    })
    return prepared
  },
  'facet-scale': ({ project, inputDirectory, mode }) => ({
    estimate: assertFacetScale(project, inputDirectory, mode),
    pixelNote: facetPixelRatioNote(project, inputDirectory)
  }),
  'usd-export': ({ path, project }) => exportProjectUsdFiles(path, project),
  // 导入 OBJ（<= 80MB）：先清洗内容（sanitizeObjContent），写到 models/（重名加 _2、_3 …），
  // 并在 positions/ 里为它生成默认单实例位置文件 `0 0 0 1 0`（x y z scale rotation），已有则不覆盖。
  'import-obj': ({ projectDir: directory, sourceName, extension, content: source }) => {
    const content = sanitizeObjContent(source)
    if (!content.trim()) throw new Error('OBJ 文件为空')
    if (Buffer.byteLength(content, 'utf8') > 80 * 1024 * 1024) throw new Error('OBJ 文件超过 80MB，请使用简化模型')
    const modelsDir = join(directory, 'models')
    mkdirSync(modelsDir, { recursive: true })
    const stem = sourceName.slice(0, -extension.length).replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '') || 'object'
    let name = `${stem}${extension}`
    for (let suffix = 2; existsSync(join(modelsDir, name)); suffix += 1) name = `${stem}_${suffix}${extension}`
    const path = join(modelsDir, name)
    const positionsDir = join(directory, 'positions')
    mkdirSync(positionsDir, { recursive: true })
    const positionPath = join(positionsDir, `${name.slice(0, -4)}_position.txt`)
    writeFileSync(path, content, 'utf8')
    if (!existsSync(positionPath)) writeFileSync(positionPath, '0 0 0 1 0\n', 'utf8')
    return { path: windowsPath(path), name, positionPath: windowsPath(positionPath), relativePath: join('models', name).replaceAll('\\', '/') }
  }
}

// 安全：cleanup 只在目录名以 .histream-runtime- 开头时才删除，且只删其中的普通文件，
// 再 rmdir 空目录，避免误删其他内容。
// 运行暂存目录由后台线程写出；cleanup 删除其中文件与目录本身（与 prepareRuntimeSceneProject 的 cleanup 相同）。
async function prepareRuntimeSceneInWorker(input, project, baseDir) {
  const prepared = await heavyTasks.run('runtime-scene', { input, project, baseDir }, { key: input })
  const runtimeDirectory = dirname(prepared.inputPath)
  const cleanup = () => {
    if (!basename(runtimeDirectory).startsWith('.histream-runtime-')) return
    try {
      for (const entry of readdirSync(runtimeDirectory, { withFileTypes: true })) {
        if (entry.isFile()) { try { unlinkSync(join(runtimeDirectory, entry.name)) } catch {} }
      }
    } catch {}
    try { if (existsSync(runtimeDirectory)) rmdirSync(runtimeDirectory) } catch {}
  }
  return { ...prepared, cleanup }
}

// project.usd 导出（首次需整读并解析每个 OBJ）；同一工程目录固定到同一线程，保证先后保存按顺序写出。
function exportProjectUsdInWorker(path, project) {
  return heavyTasks.run('usd-export', { path, project }, { key: dirname(path) })
}

// 同步执行后台任务（降级路径）；任务名必须在 HEAVY_TASKS 自有属性内，防止原型链上的名称被调用。
function runHeavyTaskInline(task, args) {
  if (!Object.hasOwn(HEAVY_TASKS, task)) throw new Error(`未知的后台任务：${task}`)
  return HEAVY_TASKS[task](args || {})
}

// 模块：HTTP 服务器入口。
//   /api/*：先拒绝 Host/Origin 非本机、或非 GET/HEAD 且不是 application/json 的请求
//     （foreignRequestReason），再交给 api()；
//   其他路径：--api-only（开发模式）返回提示；否则从 GUI 构建目录提供静态文件，
//   路径必须在该目录内，找不到文件时回退到 index.html（单页应用路由）。
// 安全响应头：X-Content-Type-Options: nosniff 与禁止被嵌入框架（FRAME_HEADERS）。
const server = createServer(async (request, response) => {
  try {
    const url = new URL(request.url, 'http://127.0.0.1')
    if (url.pathname.startsWith('/api/')) {
      const rejection = foreignRequestReason(request)
      if (rejection) return json(response, 403, { error: rejection })
      const handled = await api(request, response, url)
      if (handled !== false) return
      return json(response, 404, { error: 'API 不存在' })
    }
    if (apiOnly) return json(response, 404, { error: '开发模式请访问 http://127.0.0.1:5173' })
    let path = resolve(GUI_OUTPUT_DIR, `.${decodeURIComponent(url.pathname)}`)
    if (!pathWithin(GUI_OUTPUT_DIR, path)) return json(response, 404, { error: '文件不存在' })
    if (!existsSync(path) || !statSync(path).isFile()) path = join(GUI_OUTPUT_DIR, 'index.html')
    if (!existsSync(path)) return json(response, 404, { error: '文件不存在' })
    response.writeHead(200, { 'Content-Type': mime[extname(path).toLowerCase()] || 'application/octet-stream', 'X-Content-Type-Options': 'nosniff', ...FRAME_HEADERS })
    createReadStream(path).on('error', () => response.destroy()).pipe(response)
  } catch (error) {
    if (response.headersSent) response.destroy()
    else json(response, 500, { error: error.message })
  }
})

// 启动与线程角色（本文件也会作为 worker 脚本被加载，由 workerData.kind 决定角色）：
//   主线程且未禁用自启：监听 127.0.0.1:<端口>（默认 4173）；桌面版把自启关掉，
//     由 desktop/server-process.mjs 用随机端口监听。
//   facet-step：一次性线程，导出单个面元节点 TIFF；
//   facet-step-service：常驻线程，几何只解析一次，结果文件未变化时复用缓存；
//   task-service / task-once：后台任务线程池 / 一次性任务线程，执行 HEAVY_TASKS。
const port = Number(envSetting('STREAM_STUDIO_PORT', 'HISTREAM_STUDIO_PORT') || 4173)
if (isMainThread && envSetting('STREAM_STUDIO_SERVER_AUTOSTART', 'HISTREAM_STUDIO_SERVER_AUTOSTART') !== '0') {
  server.once('error', (error) => {
    const detail = error.code === 'EADDRINUSE'
      ? `端口 ${port} 已被占用，请关闭占用该端口的程序，或设置 STREAM_STUDIO_PORT 使用其他端口`
      : error.message
    console.error(`STREAM STUDIO: 本地服务启动失败：${detail}`)
    process.exitCode = 1
  })
  server.listen(port, '127.0.0.1', () => console.log(`STREAM STUDIO: http://127.0.0.1:${apiOnly ? 5173 : server.address().port}`))
}
else if (workerData?.kind === 'facet-step') {
  parentPort.postMessage(writeRadiosityTiff(workerData.jsonPath, workerData.inputPath, 'eFacetEB', workerData.step))
  parentPort.close()
}
// 缓存判据：路径、大小、修改时间都未变才复用已解析的几何，否则重新读取。
else if (workerData?.kind === 'facet-step-service') {
  // Geometry is parsed once and reused while the result file is unchanged.
  let cache = null
  parentPort.on('message', ({ jsonPath, inputPath, step }) => {
    try {
      const info = statSync(jsonPath)
      if (!cache || cache.path !== jsonPath || cache.size !== info.size || cache.mtimeMs !== info.mtimeMs) {
        cache = null
        cache = { path: jsonPath, size: info.size, mtimeMs: info.mtimeMs, geometry: readRadiosityResult(jsonPath, true) }
      }
      parentPort.postMessage({ ok: true, result: writeRadiosityTiff(jsonPath, inputPath, 'eFacetEB', step, cache.geometry) })
    } catch (error) {
      parentPort.postMessage({ ok: false, error: error?.message || String(error) })
    }
  })
}
else if (workerData?.kind === 'task-service') {
  resultCache.limit = Math.max(0, Number(workerData.cacheBytes) || 0)
  serveTasks(parentPort, HEAVY_TASKS, { onClear: () => resultCache.clear() })
}
else if (workerData?.kind === 'task-once') {
  resultCache.limit = 0
  void serveTaskOnce(parentPort, HEAVY_TASKS, workerData)
}

// 导出给桌面进程使用（desktop/server-process.mjs）：HTTP 服务对象与引擎停止函数。
export { server, stopEngines }

// 作用：停止所有后台工作：面元出图线程、后台线程池、正在运行的 histream 子进程（Windows 用 taskkill
// 终止整棵进程树，同步等待）以及 STREAM 服务；已经退出只剩后处理的运行不再杀进程。
function stopEngines() {
  if (child?.facetWorker) void child.facetWorker.terminate()
  heavyTasks.terminate()
  if (child && !child.histreamFinished) {
    child.histreamStopped = true
    if (process.platform === 'win32') {
      try { spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { windowsHide: true }) } catch { child.kill('SIGTERM') }
    } else child.kill('SIGTERM')
  }
  streamService.shutdown()
}

// 独立运行时对 SIGINT/SIGTERM 的处理：停止引擎，关闭服务器后退出进程。
function shutdown() {
  stopEngines()
  server.close(() => process.exit(0))
  // 事件流（SSE）长连接不会自己断开，不关掉 server.close 的回调永远不触发。
  server.closeAllConnections()
}
process.on('SIGINT', shutdown)
process.on('SIGTERM', shutdown)
