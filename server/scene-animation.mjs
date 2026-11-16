// HiStream eSceneAnimation research runs (/api/scene-animation/*). A run uses the single HiStream
// engine slot shared with /api/run, so a scene animation and a 3D project run never overlap. The
// input is an existing scene JSON; the engine validates every scene field and writes a new folder.
import { execFile, spawn } from 'node:child_process'
import { createHash, randomUUID } from 'node:crypto'
import { closeSync, createReadStream, existsSync, lstatSync, mkdirSync, openSync, readFileSync, readSync, readdirSync, realpathSync, renameSync, statSync, writeFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { basename, dirname, extname, isAbsolute, join, resolve } from 'node:path'
import { createInterface } from 'node:readline'
import {
  createSceneAnimationJob, SCENE_ANIMATION_FRAME_KINDS, SCENE_ANIMATION_JOB_FILE, SCENE_ANIMATION_JOB_FORMAT,
  SCENE_ANIMATION_MODE, sceneAnimationFileReferences, validateSceneAnimationInput, validateSceneAnimationJob
} from '../src/renderer/src/project-schema.js'

// Development only: run scene animations with another histream.exe (for example a separate
// HISTREAM_ENABLE_SCENE_ANIMATION=ON build) while the product engine stays unchanged.
export const SCENE_ANIMATION_ENGINE_VARIABLE = 'STREAM_STUDIO_SCENE_ANIMATION_ENGINE'
export const SCENE_ANIMATION_ENGINE_LOG = 'scene-animation-engine.log'

const FRAME_LINE = /^FRAME\t(\d+)\/(\d+)\tt=([^\t]*)/
const FAILURE_LINE = /HiStream GPU scene animation failed:\s*(.*)$/
const MODULE_DISABLED_LINE = /scene animation module is not enabled/i
// Engines that predate eSceneAnimation reject the mode with the general usage text.
const LEGACY_USAGE_LINE = /^Usage: histream \[eVoxelEB\|/
const MISSING_SHADER = /Missing [^:]*(SPIR-V|shader)/i
const FRAME_FILE = /^frame_(\d{4,5})\.png$/i
// Additional cameras (sequence.additionalCameras) write the same folders under cameras/<id>/.
const CAMERA_ID = /^[A-Za-z0-9_-]{1,32}$/
const CAMERA_KIND = /^cameras\/([A-Za-z0-9_-]{1,32})\/([a-z_]+)$/
// Static `mesh` backend images are written beside run-manifest.json as <camera>_<product>.png.
const VIEW_FILE = /^[A-Za-z0-9_.-]{1,120}_(visible|thermal|material|normal)\.png$/
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
const MAX_INPUT_BYTES = 32 * 1024 * 1024
const MAX_MANIFEST_BYTES = 32 * 1024 * 1024
const MAX_FRAME_BYTES = 512 * 1024 * 1024
const MAX_LOG_LINES = 400
const MAX_SAVED_LOG_BYTES = 4 * 1024 * 1024
const MAX_LISTED_FRAMES = 20000
const PROGRESS_INTERVAL_MS = 100

const isUncPath = (value) => /^[\\/]{2}/.test(String(value || ''))
const errorWithStatus = (message, statusCode) => Object.assign(new Error(message), { statusCode })
const finiteOrNull = (value) => (typeof value === 'number' && Number.isFinite(value) ? value : null)
const integerOrNull = (value) => (Number.isInteger(value) ? value : null)
const textOrEmpty = (value, limit = 2000) => (typeof value === 'string' ? value.slice(0, limit) : '')
const booleanOrNull = (value) => (typeof value === 'boolean' ? value : null)

function samePath(left, right) {
  const normalize = (value) => {
    const path = resolve(String(value || '')).replaceAll('\\', '/')
    return process.platform === 'win32' ? path.toLowerCase() : path
  }
  return Boolean(left) && Boolean(right) && normalize(left) === normalize(right)
}

function fileIdentity(path) {
  try {
    const hash = createHash('sha256')
    const descriptor = openSync(path, 'r')
    const buffer = Buffer.allocUnsafe(1024 * 1024)
    try {
      let count
      while ((count = readSync(descriptor, buffer, 0, buffer.length, null)) > 0) hash.update(buffer.subarray(0, count))
    } finally { closeSync(descriptor) }
    return { path, bytes: statSync(path).size, sha256: hash.digest('hex') }
  } catch { return null }
}

function readJsonFile(path, limit = MAX_MANIFEST_BYTES) {
  try {
    const stat = statSync(path)
    if (!stat.isFile() || stat.size > limit) return null
    const value = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''))
    return value && typeof value === 'object' && !Array.isArray(value) ? value : null
  } catch { return null }
}

// The procedural reference writes kind=histream-gpu-scene-animation; mesh backends write backend=mesh|meshSequence.
export function sceneAnimationManifestBackend(manifest) {
  if (!manifest) return ''
  if (manifest.kind === 'histream-gpu-scene-animation') return 'procedural'
  return ['mesh', 'meshSequence'].includes(manifest.backend) ? manifest.backend : ''
}

