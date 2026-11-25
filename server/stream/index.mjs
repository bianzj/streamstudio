// ==========================================================================
// 文件：index.mjs
// 功能：STREAM 遥感影像模拟的服务端入口。提供 /api/stream/* 接口、串行任务队列、SSE 事件通道，
//   负责启动 stream.exe、管理运行输出目录和气象强迫数据包，与 HiStream 的运行机制互不共享
// 主要模块：
//   1. 引擎定位与状态：engine / dataDir / status / assertEngine；非 ASCII 数据包路径预检查
//   2. 串行任务队列：enqueue / pump / launch / finish / stop（子进程输出转 SSE 事件）
//   3. 影像运行：submitRun（提交期校验、入队、建输出目录并写配置与清单）、readRun、listRuns
//   4. 气象数据包：packSummary / listPacks / registerPack / removePack
//   5. 强迫元数据：runEngine（短调用）、listPresets / getPreset / checkForcing、
//      工程元数据的保存与读取
//   6. 浏览对话框 pick（Windows PowerShell）与路由表 routes / handle / shutdown
// 数据流：界面 → /api/stream/run → config.mjs 生成并校验引擎配置 → 写入 run-config.json →
//   spawn stream.exe run → stdout 进度行/日志经 SSE 推给界面 → 退出后写 run-manifest.json
//   并读取 scene_summary.json 作为结果
// 与其他文件的关系：config.mjs（工程与引擎配置）、forcing.mjs（强迫元数据规则）、
//   rasters.mjs（栅格检查与统计）；readTiff 与 runTask 由 server.mjs 注入；引擎命令行见
//   models/stream/README.md
// 约定：同一时刻最多运行一个引擎进程；输出目录 = <输出根>/<名称>_<时间戳>，内含
//   run-config.json、scene.json、run-manifest.json 与产品文件；只有 completed 的运行才列产品
// ==========================================================================

// STREAM remote-sensing image simulation service: its own API (/api/stream/*), serial job queue
// and event channel. It shares nothing with the HiStream run machinery.
import { execFile, spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, renameSync, rmSync, statSync, unlinkSync, writeFileSync } from 'node:fs'
import { homedir, tmpdir } from 'node:os'
import { basename, dirname, isAbsolute, join, resolve } from 'node:path'
import { createInterface } from 'node:readline'
import { createProjectStore } from './config.mjs'
import { FORCING_METADATA_FILE, FORCING_PRESETS, createForcingRules, isPlainObject, isPresetName } from './forcing.mjs'
import { createRasterTools } from './rasters.mjs'

// 常量：PROGRESS 匹配引擎 stdout 的进度行 "STREAM<TAB>progress<TAB>百分比<TAB>阶段"；
// PRODUCT_FILE 为运行产品的扩展名（.tif/.json）；PACK_KINDS 为合法的气象数据包索引 kind；
// 两个 FORCING_*_TIMEOUT_MS 为调用引擎校验强迫元数据的超时（完整检查 60 s，快速检查 15 s）。
const PROGRESS = /^STREAM\tprogress\t([\d.]+)\t(.*)$/
const PRODUCT_FILE = /\.(tif|json)$/i
const PACK_KINDS = ['stream-era5-pack', 'stream-forcing-pack']
const FORCING_CHECK_TIMEOUT_MS = 60_000
const FORCING_QUICK_TIMEOUT_MS = 15_000

// FIX(2026-11-24 E4): netCDF opens files through the ANSI code page, so the engine reaches a non-ASCII ERA5 file only via its
// 8.3 short name (forcing/era5.cpp) and fails when the volume has none. Check that at submission instead of in the queue.
// Returns the offending file path, or '' when every data file of the pack can be opened.
export function unreachableNonAsciiPackFile(packPath, { shortName = windowsShortName } = {}) {
  let index
  try { index = JSON.parse(readFileSync(packPath, 'utf8')) } catch { return '' }  // the engine reports unreadable indexes itself
  const folder = dirname(packPath)
  for (const item of Array.isArray(index?.files) ? index.files : []) {
    if (typeof item?.path !== 'string') continue
    const file = isAbsolute(item.path) ? item.path : join(folder, item.path)
    if (!/[^\x00-\x7f]/.test(file) || !existsSync(file)) continue
    const short = shortName(file)
    if (!short || /[^\x00-\x7f]/.test(short)) return file
  }
  return ''
}

// 作用：用 cmd 的 %~sI 取文件的 8.3 短路径（仅 Windows）；失败返回空串。
// 非 Windows 平台直接返回原路径（可直接打开 UTF-8 路径）。
function windowsShortName(file) {
  if (process.platform !== 'win32') return file  // other platforms open UTF-8 paths directly
  const result = spawnSync('cmd.exe', ['/d', '/c', `for %I in ("${file.replace(/"/g, '')}") do @echo %~sI`], { encoding: 'utf8', windowsHide: true })
  return result.status === 0 ? String(result.stdout || '').split(/\r?\n/)[0].trim() : ''
}

