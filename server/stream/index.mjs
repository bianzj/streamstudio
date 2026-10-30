// STREAM remote-sensing image simulation service: its own API (/api/stream/*), serial job queue
// and event channel. It shares nothing with the HiStream run machinery.
import { execFile, spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, readdirSync, statSync, unlinkSync, writeFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { basename, dirname, isAbsolute, join, resolve } from 'node:path'
import { createInterface } from 'node:readline'
import { createProjectStore } from './config.mjs'
import { createRasterTools } from './rasters.mjs'

const PROGRESS = /^STREAM\tprogress\t([\d.]+)\t(.*)$/
const PRODUCT_FILE = /\.(tif|json)$/i

export function createStreamService({ resourceRoot, modelRoot, json, body, readTiff, normalizeHostPath, windowsPath }) {
  const candidates = [
    join(resourceRoot, 'engine', 'stream.exe'),
    join(modelRoot, 'bin_x64', 'Release', 'stream.exe'),
    join(modelRoot, 'bin_x64', 'Debug', 'stream.exe')
  ]
  const projects = createProjectStore({ normalizeHostPath, windowsPath })
  const rasters = createRasterTools({ readTiff, normalizeHostPath, windowsPath })
  const clients = new Set()
  const queue = []
  let active = null
  let nextId = 1

  const engine = () => candidates.find(existsSync) || candidates[0]
  const dataDir = () => join(dirname(engine()), 'stream-defined')
  const documents = join(homedir(), 'Documents', 'Stream Studio')
  const defaultOutputRoot = () => windowsPath(join(documents, 'stream'))
  const era5Directory = () => join(documents, 'era5-packs')

  // FIXED[低] 错误处理: 向已断开的 SSE 响应写入时没有 error 监听；已修复：SSE 响应注册 'error' → 从 clients 移除（避免写入已断开连接后无监听者抛未捕获异常）；心跳 ': ping' 未加
  function emit(payload) {
    const line = `data: ${JSON.stringify(payload)}\n\n`
    for (const response of clients) response.write(line)
  }

  const summary = (job) => job && ({ id: job.id, kind: job.kind, label: job.label, status: job.status, progress: job.progress, stage: job.stage,
    outputDir: job.outputDir ? windowsPath(job.outputDir) : null, pid: job.child?.pid ?? null, startedAt: job.startedAt || null })

  function status() {
    const executable = engine()
    return {
      executable: windowsPath(executable), executableExists: existsSync(executable),
      dataReady: ['classes.json', 'optipar_new.csv', 'Esun_.dat', 'Esky_.dat'].every((name) => existsSync(join(dataDir(), name))),
      defaultOutputRoot: defaultOutputRoot(), era5Directory: windowsPath(era5Directory()),
      running: summary(active), queue: queue.map(summary)
    }
  }

  function assertEngine() {
    const executable = engine()
    if (!existsSync(executable)) throw new Error(`找不到 STREAM 引擎：${windowsPath(executable)}，请先运行 npm run build:engine`)
    if (!existsSync(join(dataDir(), 'classes.json'))) throw new Error('STREAM 引擎缺少 stream-defined 参数目录')
    return executable
  }

  function writeManifest(job, patch) {
    Object.assign(job.manifest, patch, { updatedAt: new Date().toISOString() })
    writeFileSync(join(job.outputDir, 'run-manifest.json'), JSON.stringify(job.manifest, null, 2) + '\n', 'utf8')
  }

  // FIXED[低] 可维护性: 产品列表只排除 run-manifest/run-config，输入副本 scene.json 和 scene_summary.json 也会被当成“产品”写进 manifest 并返回前端；已修复：排除名单同时包含 scene.json / scene_summary.json
  const products = (directory) => existsSync(directory)
    ? readdirSync(directory).filter((name) => PRODUCT_FILE.test(name) && !['run-manifest.json', 'run-config.json', 'scene.json', 'scene_summary.json'].includes(name)).map((name) => windowsPath(join(directory, name)))
    : []

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

  function pump() {
    if (active || !queue.length) return
    const job = queue.shift()
    active = job
    try { job.start() } catch (error) {
      finish(job, 'failed', null, error.message)
    }
  }

  function finish(job, status, code, error) {
    if (job.finished) return
    job.finished = true
    job.status = status
    if (job.manifest) {
      try { writeManifest(job, { status, exitCode: code, finishedAt: new Date().toISOString(), products: products(job.outputDir).map((path) => basename(path)), ...(error ? { error } : {}) }) } catch {}
    }
    let result = null
    try { result = job.collect?.(status) ?? null } catch (collectError) { error = error || collectError.message }
    if (error) emit({ type: 'job-log', job: summary(job), level: 'error', text: error })
    emit({ type: 'job-closed', job: summary(job), status, code, elapsed: job.startedAt ? Date.now() - job.startedAt : 0, result })
    if (active === job) active = null
    pump()
  }

  function launch(job, args) {
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
    // FIXED[中] 错误处理: spawn 失败（ENOENT/EACCES/ENAMETOOLONG）时这里只记日志，Node 不保证之后会触发 close，任务会一直占着 active，串行队列永久阻塞；已修复：error 回调同样调用 finish(job, 'failed', ...)，finish 用 job.finished 去重，避免与 close 重复结束
    child.on('error', (error) => {
      emit({ type: 'job-log', job: summary(job), level: 'error', text: error.message })
      finish(job, 'failed', null, error.message)
    })
    child.on('close', (code) => {
      const status = job.stopped ? 'stopped' : job.successCodes.includes(code) ? 'completed' : 'failed'
      finish(job, status, code)
    })
  }

  function stop(id) {
    const target = Number(id)
    // FIXED[低] Bug: id 缺省（undefined/null）时会停止当前任意任务（包括 ERA5 登记）；传入非数字 id（如前端的 'pending'）时 Number() 得到 NaN，静默返回 stopped:false；已修复：id 非整数时抛错（路由层转成 400），不再退化成“停任意任务”
    if (id === undefined || id === null || !Number.isInteger(target)) throw new Error('任务 id 无效')
    const queued = queue.findIndex((job) => job.id === target)
    if (queued >= 0) {
      const [job] = queue.splice(queued, 1)
      emit({ type: 'job-closed', job: { ...summary(job), status: 'stopped' }, status: 'stopped', code: null, elapsed: 0 })
      return { stopped: true }
    }
    if (!active?.child || active.id !== target) return { stopped: false }
    active.stopped = true
    if (process.platform === 'win32') {
      const child = active.child
      // FIXED[低] 错误处理: 只处理 taskkill 启动失败，不检查它的退出码；已修复：监听 taskkill 的 close，退出码非 0 时退回 child.kill()
      spawn('taskkill', ['/pid', String(child.pid), '/T', '/F'], { windowsHide: true })
        .on('error', () => child.kill())
        .on('close', (code) => { if (code !== 0) child.kill() })
    } else active.child.kill('SIGTERM')
    return { stopped: true }
  }

  // ---------- image runs ----------
  function submitRun(data) {
    assertEngine()
    const scene = data.scene && typeof data.scene === 'object' ? data.scene : data
    // DEFERRED[中] Bug: 前端 streamApi.run 把 projectPath 放在 { scene } 里（即 data.scene.projectPath），这里读 data.projectPath 永远为空，这个分支是死代码；输出目录实际靠前端拼出的 scene.outputRoot；建议：统一字段位置（服务端读 scene.projectPath 或前端放到顶层），只保留一处推导输出目录的逻辑
    const projectPath = String(data.projectPath || '').trim().replace(/^"|"$/g, '')
    // DEFERRED[低] 安全: 输出根目录完全由请求决定，任意绝对路径都会被 mkdir 并写入 run-config/scene/manifest，也不校验 projectPath 是否为真实存在的 STREAM 工程；建议：校验工程文件存在且 kind 为 stream-image-project，或把输出根限制在工程目录/默认输出目录下
    const root = projectPath
      ? join(dirname(normalizeHostPath(projectPath)), 'output')
      : normalizeHostPath(String(scene.outputRoot || data.outputRoot || defaultOutputRoot()).trim().replace(/^"|"$/g, ''))
    if (!isAbsolute(root)) throw new Error('输出目录须为绝对路径')
    const name = String(scene.name || data.name || 'image').replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '').slice(0, 64) || 'image'
    // Validate now so a bad form fails at submission rather than in the queue.
    projects.buildEngineConfig(scene, { outputDir: root, dataDir: dataDir() })
    const job = {
      kind: 'image', label: name, successCodes: [0],
      start() {
        const runId = new Date().toISOString().replace(/[:.]/g, '-')
        job.outputDir = join(resolve(root), `${name}_${runId}`)
        mkdirSync(job.outputDir, { recursive: true })
        const config = projects.buildEngineConfig(scene, { outputDir: job.outputDir, dataDir: dataDir() })
        const configPath = join(job.outputDir, 'run-config.json')
        writeFileSync(configPath, JSON.stringify(config, null, 2) + '\n', 'utf8')
        writeFileSync(join(job.outputDir, 'scene.json'), JSON.stringify(scene, null, 2) + '\n', 'utf8')
        job.manifest = { kind: 'stream-image-run', schemaVersion: 2, runId, name, outputDir: windowsPath(job.outputDir), status: 'running',
          mode: config.temperature.mode, overpass: config.overpass, startedAt: new Date().toISOString(), engine: windowsPath(engine()) }
        writeManifest(job, {})
        launch(job, ['run', configPath])
      },
      collect() {
        let result = null
        try { result = JSON.parse(readFileSync(join(job.outputDir, 'scene_summary.json'), 'utf8')) } catch {}
        return { products: products(job.outputDir), summary: result }
      }
    }
    return enqueue(job)
  }

  function readRun(directory) {
    const outputDir = resolve(normalizeHostPath(String(directory || '').trim()))
    const manifest = JSON.parse(readFileSync(join(outputDir, 'run-manifest.json'), 'utf8'))
    if (manifest.kind !== 'stream-image-run' && manifest.kind !== 'stream-scene-run') throw new Error('不是遥感影像模拟的输出目录')
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
      products: products(outputDir), summary: result, config }
  }

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

  // ---------- ERA5 packs ----------
  function packSummary(path) {
    const pack = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
    if (pack.kind !== 'stream-era5-pack') return null
    // Indexes registered before time-period packs only ever completed for a whole calendar year.
    const fullYear = pack.fullYear ?? Boolean(pack.complete)
    const period = pack.period || (fullYear ? { start: `${pack.year}-01-01T00:00:00Z`, end: `${pack.year}-12-31T23:00:00Z` } : null)
    return { path: windowsPath(path), label: pack.label || `${String(pack.product || 'era5').toUpperCase()} ${period ? period.start.slice(0, 10) : pack.year}`, product: pack.product,
      year: pack.year, period, fullYear, complete: Boolean(pack.complete), files: (pack.files || []).length, variables: pack.variables || {},
      missingVariables: pack.missingVariables || [], incompleteVariables: pack.incompleteVariables || [], grid: pack.grid, createdAt: pack.createdAt }
  }

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

  function registerPack(data) {
    assertEngine()
    const files = (Array.isArray(data.files) ? data.files : []).map((value) => resolve(normalizeHostPath(String(value || '').trim().replace(/^"|"$/g, ''))))
    if (!files.length) throw new Error('请选择 ERA5 NetCDF 文件')
    for (const file of files) if (!existsSync(file) || !statSync(file).isFile()) throw new Error(`ERA5 文件不存在：${windowsPath(file)}`)
    const product = ['era5', 'era5-land'].includes(data.product) ? data.product : 'auto'
    const label = String(data.label || '').trim().slice(0, 64)
    mkdirSync(era5Directory(), { recursive: true })
    const index = join(era5Directory(), `pack-${new Date().toISOString().replace(/[:.]/g, '-')}.json`)
    const job = {
      kind: 'era5', label: label || 'ERA5 数据包', successCodes: [0, 3],
      // DEFERRED[中] Bug: 所有 NetCDF 路径直接作为命令行参数，Windows CreateProcess 命令行上限约 32767 字符，按天下载的一年数据（数百个长路径）会让 spawn 失败（ENAMETOOLONG），并落入 launch 中 error 后不一定触发 close 的问题；建议：把文件列表写入临时清单文件，用 --files-from <list> 之类的参数传给引擎
      start() { launch(job, ['era5-register', index, '--product', product, ...files.map(windowsPath)]) },
      // FIXED[低] 资源泄漏: 任务失败或被停止时，引擎可能已写出不完整的 pack-*.json，这里不清理，之后会出现在列表中；已修复：status 非 completed 时删除该索引文件
      collect(status) {
        if (status !== 'completed') {
          try { if (existsSync(index)) unlinkSync(index) } catch {}
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

  function removePack(value) {
    const path = resolve(normalizeHostPath(String(value || '').trim()))
    if (dirname(path).toLowerCase() !== resolve(era5Directory()).toLowerCase() || !existsSync(path)) throw new Error('只能移除登记目录中的 ERA5 数据包索引')
    if (!packSummary(path)) throw new Error('不是 ERA5 数据包索引')
    unlinkSync(path)
    return { removed: true, path: windowsPath(path) }
  }

  // ---------- dialogs ----------
  function pick(kind) {
    if (process.platform !== 'win32') throw new Error('浏览选择仅在 Windows 上可用，请直接粘贴绝对路径')
    const filters = {
      file: ['选择栅格文件', '栅格 (*.tif;*.tiff;*.img;*.dat;*.nc;*.h5;*.hdf)|*.tif;*.tiff;*.img;*.dat;*.nc;*.h5;*.hdf|所有文件 (*.*)|*.*', false],
      project: ['打开遥感影像模拟工程', 'STREAM 工程 (*.json)|*.json|所有文件 (*.*)|*.*', false],
      era5: ['选择 ERA5 / ERA5-Land 逐小时 NetCDF 文件（可多选，覆盖一天到一年均可）', 'NetCDF (*.nc;*.nc4)|*.nc;*.nc4|所有文件 (*.*)|*.*', true]
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

  // ---------- routes ----------
  const routes = {
    'GET /api/stream/status': () => status(),
    'POST /api/stream/run': (data) => submitRun(data),
    'POST /api/stream/stop': (data) => stop(data.id),
    'POST /api/stream/inspect': (data) => ({ rasters: rasters.inspectRasters(data.paths) }),
    // DEFERRED[中] 性能: raster 与 class-stats 在请求处理中同步执行 readTiff（readFileSync 整个文件 + 全幅 Float64 解码），大影像会阻塞事件循环数秒，期间 SSE 进度与其它 API 全部停顿，还可能内存溢出；建议：放到 worker_threads 执行，并对像元总数设上限/按抽样行列读取
    'POST /api/stream/raster': (data) => rasters.rasterBand(data.path, Number(data.band) || 0),
    'POST /api/stream/runs': (data) => ({ runs: listRuns(data.outputRoot) }),
    'POST /api/stream/run-info': (data) => readRun(data.outputDir),
    'POST /api/stream/class-stats': (data) => {
      const run = readRun(data.outputDir)
      return rasters.classStatistics(resolve(normalizeHostPath(run.outputDir)), run.typeFile, run.typeBand)
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
    'POST /api/stream/pick': async (data) => {
      const kind = ['folder', 'project', 'era5'].includes(data.kind) ? data.kind : 'file'
      const paths = await pick(kind)
      return paths.length ? { path: paths[0], paths } : { cancelled: true }
    }
  }

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

  // 进程随后就退出，等不到 close 事件，运行清单在这里同步写成 stopped。
  function shutdown() {
    queue.length = 0
    const job = active
    if (!job?.child) return
    job.stopped = true
    if (job.manifest) {
      try { writeManifest(job, { status: 'stopped', exitCode: null, finishedAt: new Date().toISOString(), products: products(job.outputDir).map((path) => basename(path)) }) } catch {}
    }
    // FIXED[低] 资源泄漏: 退出时只 kill 引擎主进程，Windows 下不像 stop() 那样用 taskkill /T 结束整个进程树；已修复：win32 下同步执行 taskkill /T /F 结束进程树（ERA5 未完成索引的清理未加）
    if (process.platform === 'win32') {
      try { spawnSync('taskkill', ['/pid', String(job.child.pid), '/T', '/F'], { windowsHide: true }) } catch { job.child.kill('SIGTERM') }
    } else job.child.kill('SIGTERM')
  }

  // STREAM 工程独立于三维工程：只打开 stream-project.json（或旧版 STREAM 工程）。
  return { handle, shutdown, executable: () => windowsPath(engine()), openProjectFile: (path) => projects.readProjectFile(path) }
}