export function isSceneAnimationManifest(manifest) {
  return Boolean(sceneAnimationManifestBackend(manifest)) && typeof manifest.status === 'string'
}

function plainCopy(value, depth = 0) {
  if (typeof value === 'string') return value.slice(0, 2000)
  if (typeof value === 'number') return Number.isFinite(value) ? value : null
  if (typeof value === 'boolean' || value === null) return value
  if (depth >= 4 || !value || typeof value !== 'object') return undefined
  if (Array.isArray(value)) return value.slice(0, 50).map((item) => plainCopy(item, depth + 1)).filter((item) => item !== undefined)
  const result = {}
  for (const [key, item] of Object.entries(value).slice(0, 80)) {
    const copied = plainCopy(item, depth + 1)
    if (copied !== undefined) result[key] = copied
  }
  return result
}

// Fields the GUI shows; impactModel is passed through generically because new model kinds
// (for example the empirical impact-v1 estimate) add their own label and presentation entries.
export function sceneAnimationManifestSummary(manifest) {
  if (!manifest) return null
  const backend = sceneAnimationManifestBackend(manifest)
  const timeline = manifest.timeline && typeof manifest.timeline === 'object' ? manifest.timeline : {}
  const observation = manifest.cameraObservation && typeof manifest.cameraObservation === 'object' ? manifest.cameraObservation : null
  return {
    status: textOrEmpty(manifest.status, 80),
    kind: textOrEmpty(manifest.kind, 120),
    backend,
    device: textOrEmpty(manifest.device, 200),
    width: integerOrNull(manifest.width),
    height: integerOrNull(manifest.height),
    fps: finiteOrNull(manifest.fps),
    firstFrame: integerOrNull(manifest.firstFrame),
    lastFrame: integerOrNull(manifest.lastFrame),
    frameCount: integerOrNull(manifest.frameCount),
    timelineFrameCount: integerOrNull(manifest.timelineFrameCount),
    impactTime: finiteOrNull(manifest.impactTime),
    wallSeconds: finiteOrNull(manifest.wallSeconds),
    physicsValidated: booleanOrNull(manifest.physicsValidated),
    radiometricallyValidated: booleanOrNull(manifest.radiometricallyValidated),
    renderMode: textOrEmpty(manifest.renderMode, 80),
    scope: textOrEmpty(manifest.scope),
    impactModel: manifest.impactModel && typeof manifest.impactModel === 'object' && !Array.isArray(manifest.impactModel) ? plainCopy(manifest.impactModel) : null,
    warnings: Array.isArray(manifest.warnings) ? manifest.warnings.filter((item) => typeof item === 'string').map((item) => item.slice(0, 2000)).slice(0, 100) : [],
    motionBackend: textOrEmpty(timeline.motionBackend, 80) || (backend === 'meshSequence' ? 'authored' : ''),
    cameraObservation: observation ? { enabled: observation.enabled !== false, limitations: textOrEmpty(observation.limitations) } : null,
    optical: manifest.optical && typeof manifest.optical === 'object' ? manifest.optical.enabled === true : false,
    views: Array.isArray(manifest.views) ? manifest.views.length : null,
    // impact-v1: physical seconds per presentation second (frame time t = (n-1)/fps x scale).
    presentationTimeScale: finiteOrNull(manifest.impactModel?.presentation?.presentationTimeScale),
    additionalCameras: Array.isArray(manifest.additionalCameras)
      ? manifest.additionalCameras.filter((item) => item && CAMERA_ID.test(String(item.id))).slice(0, 8).map((item) => ({ id: String(item.id), camera: textOrEmpty(item.camera, 120), lastFrame: integerOrNull(item.lastFrame) }))
      : []
  }
}

function localStamp(date = new Date()) {
  const pad = (value) => String(value).padStart(2, '0')
  return `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}-${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`
}