// 模块：STREAM 服务工厂。独立于 HiStream 的运行机制，自带 HTTP 接口（/api/stream/*）、
// 串行任务队列与 SSE 事件通道。
// 输入：resourceRoot/modelRoot（定位 stream.exe）、json/body（请求读写辅助）、readTiff、
// 路径换算、runTask（后台线程执行器，待核对）、documentsRoot（缺省 ~/Documents/Stream Studio）。
// 内部状态：clients（SSE 连接）、queue（排队任务）、active（当前任务，同一时刻最多一个）。
export function createStreamService({ resourceRoot, modelRoot, json, body, readTiff, normalizeHostPath, windowsPath, runTask = null, documentsRoot = null }) {
  // 引擎可执行文件候选位置，按顺序取第一个存在者：安装包 engine/、开发构建 Release、Debug。
  const candidates = [
    join(resourceRoot, 'engine', 'stream.exe'),
    join(modelRoot, 'bin_x64', 'Release', 'stream.exe'),
    join(modelRoot, 'bin_x64', 'Debug', 'stream.exe')
  ]
  const documents = documentsRoot || join(homedir(), 'Documents', 'Stream Studio')
  const era5Directory = () => join(documents, 'era5-packs')
  const forcingMetadataDirectory = () => join(documents, 'forcing-metadata')
  const projects = createProjectStore({ normalizeHostPath, windowsPath, forcingDirectories: () => [forcingMetadataDirectory(), era5Directory()] })
  const forcingRules = createForcingRules({ normalizeHostPath, windowsPath })
  const rasters = createRasterTools({ readTiff, normalizeHostPath, windowsPath })
  const clients = new Set()
  const queue = []
  let active = null
  let nextId = 1

  const engine = () => candidates.find(existsSync) || candidates[0]
  const dataDir = () => join(dirname(engine()), 'stream-defined')
  const defaultOutputRoot = () => windowsPath(join(documents, 'stream'))

  // 作用：向所有 SSE 客户端广播一条事件（data: JSON）。
  function emit(payload) {
    const line = `data: ${JSON.stringify(payload)}\n\n`
    for (const response of clients) response.write(line)
  }

  // 任务摘要：只暴露界面需要的字段（id、类型、状态、进度、阶段、输出目录、进程号、开始时间）。
  const summary = (job) => job && ({ id: job.id, kind: job.kind, label: job.label, status: job.status, progress: job.progress, stage: job.stage,
    outputDir: job.outputDir ? windowsPath(job.outputDir) : null, pid: job.child?.pid ?? null, startedAt: job.startedAt || null })

  // 作用：返回引擎与目录状态：引擎路径、是否存在、数据文件（classes.json 等四个）是否齐全，
  // 默认输出根、ERA5 包与强迫元数据目录，以及当前任务和排队任务。
  function status() {
    const executable = engine()
    return {
      executable: windowsPath(executable), executableExists: existsSync(executable),
      dataReady: ['classes.json', 'optipar_new.csv', 'Esun_.dat', 'Esky_.dat'].every((name) => existsSync(join(dataDir(), name))),
      defaultOutputRoot: defaultOutputRoot(), era5Directory: windowsPath(era5Directory()), forcingMetadataDirectory: windowsPath(forcingMetadataDirectory()),
      running: summary(active), queue: queue.map(summary)
    }
  }

  // 作用：提交/启动前确认引擎可执行文件与 stream-defined 参数目录存在，否则给出可操作的提示。
  function assertEngine() {
    const executable = engine()
    if (!existsSync(executable)) throw new Error(`找不到 STREAM 引擎：${windowsPath(executable)}，请先运行 npm run build:engine`)
    if (!existsSync(join(dataDir(), 'classes.json'))) throw new Error('STREAM 引擎缺少 stream-defined 参数目录')
    return executable
  }

  // 作用：合并 patch 并重写输出目录下的 run-manifest.json（带 updatedAt）。
  function writeManifest(job, patch) {
    Object.assign(job.manifest, patch, { updatedAt: new Date().toISOString() })
    writeFileSync(join(job.outputDir, 'run-manifest.json'), JSON.stringify(job.manifest, null, 2) + '\n', 'utf8')
  }

  // 2026-11-24 (审核 S6): a run that did not complete (engine killed / crashed / failed) can leave a half-written
  // .tif behind, so only completed runs list products.
  const runProducts = (directory, status) => status === 'completed' ? products(directory) : []

  const products = (directory) => existsSync(directory)
    ? readdirSync(directory).filter((name) => PRODUCT_FILE.test(name) && !['run-manifest.json', 'run-config.json', 'scene.json', 'scene_summary.json'].includes(name)).map((name) => windowsPath(join(directory, name)))
    : []

  // 模块：串行任务队列。job 对象约定：kind/label/successCodes、start()（准备并调用 launch）、
  // collect(status)（结束后收集结果）；运行期字段 id/status/progress/stage/child/outputDir/
  // manifest/stopped/finished/startedAt。同一时刻只运行一个引擎进程。
  // ---------- queue ----------
  // DEFERRED[低] 并发: 队列不做去重，同一工程连点/多窗口会排入多个相同任务（前端 startRun 在 await 期间也没有加锁）；建议：按 projectPath+scene 摘要拒绝已在队列/运行中的重复提交，或返回已有任务 id
  function enqueue(job) {
    job.id = nextId++
    job.status = 'queued'
    job.progress = 0
    job.stage = '排队中'
    queue.push(job)
    emit({ type: 'job-queued', job: summary(job) })
    // Start after the HTTP reply so the submitting page knows the job id before its events arrive.
    setImmediate(pump)
    return summary(job)
  }

  // 作用：当前没有运行任务且队列非空时，取队首启动；start() 抛错则该任务按 failed 结束。
  function pump() {
    if (active || !queue.length) return
    const job = queue.shift()
    active = job
    try { job.start() } catch (error) {
      finish(job, 'failed', null, error.message)
    }
  }

  // 作用：任务收尾（幂等，job.finished 防重入）。
  // 步骤：1) 写 manifest（状态、退出码、产品列表，仅 completed 才列产品）；2) 调 collect 收集结果；
  // 3) 广播错误与 job-closed；4) 释放 active 并启动下一个任务。
  function finish(job, status, code, error) {
    if (job.finished) return
    job.finished = true
    job.status = status
    if (job.manifest) {
      try { writeManifest(job, { status, exitCode: code, finishedAt: new Date().toISOString(), products: runProducts(job.outputDir, status).map((path) => basename(path)), ...(error ? { error } : {}) }) } catch {}
    }
    let result = null
    try { result = job.collect?.(status) ?? null } catch (collectError) { error = error || collectError.message }
    if (error) emit({ type: 'job-log', job: summary(job), level: 'error', text: error })
    emit({ type: 'job-closed', job: summary(job), status, code, elapsed: job.startedAt ? Date.now() - job.startedAt : 0, result })
    if (active === job) active = null
    pump()
  }

  // 作用：启动引擎子进程并把输出转成事件。
  // 步骤：1) 若在启动前已被停止则直接以 stopped 收尾；2) spawn 引擎（工作目录为引擎目录）；
  // 3) stdout：进度行更新 progress/stage，其他非空行作日志；4) stderr：GDAL/PROJ 的
  // "ERROR n:"/"Warning n:" 诊断标为 diagnostic，其余标为 error；
  // 5) 进程关闭时按 successCodes 判定 completed/failed，被停止则为 stopped。
  function launch(job, args) {
    if (job.stopped) { finish(job, 'stopped', null); return }
    job.status = 'running'
    job.startedAt = Date.now()
    job.stage = '启动引擎'
    const executable = assertEngine()
    // DEFERRED[中] 资源泄漏: 引擎子进程没有任何超时/看门狗，引擎卡死（读网络盘、死循环等）会让串行队列永久阻塞，只能手动停止；建议：按任务类型设置最长运行时间，超时后走与 stop() 相同的 taskkill /T 流程并标记为 failed
    const child = spawn(executable, args, { cwd: dirname(executable), windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] })
    job.child = child
    emit({ type: 'job-started', job: summary(job) })
    createInterface({ input: child.stdout, crlfDelay: Infinity }).on('line', (line) => {
      const match = PROGRESS.exec(line)
      if (match) {
        job.progress = Number(match[1]); job.stage = match[2]
        emit({ type: 'job-progress', job: summary(job) })
      } else if (line.trim()) emit({ type: 'job-log', job: summary(job), text: line.replace(/^\[STREAM\]\s*/, '') })
    })
    createInterface({ input: child.stderr, crlfDelay: Infinity }).on('line', (line) => {
      if (!line.trim()) return
      const text = line.replace(/^\[STREAM\]\s*/, '')
      // GDAL/PROJ library diagnostics ("ERROR 1: PROJ: ...", "Warning 1: ...") are not engine failures.
      emit({ type: 'job-log', job: summary(job), level: /^(ERROR|Warning) \d+: /.test(text) ? 'diagnostic' : 'error', text })
    })
    child.on('error', (error) => {
      emit({ type: 'job-log', job: summary(job), level: 'error', text: error.message })
      if (!child.pid) finish(job, 'failed', null, error.message)
    })
    child.on('close', (code) => {
      const status = job.stopped ? 'stopped' : job.successCodes.includes(code) ? 'completed' : 'failed'
      finish(job, status, code)
    })
  }

  // 作用：停止任务。排队中的直接移出并广播关闭；已激活但引擎尚未启动的只打标记；
  // 运行中的 Windows 用 taskkill /T /F 结束整个进程树（失败再 child.kill），其他平台发 SIGTERM。
  // 返回 { stopped }，找不到对应运行任务时为 false。
  function stop(id) {
    const target = Number(id)
    if (id === undefined || id === null || !Number.isInteger(target)) throw new Error('任务 id 无效')
    const queued = queue.findIndex((job) => job.id === target)
    if (queued >= 0) {
      const [job] = queue.splice(queued, 1)
      emit({ type: 'job-closed', job: { ...summary(job), status: 'stopped' }, status: 'stopped', code: null, elapsed: 0 })
      return { stopped: true }
    }
    // FIX(2026-11-24 S7): an active job whose engine has not been spawned yet (start() still preparing the run) is marked
    // stopped; launch() then closes it without spawning, instead of the click being ignored.
    if (active && active.id === target && !active.child && !active.finished) { active.stopped = true; return { stopped: true } }
    if (!active?.child || active.id !== target) return { stopped: false }
    active.stopped = true
    if (process.platform === 'win32') {
      const child = active.child
      spawn('taskkill', ['/pid', String(child.pid), '/T', '/F'], { windowsHide: true })
        .on('error', () => child.kill())
        .on('close', (code) => { if (code !== 0) child.kill() })
    } else active.child.kill('SIGTERM')
    return { stopped: true }
  }

  // 模块：影像模拟运行（提交、读取、列表）。
  // submitRun 步骤：1) 校验引擎与输出根；2) 提交时先 buildEngineConfig 校验（坏参数不入队）；
  // 3) 检查气象数据包路径的非 ASCII 可达性；4) 入队，真正启动时才建输出目录并写
  // run-config.json / scene.json / run-manifest.json，再调用 launch。
  // ---------- image runs ----------
  function submitRun(data) {
    assertEngine()
    const scene = data.scene && typeof data.scene === 'object' ? data.scene : data
    // The workbench derives scene.outputRoot (<project folder>/output) before submitting; the former
    // data.projectPath branch could never run (the client sends projectPath inside scene) and was removed.
    // DEFERRED[低] 安全: 输出根目录完全由请求决定，任意绝对路径都会被 mkdir 并写入 run-config/scene/manifest；建议：把输出根限制在工程目录/默认输出目录下
    const root = normalizeHostPath(String(scene.outputRoot || data.outputRoot || defaultOutputRoot()).trim().replace(/^"|"$/g, ''))
    if (!isAbsolute(root)) throw new Error('输出目录须为绝对路径')
    // 运行名：只保留字母、数字、下划线、连字符（其他替换为 _），最长 64 字符，空则用 image。
    const name = String(scene.name || data.name || 'image').replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '').slice(0, 64) || 'image'
    // Validate now so a bad form fails at submission rather than in the queue.
    // probe 仅用于提交期校验与取数据包路径；真正使用的配置在 start() 里重新生成（输出目录不同）。
    const probe = projects.buildEngineConfig(scene, { outputDir: root, dataDir: dataDir() })
    // Unknown temperature.energy fields are ignored (old projects must keep running) but reported, in the reply and in the job log.
    const warnings = projects.sceneWarnings(scene)
    // 数据包路径：forcing.pack 优先，其次旧字段 era5.pack（与引擎、buildForcing 的取值顺序一致）。
    const pack = probe?.temperature?.forcing?.pack || probe?.temperature?.era5?.pack
    if (typeof pack === 'string' && pack) {
      const bad = unreachableNonAsciiPackFile(resolve(normalizeHostPath(pack)))
      if (bad) throw new Error(`气象强迫数据文件路径含非 ASCII 字符且系统没有短文件名，引擎无法打开：${windowsPath(bad)}。请把数据包移到纯英文路径下后重新登记`)
    }
    const job = {
      kind: 'image', label: name, successCodes: [0],
      start() {
        // 输出目录名 = <运行名>_<时间戳>；时间戳为 UTC ISO 串，冒号和点换成连字符。
        const runId = new Date().toISOString().replace(/[:.]/g, '-')
        job.outputDir = join(resolve(root), `${name}_${runId}`)
        // FIX(2026-11-24 S5): build (= validate) the configuration first, so a failure leaves no empty output folder behind.
        const config = projects.buildEngineConfig(scene, { outputDir: job.outputDir, dataDir: dataDir() })
        mkdirSync(job.outputDir, { recursive: true })
        const configPath = join(job.outputDir, 'run-config.json')
        writeFileSync(configPath, JSON.stringify(config, null, 2) + '\n', 'utf8')
        writeFileSync(join(job.outputDir, 'scene.json'), JSON.stringify(scene, null, 2) + '\n', 'utf8')
        job.manifest = { kind: 'stream-image-run', schemaVersion: 2, runId, name, outputDir: windowsPath(job.outputDir), status: 'running',
          mode: config.temperature.mode, overpass: config.overpass, startedAt: new Date().toISOString(), engine: windowsPath(engine()) }
        writeManifest(job, {})
        for (const text of warnings) emit({ type: 'job-log', job: summary(job), level: 'warning', text: '警告：' + text })
        launch(job, ['run', configPath])
      },
      collect(status) {
        let result = null
        try { result = JSON.parse(readFileSync(join(job.outputDir, 'scene_summary.json'), 'utf8')) } catch {}
        return { products: runProducts(job.outputDir, status), summary: result }
      }
    }
    return { ...enqueue(job), ...(warnings.length ? { warnings } : {}) }
  }

  // 作用：读取一次运行的输出目录，返回清单、摘要（scene_summary.json）、配置与产品列表。
  // 说明：同时兼容旧版 stream-scene-run 清单与 scene-config.json；类型栅格路径若是相对路径
  // 则按输出目录解析（随包参考结果可整体拷贝）；读取失败时给出面向用户的提示。
  function readRun(directory) {
    const text = String(directory || '').trim()
    if (!text) throw new Error('未指定运行输出目录')
    const outputDir = resolve(normalizeHostPath(text))
    // Friendly messages instead of the raw ENOENT / JSON.parse text.
    if (!existsSync(outputDir) || !statSync(outputDir).isDirectory()) throw new Error(`找不到运行输出目录：${windowsPath(outputDir)}（可能已被移动或删除）`)
    const manifestPath = join(outputDir, 'run-manifest.json')
    if (!existsSync(manifestPath)) throw new Error(`该目录不是遥感影像模拟的输出目录（缺少 run-manifest.json）：${windowsPath(outputDir)}`)
    let manifest
    try { manifest = JSON.parse(readFileSync(manifestPath, 'utf8')) } catch { throw new Error(`运行记录 run-manifest.json 无法读取或已损坏：${windowsPath(outputDir)}`) }
    if (manifest?.kind !== 'stream-image-run' && manifest?.kind !== 'stream-scene-run') throw new Error('不是遥感影像模拟的输出目录')
    let result = null, config = null
    try { result = JSON.parse(readFileSync(join(outputDir, 'scene_summary.json'), 'utf8')) } catch {}
    for (const file of ['run-config.json', 'scene-config.json']) {
      if (config) break
      try { config = JSON.parse(readFileSync(join(outputDir, file), 'utf8')) } catch {}
    }
    const typeField = config?.surface?.type || config?.inputs?.type
    const typeFile = typeField?.file || null
    const typeBand = Number(typeField?.band) || 1
    return { outputDir: windowsPath(outputDir), runId: manifest.runId, status: manifest.status, startedAt: manifest.startedAt,
      finishedAt: manifest.finishedAt || null, mode: manifest.mode || 'rt', overpass: manifest.overpass || result?.overpass || null,
      // Shipped reference runs store the land-type path relative to the run folder so they survive being copied.
      typeFile: typeFile && !isAbsolute(typeFile) ? windowsPath(resolve(outputDir, typeFile)) : typeFile,
      typeBand,
      products: runProducts(outputDir, manifest.status), summary: result, config }
  }

  // 作用：列出输出根目录下的运行记录（含 run-manifest.json 的子目录），按开始时间倒序，最多 50 条。
  function listRuns(root) {
    const directory = resolve(normalizeHostPath(String(root || defaultOutputRoot()).trim()))
    if (!existsSync(directory)) return []
    const runs = []
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      if (!entry.isDirectory() || !existsSync(join(directory, entry.name, 'run-manifest.json'))) continue
      // DEFERRED[低] 性能: 列表只需要 manifest 字段，却对每个目录完整同步解析 scene_summary 与 run-config（目录多时阻塞事件循环），而且全部读完才截取 50 条；建议：列表只读 run-manifest.json，先按目录名/mtime 排序截断再读
      try {
        const run = readRun(join(directory, entry.name))
        runs.push({ outputDir: run.outputDir, name: entry.name, status: run.status, startedAt: run.startedAt, mode: run.mode })
      } catch {}
    }
    return runs.sort((a, b) => String(b.startedAt).localeCompare(String(a.startedAt))).slice(0, 50)
  }

  // 模块：气象数据包（ERA5 / 通用强迫包）的登记、列表与检查。
  // ---------- ERA5 packs ----------
  // 作用：读取数据包索引并生成界面摘要；kind 不是数据包时返回 null。
  // 说明：旧索引没有 fullYear/period 时按整年推出；skt/stl1/stl2 不再是必需变量
  // （EB 初值已改用 2 m 气温），故从缺失/不完整列表里排除，并据此重新判定 complete。
  function packSummary(path) {
    const pack = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
    if (!PACK_KINDS.includes(pack.kind)) return null
    // Indexes registered before time-period packs only ever completed for a whole calendar year.
    const fullYear = pack.fullYear ?? Boolean(pack.complete)
    const period = pack.period || (fullYear ? { start: `${pack.year}-01-01T00:00:00Z`, end: `${pack.year}-12-31T23:00:00Z` } : null)
    // 2026-11-24: skt / stl1 are no longer required (EB initial values come from the 2 m air temperature; skt is only
    // read for water pixels). Indexes registered earlier may list them as missing or incomplete: do not block on that.
    const optionalNow = new Set(['skt', 'stl1', 'stl2'])
    const missingVariables = (pack.missingVariables || []).filter((name) => !optionalNow.has(name))
    const incompleteVariables = (pack.incompleteVariables || []).filter((name) => !optionalNow.has(name))
    const complete = Boolean(pack.complete) || (missingVariables.length === 0 && incompleteVariables.length === 0 &&
      ((pack.missingVariables || []).length + (pack.incompleteVariables || []).length) > 0)
    return { path: windowsPath(path), label: pack.label || `${String(pack.product || 'era5').toUpperCase()} ${period ? period.start.slice(0, 10) : pack.year}`, product: pack.product,
      year: pack.year, period, fullYear, complete, files: (pack.files || []).length, variables: pack.variables || {},
      missingVariables, incompleteVariables, grid: pack.grid, createdAt: pack.createdAt || pack.created,
      // stream-forcing-pack (non-ERA5 gridded sources) carries the metadata it was registered with and its record step.
      kind: pack.kind, metadata: typeof pack.metadata === 'string' ? pack.metadata : null, stepHours: Number(pack.stepHours) || 1 }
  }

  // 作用：列出气象数据包摘要：登记目录里的索引（external=false）加上工程指向的其他位置的索引
  // （external=true）。排序：已登记在前，同组内按起始时间新→旧，再按标签。解析失败的索引被忽略。
  // Registered packs plus any pack a project points at elsewhere (for example one shipped inside the project).
  function listPacks(include = []) {
    const directory = era5Directory()
    const registered = existsSync(directory) ? readdirSync(directory).filter((name) => /\.json$/i.test(name)).map((name) => join(directory, name)) : []
    const extra = (Array.isArray(include) ? include : []).map((value) => resolve(normalizeHostPath(String(value || '').trim().replace(/^"|"$/g, ''))))
      .filter((path) => /\.json$/i.test(path) && existsSync(path) && !registered.some((item) => item.toLowerCase() === path.toLowerCase()))
    return [...registered.map((path) => [path, false]), ...extra.map((path) => [path, true])].map(([path, external]) => {
      try { const pack = packSummary(path); return pack && { ...pack, external } } catch { return null }
    }).filter(Boolean).sort((a, b) => Number(a.external) - Number(b.external) || String(b.period?.start || b.year).localeCompare(String(a.period?.start || a.year)) || String(a.label).localeCompare(String(b.label)))
  }

  // 作用：把 NetCDF 文件登记为数据包索引（作为 'era5' 类任务入队），返回入队摘要。
  // 步骤：1) 校验文件（1–4096 个且都存在）；2) 判定是否通用来源：metadata（或 product）不是
  // era5/era5-land/auto 即走 forcing-register，内联对象会先存成 era5-packs/metadata/ 下的副本；
  // 3) 站点形态元数据不需要登记；4) 任务成功后若给了 label 则写回索引，失败则删除半成品索引与副本。
  // successCodes 为 [0, 3]：退出码 3 的含义由引擎定义（待核对）。
  // ERA5 / ERA5-Land go through era5-register (unchanged); any other gridded source goes through forcing-register
  // with its metadata (a preset name, an inline object stored beside the index, or a metadata file).
  function registerPack(data) {
    assertEngine()
    const files = (Array.isArray(data.files) ? data.files : []).map((value) => resolve(normalizeHostPath(String(value || '').trim().replace(/^"|"$/g, ''))))
    if (!files.length) throw new Error('请选择逐时气象强迫 NetCDF 文件')
    if (files.length > 4096) throw new Error('气象强迫文件过多（上限 4096）')
    for (const file of files) if (!existsSync(file) || !statSync(file).isFile()) throw new Error(`气象强迫文件不存在：${windowsPath(file)}`)
    const rawMetadata = data.metadata === undefined || data.metadata === null || data.metadata === '' ? data.product : data.metadata
    const generic = rawMetadata !== undefined && rawMetadata !== null && rawMetadata !== '' && !['era5', 'era5-land', 'auto'].includes(rawMetadata)
    const product = ['era5', 'era5-land'].includes(rawMetadata) ? rawMetadata : 'auto'
    const label = String(data.label || '').trim().slice(0, 64)
    mkdirSync(era5Directory(), { recursive: true })
    const stamp = new Date().toISOString().replace(/[:.]/g, '-')
    const index = join(era5Directory(), `pack-${stamp}.json`)
    let args, metadataCopy = null
    if (generic) {
      if (typeof rawMetadata === 'string' && rawMetadata.trim().toLowerCase() === 'station-csv') throw new Error('气象站点 CSV 无需登记，请直接在气象强迫面板中选择 CSV 文件')
      const resolved = forcingRules.resolveMetadata(rawMetadata, { projectPath: data.projectPath, directories: [era5Directory(), forcingMetadataDirectory()] })
      if (resolved.form === 'station') throw new Error('站点形态的气象强迫元数据无需登记数据包')
      let argument = resolved.metadata
      if (isPlainObject(resolved.metadata)) {
        metadataCopy = join(era5Directory(), 'metadata', `pack-${stamp}.metadata.json`)
        mkdirSync(dirname(metadataCopy), { recursive: true })
        writeFileSync(metadataCopy, JSON.stringify({ kind: 'stream-forcing-metadata', schemaVersion: 1, ...resolved.metadata }, null, 2) + '\n', 'utf8')
        argument = metadataCopy
      }
      args = ['forcing-register', index, '--metadata', String(argument), ...files.map(windowsPath)]
    } else args = ['era5-register', index, '--product', product, ...files.map(windowsPath)]
    const job = {
      kind: 'era5', label: label || '气象数据包', successCodes: [0, 3],
      // DEFERRED[中] Bug: 所有 NetCDF 路径直接作为命令行参数，Windows CreateProcess 命令行上限约 32767 字符，按天下载的一年数据（数百个长路径）会让 spawn 失败（ENAMETOOLONG），并落入 launch 中 error 后不一定触发 close 的问题；建议：把文件列表写入临时清单文件，用 --files-from <list> 之类的参数传给引擎
      start() { launch(job, args) },
      collect(status) {
        if (status !== 'completed') {
          try { if (existsSync(index)) unlinkSync(index) } catch {}
          try { if (metadataCopy && existsSync(metadataCopy)) unlinkSync(metadataCopy) } catch {}
          return null
        }
        if (!existsSync(index)) return null
        const pack = JSON.parse(readFileSync(index, 'utf8'))
        if (label) { pack.label = label; writeFileSync(index, JSON.stringify(pack, null, 2) + '\n', 'utf8') }
        return { pack: packSummary(index) }
      }
    }
    return enqueue(job)
  }

  // 作用：只允许移除登记目录里的数据包索引，同时删除该索引名下 metadata/ 里的元数据副本。
  function removePack(value) {
    const path = resolve(normalizeHostPath(String(value || '').trim()))
    if (dirname(path).toLowerCase() !== resolve(era5Directory()).toLowerCase() || !existsSync(path)) throw new Error('只能移除登记目录中的气象数据包索引')
    if (!packSummary(path)) throw new Error('不是气象数据包索引')
    // A generic pack may own a metadata copy stored under <packs>/metadata/.
    try {
      const metadata = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')).metadata
      if (typeof metadata === 'string' && /\.json$/i.test(metadata)) {
        const owned = resolve(dirname(path), metadata)
        if (dirname(owned).toLowerCase() === join(resolve(era5Directory()), 'metadata').toLowerCase() && existsSync(owned)) unlinkSync(owned)
      }
    } catch {}
    unlinkSync(path)
    return { removed: true, path: windowsPath(path) }
  }

  // 模块：气象强迫元数据的引擎调用与工程元数据文件读写。
  // ---------- forcing metadata ----------
  // Short engine calls (preset listing, forcing-check) run to completion outside the job queue, under a timeout.
  function runEngine(args, timeoutMs) {
    const executable = assertEngine()
    return new Promise((resolveRun, reject) => {
      execFile(executable, args, { cwd: dirname(executable), windowsHide: true, encoding: 'utf8', timeout: timeoutMs, maxBuffer: 8 * 1024 * 1024 }, (error, stdout, stderr) => {
        if (error && error.killed) return reject(new Error(`STREAM 引擎调用超时（${Math.round(timeoutMs / 1000)} s）：${args[0]}`))
        if (error && typeof error.code !== 'number') return reject(new Error(`无法运行 STREAM 引擎：${error.message}`))
        resolveRun({ code: error ? error.code : 0, stdout: String(stdout || ''), stderr: String(stderr || '') })
      })
    })
  }

  // 预设内容缓存（预设随引擎版本固定，进程内只取一次）。
  const presetCache = new Map()
  // 作用：调用 stream forcing-preset 列出引擎支持的预设名（只保留 forcing.mjs 认识的名字），
  // 同时返回前端内置列表 builtin。
  async function listPresets() {
    const result = await runEngine(['forcing-preset'], FORCING_QUICK_TIMEOUT_MS)
    const names = result.stdout.split(/\r?\n/).map((line) => line.trim()).filter((line) => isPresetName(line))
    if (result.code !== 0 || !names.length) throw new Error(`stream forcing-preset 失败（退出码 ${result.code}）：${result.stderr.trim().slice(-300) || result.stdout.trim().slice(0, 300)}`)
    return { presets: names, builtin: FORCING_PRESETS }
  }

  async function getPreset(name) {
    const text = String(name ?? '').trim()
    if (!isPresetName(text)) throw new Error(`未知的气象强迫预设 "${text}"；可用：${FORCING_PRESETS.join('、')}`)
    if (!presetCache.has(text)) {
      const result = await runEngine(['forcing-preset', text], FORCING_QUICK_TIMEOUT_MS)
      let preset
      try { preset = JSON.parse(result.stdout) } catch { preset = null }
      if (result.code !== 0 || !isPlainObject(preset)) throw new Error(`stream forcing-preset ${text} 失败（退出码 ${result.code}）：${result.stderr.trim().slice(-300)}`)
      presetCache.set(text, preset)
    }
    return { name: text, metadata: structuredClone(presetCache.get(text)) }
  }

  // 步骤：1) 解析元数据（预设/文件/对象）；2) 收集数据包与站点文件；3) 内联对象写入临时目录；
  // 4) 调 forcing-check --json（超时 60 s），退出码 0 或 3 视为得到报告（3 的含义待核对）；
  // 5) finally 删除临时目录。输出 { ok, errors, warnings, notes, resolved, form }。
  // metadata (preset name | object | file path) + optional data (pack index | station CSV files) -> engine forcing-check JSON.
  async function checkForcing(data) {
    const raw = data.metadata
    if (raw === undefined || raw === null || raw === '') throw new Error('请提供要校验的气象强迫元数据（预设名、元数据对象或文件路径）')
    const resolved = forcingRules.resolveMetadata(raw, { projectPath: data.projectPath, directories: [era5Directory(), forcingMetadataDirectory()] })
    const packText = String(data.pack ?? '').trim()
    const files = forcingRules.stationFiles(data.files)
    const dataArguments = []
    if (packText) dataArguments.push(windowsPath(forcingRules.packFile(packText)))
    dataArguments.push(...files)
    const scratch = mkdtempSync(join(tmpdir(), 'stream-forcing-'))
    try {
      let argument = resolved.metadata
      if (isPlainObject(argument)) {
        argument = join(scratch, 'metadata.json')
        writeFileSync(argument, JSON.stringify({ kind: 'stream-forcing-metadata', schemaVersion: 1, ...resolved.metadata }), 'utf8')
      }
      const args = ['forcing-check', String(argument), ...(dataArguments.length ? ['--data', ...dataArguments] : []), '--json']
      const result = await runEngine(args, FORCING_CHECK_TIMEOUT_MS)
      if (![0, 3].includes(result.code)) throw new Error(`stream forcing-check 异常结束（退出码 ${result.code}）：${(result.stderr.trim() || result.stdout.trim()).slice(-400)}`)
      let report
      try { report = JSON.parse(result.stdout) } catch { throw new Error(`stream forcing-check 输出无法解析：${result.stdout.trim().slice(0, 300)}`) }
      return { ok: report.ok === true, errors: report.errors || [], warnings: report.warnings || [], notes: report.notes || [], resolved: report.resolved || null, form: resolved.form }
    } finally { try { rmSync(scratch, { recursive: true, force: true }) } catch {} }
  }

  // 工程元数据文件名限制为 forcing-metadata*.json（缺省 forcing-metadata.json），防止写到任意文件名。
  const metadataFileName = (value) => {
    const name = String(value || FORCING_METADATA_FILE).trim()
    if (!/^forcing-metadata[\w.-]*\.json$/.test(name)) throw new Error(`元数据文件名须形如 ${FORCING_METADATA_FILE}`)
    return name
  }

  // 作用：由已保存的工程文件路径得到其所在目录；工程未保存时提示先保存。
  function projectDirectory(value) {
    const text = String(value || '').trim()
    if (!text) throw new Error('请先保存工程，再保存工程元数据')
    const path = resolve(normalizeHostPath(text.replace(/^"|"$/g, '')))
    if (!isAbsolute(path) || !/\.json$/i.test(path) || !existsSync(path) || !statSync(path).isFile()) throw new Error('请先保存工程，再保存工程元数据')
    return dirname(path)
  }

  // 作用：把元数据对象校验后写入工程目录（先写 .tmp 再 rename，避免写一半被读到）。
  function saveProjectMetadata(data) {
    const directory = projectDirectory(data.projectPath)
    const metadata = data.metadata
    if (!isPlainObject(metadata)) throw new Error('气象强迫元数据须为对象')
    forcingRules.assertInlineSize(metadata)
    const document = { kind: 'stream-forcing-metadata', schemaVersion: 1, ...metadata }
    forcingRules.checkDocument(document, '气象强迫元数据')
    const target = join(directory, metadataFileName(data.fileName))
    const text = JSON.stringify(document, null, 2) + '\n'
    const temporary = `${target}.${process.pid}.tmp`
    writeFileSync(temporary, text, 'utf8')
    renameSync(temporary, target)
    return { path: windowsPath(target), bytes: Buffer.byteLength(text, 'utf8') }
  }

  // 作用：读取工程元数据；data.import 为真时读取任意位置的文件（只回显给编辑器），
  // 否则只允许工程目录、数据包目录或元数据目录内的文件。文件不存在返回 { exists: false }。
  function readProjectMetadata(data) {
    if (data.import) { // metadata picked anywhere: read back to the editor only
      const { path, document } = forcingRules.importMetadataFile(data.path)
      return { exists: true, path: windowsPath(path), metadata: document }
    }
    const directory = projectDirectory(data.projectPath)
    const target = data.path ? String(data.path) : join(directory, metadataFileName(data.fileName))
    const absolute = resolve(normalizeHostPath(target))
    if (!existsSync(absolute)) return { exists: false, path: windowsPath(absolute) }
    const { path, document } = forcingRules.readMetadataFile(absolute, [directory, era5Directory(), forcingMetadataDirectory()])
    return { exists: true, path: windowsPath(path), metadata: document }
  }

  // 模块：文件/目录浏览对话框。Windows 下用 PowerShell（-STA）弹出系统对话框，
  // 其他平台提示直接粘贴路径。kind 决定过滤器与是否多选。
  // ---------- dialogs ----------
  function pick(kind) {
    if (process.platform !== 'win32') throw new Error('浏览选择仅在 Windows 上可用，请直接粘贴绝对路径')
    const filters = {
      file: ['选择栅格文件', '栅格 (*.tif;*.tiff;*.img;*.dat;*.nc;*.h5;*.hdf)|*.tif;*.tiff;*.img;*.dat;*.nc;*.h5;*.hdf|所有文件 (*.*)|*.*', false],
      project: ['打开遥感影像模拟工程', 'STREAM 工程 (*.json)|*.json|所有文件 (*.*)|*.*', false],
      era5: ['选择逐小时气象强迫 NetCDF 文件（可多选，覆盖一天到一年均可；ERA5 / ERA5-Land 为用例）', 'NetCDF (*.nc;*.nc4)|*.nc;*.nc4|所有文件 (*.*)|*.*', true],
      station: ['选择气象站点 CSV 文件（可多选）', 'CSV (*.csv;*.txt;*.dat)|*.csv;*.txt;*.dat|所有文件 (*.*)|*.*', true],
      metadata: ['选择气象强迫元数据文件', '元数据 (*.json)|*.json|所有文件 (*.*)|*.*', false]
    }
    const script = kind === 'folder' ? [
      "$ErrorActionPreference = 'Stop'", 'Add-Type -AssemblyName System.Windows.Forms',
      '$dialog = New-Object System.Windows.Forms.FolderBrowserDialog',
      "$dialog.Description = '选择目录'",
      'if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $dialog.SelectedPath }'
    ] : [
      "$ErrorActionPreference = 'Stop'", 'Add-Type -AssemblyName System.Windows.Forms',
      '$dialog = New-Object System.Windows.Forms.OpenFileDialog',
      `$dialog.Title = '${filters[kind][0]}'`,
      `$dialog.Filter = '${filters[kind][1]}'`,
      '$dialog.CheckFileExists = $true',
      `$dialog.Multiselect = $${filters[kind][2] ? 'true' : 'false'}`,
      'if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $dialog.FileNames | ForEach-Object { Write-Output $_ } }'
    ]
    // DEFERRED[低] 资源泄漏: 选择对话框进程既没有超时也不限并发；对话框被主窗口挡住或用户不操作时 HTTP 请求一直挂起，重复点击会同时弹出多个 PowerShell 进程；建议：同一时间只允许一个对话框（复用进行中的 Promise），并给 execFile 设置 timeout
    return new Promise((resolveChoice, reject) => {
      execFile('powershell.exe', ['-NoProfile', '-STA', '-Command', '[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; ' + script.join('; ')],
        { windowsHide: false, encoding: 'utf8', maxBuffer: 1024 * 1024 }, (error, stdout, stderr) => {
          if (error) return reject(new Error(String(stderr || '').trim() || error.message))
          resolveChoice(String(stdout || '').split(/\r?\n/).map((line) => line.trim()).filter(Boolean))
        })
    })
  }

  // 模块：路由表与请求分发。routes 的键为 "方法 路径"，值为 (请求体) => 结果（可为 Promise）；
  // handle 返回 false 表示不是本服务的接口，由 server.mjs 继续处理。
  // ---------- routes ----------
  const routes = {
    'GET /api/stream/status': () => status(),
    'POST /api/stream/run': (data) => submitRun(data),
    'POST /api/stream/stop': (data) => stop(data.id),
    'POST /api/stream/inspect': (data) => ({ rasters: rasters.inspectRasters(data.paths) }),
    // raster 与 class-stats 的整幅 readTiff 解码由 runTask 放到后台计算线程（server.mjs 提供）；未提供时在本线程同步执行。
    'POST /api/stream/raster': (data) => runTask
      ? runTask('stream-raster', { path: data.path, band: Number(data.band) || 0 }, { key: String(data.path || '') })
      : rasters.rasterBand(data.path, Number(data.band) || 0),
    'POST /api/stream/runs': (data) => ({ runs: listRuns(data.outputRoot) }),
    'POST /api/stream/run-info': (data) => readRun(data.outputDir),
    'POST /api/stream/class-stats': (data) => {
      const run = readRun(data.outputDir)
      const outputDir = resolve(normalizeHostPath(run.outputDir))
      return runTask
        ? runTask('stream-class-stats', { outputDir, typeFile: run.typeFile, typeBand: run.typeBand }, { key: String(run.typeFile || '') })
        : rasters.classStatistics(outputDir, run.typeFile, run.typeBand)
    },
    'POST /api/stream/project/open': (data) => {
      const project = projects.readProjectFile(data.path)
      if (!project) throw new Error('不是 STREAM 遥感影像模拟工程')
      return project
    },
    'POST /api/stream/project/save': (data) => projects.saveProjectFile(data),
    'POST /api/stream/era5/list': (data) => ({ packs: listPacks(data.include), directory: windowsPath(era5Directory()) }),
    'POST /api/stream/era5/register': (data) => registerPack(data),
    'POST /api/stream/era5/remove': (data) => removePack(data.path),
    'GET /api/stream/forcing/presets': () => listPresets(),
    'POST /api/stream/forcing/preset': (data) => getPreset(data.name),
    'POST /api/stream/forcing/check': (data) => checkForcing(data),
    'POST /api/stream/forcing/register': (data) => registerPack(data),
    'POST /api/stream/forcing/metadata/save': (data) => saveProjectMetadata(data),
    'POST /api/stream/forcing/metadata/read': (data) => readProjectMetadata(data),
    'POST /api/stream/pick': async (data) => {
      const kind = ['folder', 'project', 'era5', 'station', 'metadata'].includes(data.kind) ? data.kind : 'file'
      const paths = await pick(kind)
      return paths.length ? { path: paths[0], paths } : { cancelled: true }
    }
  }

  // 作用：处理 /api/stream/*。GET /events 建立 SSE 并先推一条 status；
  // 其他按路由表执行，成功返回 200 JSON，异常统一返回 400 { error }。
  async function handle(request, response, url) {
    if (!url.pathname.startsWith('/api/stream/')) return false
    if (request.method === 'GET' && url.pathname === '/api/stream/events') {
      response.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' })
      response.write(`data: ${JSON.stringify({ type: 'status', ...status() })}\n\n`)
      clients.add(response)
      request.on('close', () => clients.delete(response))
      response.on('error', () => clients.delete(response))
      return true
    }
    const route = routes[`${request.method} ${url.pathname}`]
    if (!route) return false
    try { return json(response, 200, await route(request.method === 'GET' ? {} : await body(request))) }
    catch (error) { return json(response, 400, { error: error.message }) }
  }

  // 作用：服务退出前的收尾：清空队列，停止当前引擎进程（Windows 结束整个进程树）。
  // 进程随后就退出，等不到 close 事件，运行清单在这里同步写成 stopped。
  function shutdown() {
    queue.length = 0
    const job = active
    if (!job?.child) return
    job.stopped = true
    if (job.manifest) {
      try { writeManifest(job, { status: 'stopped', exitCode: null, finishedAt: new Date().toISOString(), products: [] }) } catch {}
    }
    if (process.platform === 'win32') {
      try { spawnSync('taskkill', ['/pid', String(job.child.pid), '/T', '/F'], { windowsHide: true }) } catch { job.child.kill('SIGTERM') }
    } else job.child.kill('SIGTERM')
  }

  // STREAM 工程独立于三维工程：只打开 stream-project.json（或旧版 STREAM 工程）。
  return { handle, shutdown, executable: () => windowsPath(engine()), openProjectFile: (path) => projects.readProjectFile(path) }
}