export function createSceneAnimationService({ json, body, emit, normalizeHostPath, windowsPath, pathWithin, realPathWithin, defaultEngine, runtimePath = (value) => value, engineSlot, appVersion = '' }) {
  let active = null
  let lastRun = null
  let pickInFlight = null

  const defaultOutputRoot = () => join(homedir(), 'Documents', 'Stream Studio', 'scene-animation')

  function hostPath(value, label) {
    const source = normalizeHostPath(String(value ?? '').trim().replace(/^"(.*)"$/, '$1')).trim()
    if (!source) throw new Error(`请填写${label}`)
    if (isUncPath(source)) throw new Error(`${label}不支持网络（UNC）路径`)
    if (!isAbsolute(source)) throw new Error(`${label}必须是绝对路径`)
    const path = resolve(source)
    if (isUncPath(path)) throw new Error(`${label}不支持网络（UNC）路径`)
    return path
  }

  // ---------- engine ----------
  function moduleMissingMessage(path) {
    return `当前 HiStream 引擎不包含场景动画（${SCENE_ANIMATION_MODE}）模块：${windowsPath(join(dirname(path), 'shader', 'animation'))} 不存在。请使用以 HISTREAM_ENABLE_SCENE_ANIMATION=ON 构建的引擎；开发调试时可设置环境变量 ${SCENE_ANIMATION_ENGINE_VARIABLE} 指向该 histream.exe。`
  }

  function engineInfo() {
    const override = String(process.env[SCENE_ANIMATION_ENGINE_VARIABLE] || '').trim().replace(/^"(.*)"$/, '$1')
    let path = ''
    let error = ''
    if (override) {
      path = resolve(normalizeHostPath(override))
      const name = basename(path).toLowerCase()
      let isFile = false
      try { isFile = statSync(path).isFile() } catch {}
      if (isUncPath(path) || !['histream.exe', 'histream'].includes(name) || !isFile)
        error = `环境变量 ${SCENE_ANIMATION_ENGINE_VARIABLE} 必须指向已有的 histream.exe：${windowsPath(path)}`
    } else path = defaultEngine()
    let exists = false
    try { exists = !error && statSync(path).isFile() } catch {}
    const shaderRoot = join(dirname(path), 'shader', 'animation')
    const module = exists && existsSync(shaderRoot)
    const meshBackends = module && existsSync(join(shaderRoot, 'mesh'))
    if (!error && !exists) error = `找不到 HiStream 引擎：${windowsPath(path)}`
    if (!error && !module) error = moduleMissingMessage(path)
    return { path, override: Boolean(override), exists, module, meshBackends, error }
  }

  const publicEngine = (engine) => ({ path: windowsPath(engine.path), override: engine.override, overrideVariable: SCENE_ANIMATION_ENGINE_VARIABLE,
    exists: engine.exists, module: engine.module, meshBackends: engine.meshBackends, ready: !engine.error, error: engine.error })

  function backendSupportError(engine, backend) {
    if (engine.error) return engine.error
    if (['mesh', 'meshSequence'].includes(backend) && !engine.meshBackends)
      return `当前 HiStream 引擎的场景动画模块版本过旧，不支持 backend=${backend}（缺少 ${windowsPath(join(dirname(engine.path), 'shader', 'animation', 'mesh'))}）。请更新引擎后再运行。`
    return ''
  }

  // ---------- input ----------
  function missingReferences(input, value) {
    const missing = []
    for (const reference of sceneAnimationFileReferences(value)) {
      const stored = normalizeHostPath(reference.path)
      if (isUncPath(stored)) continue
      const path = isAbsolute(stored) ? resolve(stored) : resolve(dirname(input), stored)
      if (!existsSync(path)) missing.push(`${reference.pointer} → ${windowsPath(path)}`)
    }
    return missing
  }

  function readSceneInput(path) {
    let stat
    try { stat = statSync(path) } catch { throw new Error(`找不到场景动画输入：${windowsPath(path)}`) }
    if (!stat.isFile()) throw new Error(`场景动画输入不是文件：${windowsPath(path)}`)
    if (extname(path).toLowerCase() !== '.json') throw new Error('场景动画输入必须是 .json 文件')
    if (stat.size > MAX_INPUT_BYTES) throw new Error(`场景动画输入超过 ${MAX_INPUT_BYTES / 1024 / 1024}MB`)
    let value
    try { value = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) }
    catch (error) { throw new Error(`场景动画输入不是有效的 JSON：${error.message}`) }
    const checked = validateSceneAnimationInput(value)
    if (checked.valid) {
      const missing = missingReferences(path, value)
      if (missing.length) {
        checked.errors.push(`输入引用的 ${missing.length} 个文件不存在：${missing.slice(0, 5).join('；')}${missing.length > 5 ? ' …' : ''}`)
        checked.valid = false
      }
    }
    return { path, value, ...checked }
  }

  function inspect(data) {
    const path = hostPath(data.input, '场景动画输入')
    const scene = readSceneInput(path)
    const engine = engineInfo()
    return {
      input: windowsPath(path), name: basename(path), valid: scene.valid, errors: scene.errors, warnings: scene.warnings,
      backend: scene.backend, summary: scene.summary, engine: publicEngine(engine),
      engineError: scene.valid ? backendSupportError(engine, scene.backend) : engine.error,
      defaultOutputRoot: windowsPath(defaultOutputRoot())
    }
  }

  // ---------- runs ----------
  function resolveOutputRoot(value, engine) {
    const root = String(value || '').trim() ? hostPath(value, '输出目录') : defaultOutputRoot()
    if (pathWithin(dirname(engine), root)) throw new Error('输出目录不能位于 HiStream 引擎目录内')
    if (existsSync(root) && !statSync(root).isDirectory()) throw new Error(`输出目录不是文件夹：${windowsPath(root)}`)
    mkdirSync(root, { recursive: true })
    return root
  }

  // The engine refuses existing output folders, so every run gets a new, never-reused name.
  function newRunDirectory(root, input) {
    const stem = basename(input, extname(input)).replace(/[^A-Za-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 40) || 'scene'
    const stamp = localStamp()
    for (let attempt = 0; attempt < 20; attempt += 1) {
      const runId = `${stem}_${stamp}_${randomUUID().slice(0, 6)}`
      const outputDir = join(root, runId)
      if (!existsSync(outputDir)) return { runId, outputDir }
    }
    throw new Error('无法生成新的输出目录名')
  }

  const publicRun = (run) => run && ({
    runId: run.runId, status: run.status, input: windowsPath(run.input), outputRoot: windowsPath(run.outputRoot),
    outputDir: windowsPath(run.outputDir), backend: run.backend, pid: run.process?.pid ?? null,
    startedAt: run.startedAt, finishedAt: run.finishedAt || null, frame: run.frame, count: run.count, time: run.time,
    first: run.first, last: run.last, percent: run.percent, exitCode: run.exitCode ?? null, error: run.error || '',
    warnings: [...run.engineWarnings], engine: windowsPath(run.engine), engineOverride: run.engineOverride
  })

  function pushLog(run, level, text) {
    const line = String(text || '').replace(/\r/g, '')
    if (!line) return
    run.log.push({ level, text: line })
    if (run.log.length > MAX_LOG_LINES) run.log.splice(0, run.log.length - MAX_LOG_LINES)
    emit({ type: 'scene-animation-log', runId: run.runId, level, text: line })
  }

  function saveLogLine(run, stream, line) {
    if (run.savedLogBytes >= MAX_SAVED_LOG_BYTES) return
    const entry = `${stream === 'stderr' ? '[stderr] ' : ''}${line}\n`
    run.savedLogBytes += Buffer.byteLength(entry)
    run.savedLog.push(entry)
  }

  function readRange(run) {
    if (run.rangeKnown) return
    const manifest = readJsonFile(join(run.outputDir, 'run-manifest.json'))
    const first = integerOrNull(manifest?.firstFrame) ?? run.inputSummary?.firstFrame ?? null
    const last = integerOrNull(manifest?.lastFrame) ?? run.inputSummary?.lastFrame ?? null
    if (first !== null && last !== null && last >= first) { run.first = first; run.last = last }
    run.rangeKnown = Boolean(manifest) || run.first !== null
  }

  function handleStdout(run, line) {
    saveLogLine(run, 'stdout', line)
    const frame = FRAME_LINE.exec(line)
    if (!frame) { pushLog(run, 'info', line); return }
    run.frame = Number(frame[1])
    run.count = Number(frame[2])
    run.time = Number(frame[3])
    readRange(run)
    const first = run.first ?? 1
    const last = run.last ?? run.count
    const total = last - first + 1
    // The engine still drains PNG writers and rewrites run-manifest.json after the last FRAME line.
    run.percent = total > 0 ? Math.max(0, Math.min(99, Math.floor((run.frame - first + 1) / total * 100))) : null
    const now = Date.now()
    if (run.frame === last || run.frame === first || now - run.progressAt >= PROGRESS_INTERVAL_MS) {
      run.progressAt = now
      emit({ type: 'scene-animation-progress', runId: run.runId, frame: run.frame, count: run.count, time: run.time, first, last, percent: run.percent })
    }
  }

  function handleStderr(run, line) {
    saveLogLine(run, 'stderr', line)
    const failure = FAILURE_LINE.exec(line)
    if (failure) run.failure = failure[1].trim()
    if (MODULE_DISABLED_LINE.test(line) || LEGACY_USAGE_LINE.test(line)) run.moduleMissing = true
    if (/^WARNING:/i.test(line)) {
      run.engineWarnings.add(line.replace(/^WARNING:\s*/i, '').slice(0, 2000))
      pushLog(run, 'warning', line)
    } else pushLog(run, failure ? 'error' : 'stderr', line)
  }

  function failureMessage(run, code, signal, manifest) {
    if (run.moduleMissing) return `${moduleMissingMessage(run.engine)}（引擎输出：模块未启用或不支持 ${SCENE_ANIMATION_MODE}）`
    if (run.failure) return MISSING_SHADER.test(run.failure)
      ? `场景动画模块的着色器文件缺失，请重新部署引擎的 shader\\animation 目录：${run.failure}`
      : `场景动画运行失败：${run.failure}`
    if (code === 0) return `引擎已退出，但 run-manifest.json 未标记为完成（status=${manifest?.status || '缺失'}）`
    return `HiStream 引擎异常退出（退出码 ${code ?? '无'}${signal ? `，信号 ${signal}` : ''}）`
  }

  function writeAtomic(path, content) {
    writeFileSync(`${path}.tmp`, content, 'utf8')
    renameSync(`${path}.tmp`, path)
  }

  function writeRunRecords(run, manifest) {
    if (!existsSync(run.outputDir)) return
    const job = createSceneAnimationJob({ input: windowsPath(run.input), outputRoot: windowsPath(run.outputRoot), label: basename(run.input), createdAt: new Date(run.startedAt).toISOString() })
    job.run = {
      runId: run.runId, outputDir: windowsPath(run.outputDir), status: run.status, error: run.error || '',
      startedAt: new Date(run.startedAt).toISOString(), finishedAt: run.finishedAt ? new Date(run.finishedAt).toISOString() : null,
      exitCode: run.exitCode ?? null, signal: run.signal ?? null, backend: run.backend,
      manifestBackend: sceneAnimationManifestBackend(manifest), manifestStatus: typeof manifest?.status === 'string' ? manifest.status : null,
      lastFrame: run.frame === null ? null : { frame: run.frame, count: run.count, time: run.time },
      engineWarnings: [...run.engineWarnings], engineOverride: run.engineOverride,
      engine: run.engineIdentity && { ...run.engineIdentity, path: windowsPath(run.engineIdentity.path) },
      input: run.inputIdentity && { ...run.inputIdentity, path: windowsPath(run.inputIdentity.path) },
      command: [run.engine, ...run.args].map((item) => windowsPath(item)),
      engineLog: SCENE_ANIMATION_ENGINE_LOG, application: { name: 'Stream Studio', version: appVersion }
    }
    writeAtomic(join(run.outputDir, SCENE_ANIMATION_JOB_FILE), `${JSON.stringify(job, null, 2)}\n`)
    writeAtomic(join(run.outputDir, SCENE_ANIMATION_ENGINE_LOG), run.savedLog.join('') + (run.savedLogBytes >= MAX_SAVED_LOG_BYTES ? '[日志超过 4MB，后续输出未保存]\n' : ''))
  }

  function finish(run, code, signal) {
    if (run.finished) return
    run.finished = true
    const manifest = readJsonFile(join(run.outputDir, 'run-manifest.json'))
    const actualBackend = sceneAnimationManifestBackend(manifest)
    const running = run.process
    let status = 'failed'
    let error = ''
    if (run.stopRequested || running?.histreamStopped || running?.histreamReset) { status = 'stopped'; error = '运行已停止，输出目录中只有部分帧' }
    else if (run.spawnError) error = `无法启动 HiStream 引擎：${run.spawnError.message}`
    else if (code === 0 && manifest?.status === 'complete' && actualBackend && actualBackend !== run.backend)
      error = `引擎没有按输入的 backend=${run.backend} 运行（实际输出 ${actualBackend}），说明当前引擎的场景动画模块不支持该后端，请更新引擎。`
    else if (code === 0 && manifest?.status === 'complete') status = 'complete'
    else error = failureMessage(run, code, signal, manifest)
    Object.assign(run, { status, error, exitCode: code, signal, finishedAt: Date.now() })
    if (status === 'complete') run.percent = 100
    if (error) pushLog(run, status === 'stopped' ? 'warning' : 'error', error)
    try { writeRunRecords(run, manifest) } catch (recordError) { pushLog(run, 'error', `无法写入运行记录：${recordError.message}`) }
    emit({ type: 'scene-animation-closed', runId: run.runId, status, code, signal, error, outputDir: windowsPath(run.outputDir),
      outputExists: existsSync(run.outputDir), elapsed: run.finishedAt - run.startedAt, warnings: [...run.engineWarnings] })
    if (active === run) active = null
    lastRun = run
    engineSlot.release(running)
  }

  function startRun(data) {
    if (['executable', 'engine', 'args', 'command', 'mode'].some((key) => data[key] !== undefined))
      throw new Error('场景动画只运行已解析的 HiStream 引擎，不接受自定义程序、模式或命令参数')
    const checked = validateSceneAnimationJob({ input: data.input, outputRoot: data.outputRoot || '' })
    if (!checked.valid) throw new Error(checked.errors.join('；'))
    const busy = engineSlot.busyReason()
    if (busy) throw errorWithStatus(busy, 409)
    const input = hostPath(checked.job.input, '场景动画输入')
    const scene = readSceneInput(input)
    if (!scene.valid) throw new Error(scene.errors.join('；'))
    const engine = engineInfo()
    const engineError = backendSupportError(engine, scene.backend)
    if (engineError) throw new Error(engineError)
    const outputRoot = resolveOutputRoot(checked.job.outputRoot, engine.path)
    const { runId, outputDir } = newRunDirectory(outputRoot, input)
    const args = [SCENE_ANIMATION_MODE, runtimePath(input, engine.path), runtimePath(outputDir, engine.path)]
    const run = {
      runId, input, outputRoot, outputDir, engine: engine.path, engineOverride: engine.override, args, backend: scene.backend,
      inputSummary: scene.summary, status: 'running', startedAt: Date.now(), finishedAt: 0,
      frame: null, count: null, time: null, first: null, last: null, percent: null, rangeKnown: false, progressAt: 0,
      log: [], savedLog: [], savedLogBytes: 0, engineWarnings: new Set(), failure: '', moduleMissing: false,
      engineIdentity: fileIdentity(engine.path), inputIdentity: fileIdentity(input), process: null
    }
    // Checked again immediately before spawning: nothing above may await, so the slot claim is atomic.
    const busyNow = engineSlot.busyReason()
    if (busyNow) throw errorWithStatus(busyNow, 409)
    const running = spawn(engine.path, args, { cwd: dirname(engine.path), windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] })
    running.sceneAnimationRun = { runId, outputDir: windowsPath(outputDir), startedAt: run.startedAt }
    run.process = running
    engineSlot.claim(running, engine.path)
    active = run
    running.stdout.setEncoding('utf8')
    running.stderr.setEncoding('utf8')
    createInterface({ input: running.stdout, crlfDelay: Infinity }).on('line', (line) => handleStdout(run, line))
    createInterface({ input: running.stderr, crlfDelay: Infinity }).on('line', (line) => handleStderr(run, line))
    running.on('error', (error) => {
      run.spawnError = error
      if (!running.pid) finish(run, null, null)
    })
    running.on('close', (code, signal) => {
      try { finish(run, code, signal) }
      catch (error) {
        try { emit({ type: 'scene-animation-closed', runId, status: 'failed', error: `结果处理失败：${error.message}`, outputDir: windowsPath(outputDir) }) } catch {}
        if (active === run) active = null
        engineSlot.release(running)
      }
    })
    const command = `"${windowsPath(engine.path)}" ${args.map((item) => `"${windowsPath(item)}"`).join(' ')}`
    emit({ type: 'scene-animation-started', runId, pid: running.pid ?? null, input: windowsPath(input), outputDir: windowsPath(outputDir),
      backend: scene.backend, first: scene.summary?.firstFrame ?? null, last: scene.summary?.lastFrame ?? null, command, engineOverride: engine.override })
    pushLog(run, 'info', `命令：${command}`)
    if (engine.override) pushLog(run, 'warning', `开发用引擎（${SCENE_ANIMATION_ENGINE_VARIABLE}）：${windowsPath(engine.path)}`)
    for (const warning of scene.warnings) pushLog(run, 'warning', warning)
    return { ...publicRun(run), summary: scene.summary, warnings: scene.warnings, command }
  }

  function stopProcess(running) {
    if (process.platform === 'win32') {
      execFile('taskkill', ['/PID', String(running.pid), '/T', '/F'], { windowsHide: true }, (error) => { if (error) running.kill() })
    } else running.kill('SIGTERM')
  }

  function stop() {
    const run = active
    if (!run?.process || run.finished) return { stopped: false }
    run.stopRequested = true
    run.process.histreamStopped = true
    pushLog(run, 'warning', '正在停止场景动画…')
    stopProcess(run.process)
    return { stopped: true, runId: run.runId }
  }

  // Stream Studio is exiting: no close event will follow, so record the stop synchronously.
  function shutdown() {
    const run = active
    if (!run || run.finished) return
    run.stopRequested = true
    Object.assign(run, { status: 'stopped', error: 'Stream Studio 退出时停止了运行', finishedAt: Date.now() })
    try { writeRunRecords(run, readJsonFile(join(run.outputDir, 'run-manifest.json'))) } catch {}
  }

  function status() {
    const engine = engineInfo()
    const run = active || lastRun
    return {
      mode: SCENE_ANIMATION_MODE, engine: publicEngine(engine), defaultOutputRoot: windowsPath(defaultOutputRoot()),
      busy: engineSlot.busyReason(), running: active ? publicRun(active) : null, last: lastRun ? publicRun(lastRun) : null,
      log: run ? run.log.slice(-200) : []
    }
  }

  // ---------- results ----------
  function isSceneAnimationResult(directory) {
    if (isSceneAnimationManifest(readJsonFile(join(directory, 'run-manifest.json')))) return true
    return readJsonFile(join(directory, SCENE_ANIMATION_JOB_FILE), 4 * 1024 * 1024)?.format === SCENE_ANIMATION_JOB_FORMAT
  }

  // Only folders written by eSceneAnimation are readable here; frames are served from inside them.
  function resultDirectory(value) {
    let path = hostPath(value, '结果目录')
    try {
      const name = basename(path).toLowerCase()
      if (statSync(path).isFile() && ['run-manifest.json', SCENE_ANIMATION_JOB_FILE].includes(name)) path = dirname(path)
    } catch {}
    let real
    try { real = realpathSync(path) } catch { throw new Error(`找不到结果目录：${windowsPath(path)}`) }
    if (!statSync(real).isDirectory()) throw new Error(`结果路径不是文件夹：${windowsPath(path)}`)
    if (!isSceneAnimationResult(real)) throw new Error(`该目录不是场景动画结果（缺少 ${SCENE_ANIMATION_MODE} 写出的 run-manifest.json）：${windowsPath(path)}`)
    return real
  }

  function frameKinds(directory) {
    const kinds = []
    const scan = (base, prefix) => {
      for (const kind of SCENE_ANIMATION_FRAME_KINDS) {
        const folder = join(base, kind)
        let names
        try {
          if (!lstatSync(folder).isDirectory()) continue
          names = readdirSync(folder)
        } catch { continue }
        const frames = names.map((name) => FRAME_FILE.exec(name)).filter(Boolean).map((match) => Number(match[1])).sort((a, b) => a - b)
        if (frames.length) kinds.push({ kind: prefix + kind, count: frames.length, frames: frames.slice(0, MAX_LISTED_FRAMES) })
      }
    }
    scan(directory, '')
    let cameras = []
    try { cameras = readdirSync(join(directory, 'cameras')).filter((name) => CAMERA_ID.test(name)).sort() } catch {}
    for (const id of cameras.slice(0, 8)) {
      try { if (!lstatSync(join(directory, 'cameras', id)).isDirectory()) continue } catch { continue }
      scan(join(directory, 'cameras', id), `cameras/${id}/`)
    }
    return kinds
  }
  // Frame folder of a kind: "frames" or "cameras/<id>/frames" (no other separators or names).
  function frameFolder(kind) {
    if (SCENE_ANIMATION_FRAME_KINDS.includes(kind)) return kind
    const match = CAMERA_KIND.exec(kind)
    if (match && SCENE_ANIMATION_FRAME_KINDS.includes(match[2])) return join('cameras', match[1], match[2])
    return ''
  }

  function viewImages(directory) {
    try { return readdirSync(directory).filter((name) => VIEW_FILE.test(name)).sort() } catch { return [] }
  }

  function outputEntries(directory) {
    const entries = []
    let names = []
    try { names = readdirSync(directory, { withFileTypes: true }) } catch {}
    for (const entry of names.sort((a, b) => a.name.localeCompare(b.name)).slice(0, 300)) {
      const path = join(directory, entry.name)
      try {
        if (entry.isDirectory()) entries.push({ name: entry.name, type: 'directory', entries: readdirSync(path).length })
        else if (entry.isFile()) entries.push({ name: entry.name, type: 'file', bytes: statSync(path).size })
      } catch {}
    }
    return entries
  }

  function publicJob(job) {
    if (!job || job.format !== SCENE_ANIMATION_JOB_FORMAT) return null
    const run = job.run && typeof job.run === 'object' ? job.run : {}
    return {
      input: textOrEmpty(job.input, 4096), outputRoot: textOrEmpty(job.outputRoot, 4096), createdAt: textOrEmpty(job.createdAt, 40),
      status: textOrEmpty(run.status, 40), error: textOrEmpty(run.error), startedAt: textOrEmpty(run.startedAt, 40),
      finishedAt: textOrEmpty(run.finishedAt, 40), exitCode: integerOrNull(run.exitCode), backend: textOrEmpty(run.backend, 40),
      engine: textOrEmpty(run.engine?.path, 4096), engineSha256: textOrEmpty(run.engine?.sha256, 64), engineOverride: run.engineOverride === true,
      engineWarnings: Array.isArray(run.engineWarnings) ? run.engineWarnings.filter((item) => typeof item === 'string').slice(0, 50) : []
    }
  }

  function readResult(value) {
    const directory = resultDirectory(value)
    const manifest = readJsonFile(join(directory, 'run-manifest.json'))
    const job = publicJob(readJsonFile(join(directory, SCENE_ANIMATION_JOB_FILE), 4 * 1024 * 1024))
    const views = viewImages(directory)
    return {
      outputDir: windowsPath(directory), running: Boolean(active && samePath(active.outputDir, directory)),
      summary: sceneAnimationManifestSummary(isSceneAnimationManifest(manifest) ? manifest : null), job,
      frames: frameKinds(directory), views, files: outputEntries(directory)
    }
  }

  function serveFrame(request, response, url) {
    const site = request.headers['sec-fetch-site']
    if (site && !['same-origin', 'none'].includes(site)) return json(response, 403, { error: '拒绝跨站请求' })
    try {
      const kind = String(url.searchParams.get('kind') || '')
      const number = Number(url.searchParams.get('n'))
      if (!Number.isInteger(number) || number < 0 || number > 99999) throw new Error('帧号无效')
      if (kind !== 'views' && !frameFolder(kind)) throw new Error('不支持的帧类型')
      const directory = resultDirectory(url.searchParams.get('dir'))
      const name = kind === 'views' ? viewImages(directory)[number - 1] : join(frameFolder(kind), `frame_${String(number).padStart(4, '0')}.png`)
      const file = name ? join(directory, name) : ''
      if (!file || !pathWithin(directory, file) || !existsSync(file) || !realPathWithin(directory, file)) return json(response, 404, { error: '找不到该帧' })
      const real = realpathSync(file)
      const stat = statSync(real)
      if (!stat.isFile() || stat.size > MAX_FRAME_BYTES) return json(response, 404, { error: '找不到该帧' })
      const header = Buffer.alloc(PNG_SIGNATURE.length)
      const descriptor = openSync(real, 'r')
      try { readSync(descriptor, header, 0, header.length, 0) } finally { closeSync(descriptor) }
      if (!header.equals(PNG_SIGNATURE)) return json(response, 415, { error: '该帧不是 PNG 文件' })
      response.writeHead(200, { 'Content-Type': 'image/png', 'Content-Length': stat.size, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' })
      createReadStream(real).on('error', () => response.destroy()).pipe(response)
      return true
    } catch (error) {
      return json(response, 400, { error: error.message })
    }
  }

  // ---------- dialogs ----------
  function pick(data) {
    if (process.platform !== 'win32') throw new Error('浏览选择仅在 Windows 上可用，请直接粘贴绝对路径')
    if (pickInFlight) throw errorWithStatus('已有一个选择窗口处于打开状态', 409)
    const kind = ['input', 'output', 'result'].includes(data.kind) ? data.kind : 'input'
    let initialDirectory = ''
    try {
      const requested = String(data.initial || '').trim() ? hostPath(data.initial, '初始目录') : ''
      const candidate = requested && existsSync(requested) && statSync(requested).isFile() ? dirname(requested) : requested
      if (candidate && existsSync(candidate) && statSync(candidate).isDirectory()) initialDirectory = candidate
    } catch {}
    const files = {
      input: ['选择场景动画输入 JSON', '场景动画输入 (*.json)|*.json|所有文件 (*.*)|*.*'],
      result: ['选择场景动画结果中的 run-manifest.json', '场景动画结果 (run-manifest.json;scene-animation-job.json)|run-manifest.json;scene-animation-job.json|JSON (*.json)|*.json']
    }
    // The initial folder travels in an environment variable so no user text is parsed as script.
    const script = kind === 'output' ? [
      "$ErrorActionPreference = 'Stop'", 'Add-Type -AssemblyName System.Windows.Forms',
      '$dialog = New-Object System.Windows.Forms.FolderBrowserDialog',
      "$dialog.Description = '选择场景动画输出目录（每次运行在其中新建子目录）'",
      'if ($env:STREAM_STUDIO_PICK_INITIAL) { $dialog.SelectedPath = $env:STREAM_STUDIO_PICK_INITIAL }',
      'if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $dialog.SelectedPath }'
    ] : [
      "$ErrorActionPreference = 'Stop'", 'Add-Type -AssemblyName System.Windows.Forms',
      '$dialog = New-Object System.Windows.Forms.OpenFileDialog',
      `$dialog.Title = '${files[kind][0]}'`, `$dialog.Filter = '${files[kind][1]}'`,
      '$dialog.CheckFileExists = $true', '$dialog.Multiselect = $false',
      'if ($env:STREAM_STUDIO_PICK_INITIAL) { $dialog.InitialDirectory = $env:STREAM_STUDIO_PICK_INITIAL }',
      'if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $dialog.FileName }'
    ]
    pickInFlight = new Promise((resolveChoice, reject) => {
      execFile('powershell.exe', ['-NoProfile', '-STA', '-Command', '[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; ' + script.join('; ')], {
        windowsHide: false, encoding: 'utf8', maxBuffer: 1024 * 1024, timeout: 30 * 60 * 1000,
        env: { ...process.env, STREAM_STUDIO_PICK_INITIAL: initialDirectory }
      }, (error, stdout, stderr) => {
        if (error) return reject(new Error(String(stderr || '').trim() || error.message))
        const path = String(stdout || '').split(/\r?\n/).map((line) => line.trim()).find(Boolean)
        resolveChoice(path ? { path: windowsPath(path) } : { cancelled: true })
      })
    }).finally(() => { pickInFlight = null })
    return pickInFlight
  }

  // ---------- routes ----------
  const routes = {
    'GET /api/scene-animation/status': () => status(),
    'POST /api/scene-animation/inspect': (data) => inspect(data),
    'POST /api/scene-animation/run': (data) => startRun(data),
    'POST /api/scene-animation/stop': () => stop(),
    'POST /api/scene-animation/result': (data) => readResult(data.outputDir),
    'POST /api/scene-animation/pick': (data) => pick(data)
  }

  async function handle(request, response, url) {
    if (!url.pathname.startsWith('/api/scene-animation/')) return false
    if (request.method === 'GET' && url.pathname === '/api/scene-animation/frame') return serveFrame(request, response, url)
    const route = routes[`${request.method} ${url.pathname}`]
    if (!route) return false
    try { return json(response, 200, await route(request.method === 'GET' ? {} : await body(request))) }
    catch (error) { return json(response, error.statusCode || 400, { error: error.message }) }
  }

  return { handle, shutdown, status, statusEvent: () => ({ type: 'scene-animation-status', ...status() }) }
}
