// 场景动画（研究功能）: runs HiStream eSceneAnimation on an existing input JSON through
// /api/scene-animation/*, follows the engine's FRAME progress and shows the run manifest,
// warnings and PNG frame sequences. The input JSON is never edited here.
// Labels stay Chinese in the DOM; i18n.js translates them (paths and engine text are marked data-i18n-ignore).
import { appStorage } from './storage.js'
import { t } from './i18n.js'
import { SCENE_ANIMATION_RESEARCH_NOTICE, SCENE_ANIMATION_UI_ENABLED } from './project-schema.js'

const KIND_LABELS = {
  frames: '红外显示帧', optical_frames: '可见光 RGB 帧', observed_ir: '相机观测红外', observed_optical: '相机观测可见光',
  fixed_frames: '固定标尺红外帧', geometry: '几何预览（导出帧）', views: '静态视图'
}
const KIND_ORDER = Object.keys(KIND_LABELS)
const STATUS_LABELS = { complete: '完成', running: '运行中（或未正常结束）', failed: '失败', stopped: '已停止' }
const BACKEND_LABELS = { procedural: '程序化参考场景', mesh: '静态网格场景', meshSequence: '网格序列动画' }
const MOTION_LABELS = { authored: '作者设定运动', 'rigid-v1': '刚体碎片动力学', 'impact-v1': '经验撞击模型' }
const MAX_LOG_ROWS = 600
// Desktop builds accept only allowlisted keys (desktop/settings.mjs); storage failures never block the panel.
const STORAGE = { input: 'sceneAnimationInput', outputRoot: 'sceneAnimationOutputRoot', result: 'sceneAnimationLastResult' }
const stored = (key) => { try { return appStorage.getItem(key) || '' } catch { return '' } }
const store = (key, value) => { try { if (value) appStorage.setItem(key, value); else appStorage.removeItem(key) } catch {} }
const ENGINE_OVERRIDE_VARIABLE = 'STREAM_STUDIO_SCENE_ANIMATION_ENGINE'

const escapeHtml = (value) => String(value ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]))
const formatNumber = (value, digits = 3) => (Number.isFinite(value) ? String(Number(value.toFixed(digits))) : '—')
const formatBytes = (value) => value < 1024 ? `${value} B` : value < 1024 * 1024 ? `${(value / 1024).toFixed(1)} KB` : `${(value / 1024 / 1024).toFixed(1)} MB`
const elapsedText = (milliseconds) => {
  const seconds = Math.max(0, milliseconds) / 1000
  return `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${(seconds % 60).toFixed(1).padStart(4, '0')}`
}
// "中文标签 · value": i18n.js translates each " · " part separately.
const labelled = (map, value) => (!value ? '—' : map[value] ? `${map[value]} · ${value}` : value)
const unquote = (value) => String(value || '').trim().replace(/^"(.*)"$/, '$1').trim()

export function createSceneAnimationPanel({ api, toast, addLog = () => {} }) {
  const root = document.querySelector('#sceneAnimationDialog')
  const $ = (selector) => root.querySelector(selector)
  const state = {
    open: false, runId: '', finishedRunId: '', running: false, startedAt: 0, progress: null, inputToken: 0,
    result: null, kinds: [], kind: '', index: -1, playing: false, timer: 0, frameToken: 0, elapsedTimer: 0, engine: null
  }

  // ---------- log & progress ----------
  function appendLog(text, level = 'info') {
    const output = $('#sceneAnimationLog')
    for (const line of String(text ?? '').replace(/\r/g, '').split('\n').filter(Boolean)) {
      const row = document.createElement('div')
      row.className = level
      row.innerHTML = `<time>${new Date().toLocaleTimeString('zh-CN', { hour12: false })}</time><span></span>`
      row.querySelector('span').textContent = line
      output.append(row)
    }
    while (output.childElementCount > MAX_LOG_ROWS) output.firstElementChild.remove()
    output.scrollTop = output.scrollHeight
  }

  function renderProgress(stage) {
    const progress = state.progress
    const box = $('#sceneAnimationProgress')
    const percent = state.running ? progress?.percent : progress?.final
    box.classList.toggle('running', state.running)
    box.classList.toggle('indeterminate', state.running && !Number.isFinite(percent))
    if (stage) $('#sceneAnimationProgressStage').textContent = stage
    $('#sceneAnimationProgressPercent').textContent = Number.isFinite(percent) ? `${percent}%` : state.running ? '…' : ''
    $('#sceneAnimationProgressBar').style.width = `${Number.isFinite(percent) ? percent : 0}%`
    const frame = $('#sceneAnimationProgressFrame')
    if (Number.isInteger(progress?.frame)) {
      frame.innerHTML = `<span>帧</span> <b>${progress.frame}</b> / ${escapeHtml(progress.last ?? progress.count ?? '—')}${Number.isFinite(progress.time) ? ` · t = ${escapeHtml(formatNumber(progress.time))} s` : ''}`
    } else frame.textContent = ''
    const end = state.running ? Date.now() : progress?.finishedAt || Date.now()
    $('#sceneAnimationProgressElapsed').textContent = state.startedAt ? `耗时 ${elapsedText(end - state.startedAt)}` : ''
  }

  function setRunning(running) {
    state.running = running
    clearInterval(state.elapsedTimer)
    if (running) state.elapsedTimer = setInterval(() => renderProgress(), 500)
    $('#sceneAnimationRunBtn').disabled = running
    $('#sceneAnimationStopBtn').disabled = !running
    for (const id of ['#sceneAnimationInput', '#sceneAnimationOutputRoot', '#sceneAnimationBrowseInputBtn', '#sceneAnimationBrowseOutputBtn']) $(id).disabled = running
  }

  // ---------- engine & input ----------
  function renderEngine(engine, problem = '') {
    if (engine) state.engine = engine
    const current = state.engine
    const box = $('#sceneAnimationEngine')
    if (!current) { box.classList.remove('error'); box.textContent = '正在检查引擎…'; return }
    const error = problem || current.error
    box.classList.toggle('error', Boolean(error))
    box.innerHTML = `<div><span>引擎</span>${current.override ? ' <em>（开发用覆盖）</em>' : ''}</div><code></code><div class="state"></div>`
    box.querySelector('code').textContent = current.path || '—'
    const message = box.querySelector('.state')
    message.textContent = error || (current.meshBackends ? '场景动画模块可用' : '场景动画模块可用（仅程序化后端）')
    if (error) message.dataset.i18nIgnore = ''
  }

  function messageLine(kind, text) {
    const line = document.createElement('div')
    line.className = kind
    line.dataset.i18nIgnore = ''
    line.textContent = text
    return line
  }

  function renderInputSummary(result, error = '') {
    const box = $('#sceneAnimationInputSummary')
    box.replaceChildren()
    if (error) { box.append(messageLine('error', error)); return }
    if (!result) { box.textContent = '尚未选择输入'; return }
    const s = result.summary || {}
    const rows = [
      ['场景后端', labelled(BACKEND_LABELS, result.backend)],
      ['分辨率', s.width && s.height ? `${s.width} × ${s.height}` : '—'],
      ['帧率', Number.isFinite(s.fps) ? `${formatNumber(s.fps)} fps` : '—'],
      ['帧范围', Number.isInteger(s.firstFrame) && Number.isInteger(s.lastFrame) ? `${s.firstFrame}–${s.lastFrame}` : '—']
    ]
    if (s.motionBackend) rows.push(['运动后端', labelled(MOTION_LABELS, s.motionBackend)])
    if (Number.isFinite(s.presentationTimeScale)) rows.push(['呈现时间比例', `presentationTimeScale = ${formatNumber(s.presentationTimeScale, 6)}`])
    box.append(definitionList(rows))
    for (const message of result.errors || []) box.append(messageLine('error', message))
    for (const message of result.warnings || []) box.append(messageLine('warning', message))
    if (result.valid && result.engineError) box.append(messageLine('error', result.engineError))
  }

  async function inspectInput({ quiet = false } = {}) {
    const input = unquote($('#sceneAnimationInput').value)
    const token = ++state.inputToken
    if (!input) { renderInputSummary(null); return null }
    try {
      const result = await api.inspectSceneAnimation(input)
      if (token !== state.inputToken) return null
      store(STORAGE.input, input)
      renderInputSummary(result)
      renderEngine(result.engine, result.valid ? result.engineError : '')
      if (result.defaultOutputRoot) $('#sceneAnimationOutputRoot').placeholder = result.defaultOutputRoot
      return result
    } catch (error) {
      if (token !== state.inputToken) return null
      renderInputSummary(null, error.message)
      if (!quiet) toast('无法检查场景动画输入', error.message, 'error')
      return null
    }
  }

  async function browse(kind) {
    const field = kind === 'output' ? $('#sceneAnimationOutputRoot') : kind === 'input' ? $('#sceneAnimationInput') : null
    try {
      const initial = field ? unquote(field.value) : stored(STORAGE.result)
      const chosen = await api.pickSceneAnimationPath(kind, initial)
      if (!chosen?.path) return
      if (kind === 'result') { await loadResult(chosen.path); return }
      field.value = chosen.path
      if (kind === 'input') await inspectInput()
      else store(STORAGE.outputRoot, chosen.path)
    } catch (error) { toast('无法打开选择窗口', error.message, 'error') }
  }

  // ---------- runs ----------
  async function run() {
    if (state.running) return
    const input = unquote($('#sceneAnimationInput').value)
    const outputRoot = unquote($('#sceneAnimationOutputRoot').value)
    if (!input) { toast('无法运行场景动画', '请选择场景动画输入 JSON', 'error'); return }
    const button = $('#sceneAnimationRunBtn')
    button.disabled = true
    try {
      store(STORAGE.outputRoot, outputRoot)
      beginRun(await api.runSceneAnimation({ input, outputRoot }))
    } catch (error) {
      appendLog(error.message, 'error')
      toast('无法运行场景动画', error.message, 'error')
      button.disabled = state.running
    }
  }

  function beginRun(run) {
    if (!run?.runId) return
    const fresh = state.runId !== run.runId
    state.runId = run.runId
    if (fresh) {
      state.startedAt = run.startedAt || Date.now()
      state.progress = { first: run.first ?? null, last: run.last ?? null }
      appendLog(`${t('已启动场景动画')} · ${run.outputDir || ''}`, 'success')
    }
    if (Number.isInteger(run.frame)) Object.assign(state.progress, { frame: run.frame, count: run.count, time: run.time, percent: run.percent, first: run.first, last: run.last })
    setRunning(true)
    renderProgress('正在运行')
  }

  async function stop() {
    try {
      const result = await api.stopSceneAnimation()
      if (result.stopped) renderProgress('正在停止')
    } catch (error) { toast('无法停止场景动画', error.message, 'error') }
  }

  function finishRun(event) {
    if (event.runId && state.runId && event.runId !== state.runId) return
    if (!state.running && event.runId && event.runId === state.finishedRunId) return
    state.finishedRunId = event.runId
    const success = event.status === 'complete'
    state.progress = { ...(state.progress || {}), final: success ? 100 : state.progress?.percent ?? null, finishedAt: Date.now() }
    setRunning(false)
    renderProgress(success ? '运行完成' : event.status === 'stopped' ? '已停止' : '运行失败')
    if (event.error) appendLog(event.error, event.status === 'stopped' ? 'warning' : 'error')
    if (success) appendLog(`${t('运行完成')} · ${formatNumber((event.elapsed || 0) / 1000, 2)} s`, 'success')
    addLog(`${t('场景动画')}：${t(STATUS_LABELS[event.status] || event.status || '')}${event.outputDir ? ` · ${event.outputDir}` : ''}`, success ? 'success' : 'error')
    if (success) toast('场景动画已完成', event.outputDir || '')
    else if (event.status !== 'stopped') toast('场景动画未完成', event.error || '', 'error')
    if (event.outputExists && event.outputDir) loadResult(event.outputDir, { quiet: true })
  }

  function syncStatus(status) {
    if (!status) return
    renderEngine(status.engine)
    if (status.defaultOutputRoot) $('#sceneAnimationOutputRoot').placeholder = status.defaultOutputRoot
    if (status.running) {
      const reconnecting = !state.running || state.runId !== status.running.runId
      if (reconnecting && Array.isArray(status.log)) {
        $('#sceneAnimationLog').replaceChildren()
        for (const line of status.log) appendLog(line.text, line.level)
      }
      if (reconnecting && status.running.input) $('#sceneAnimationInput').value = status.running.input
      beginRun(status.running)
    } else if (state.running && status.last?.runId === state.runId) {
      // The closed event was missed while the event stream was down.
      finishRun({ ...status.last, outputExists: Boolean(status.last.outputDir) })
    }
  }

  // ---------- results ----------
  async function loadResult(path, { quiet = false } = {}) {
    try {
      const result = await api.sceneAnimationResult(path)
      store(STORAGE.result, result.outputDir)
      renderResult(result)
      return result
    } catch (error) {
      if (!quiet) toast('无法读取场景动画结果', error.message, 'error')
      return null
    }
  }

  // rows: [label, value, ignore]; ignore marks paths and engine-provided text that must not be translated.
  function definitionList(rows) {
    const list = document.createElement('dl')
    for (const [name, value, ignore] of rows) {
      const term = document.createElement('dt')
      term.textContent = name
      const detail = document.createElement('dd')
      detail.textContent = value ?? '—'
      if (ignore) detail.dataset.i18nIgnore = ''
      list.append(term, detail)
    }
    return list
  }

  function plainValue(value) {
    if (Array.isArray(value)) return value.map((item) => (item && typeof item === 'object' ? JSON.stringify(item) : String(item))).join(', ')
    if (value && typeof value === 'object') return JSON.stringify(value)
    return typeof value === 'number' ? formatNumber(value, 6) : String(value)
  }

  function renderImpact(summary) {
    const box = $('#sceneAnimationImpact')
    box.replaceChildren()
    const heading = document.createElement('h4')
    heading.textContent = '撞击模型'
    box.append(heading)
    const model = summary?.impactModel
    $('#sceneAnimationImpactLabel').textContent = model ? `impactModel.kind = ${model.kind || '—'}${model.label ? ` · ${model.label}` : ''}` : ''
    if (!model) {
      const note = document.createElement('p')
      note.textContent = '运行清单未包含 impactModel（较早版本的引擎）'
      box.append(note)
    } else {
      const rows = [['类型', model.kind || '—', true], ['说明', model.label || '—', true], ['可作预测', model.predictive === true ? '是' : model.predictive === false ? '否' : '未声明']]
      // Other fields are shown as delivered, so new model kinds need no GUI change.
      for (const [key, value] of Object.entries(model)) {
        if (['kind', 'label', 'predictive', 'presentation'].includes(key) || value === null || value === '') continue
        rows.push([key, plainValue(value), true])
      }
      box.append(definitionList(rows))
      if (model.presentation && typeof model.presentation === 'object') {
        const title = document.createElement('h5')
        title.textContent = '呈现'
        box.append(title, definitionList(Object.entries(model.presentation).map(([key, value]) => [key, plainValue(value), true])))
      }
    }
    if (summary?.scope) {
      const scope = document.createElement('p')
      scope.className = 'scope'
      scope.dataset.i18nIgnore = ''
      scope.textContent = summary.scope
      box.append(scope)
    }
  }

  function renderWarnings(result) {
    const box = $('#sceneAnimationWarnings')
    const warnings = result.summary?.warnings || []
    const extra = (result.job?.engineWarnings || []).filter((item) => !warnings.includes(item))
    box.replaceChildren()
    box.classList.toggle('none', !warnings.length && !extra.length)
    const heading = document.createElement('h4')
    heading.innerHTML = `<span>警告</span> (${warnings.length})`
    box.append(heading)
    const list = (items) => {
      const element = document.createElement('ul')
      element.dataset.i18nIgnore = ''
      for (const item of items) { const row = document.createElement('li'); row.textContent = item; element.append(row) }
      return element
    }
    if (warnings.length) box.append(list(warnings))
    else {
      const empty = document.createElement('p')
      empty.textContent = '运行清单未列出警告。'
      box.append(empty)
    }
    if (extra.length) {
      const title = document.createElement('h5')
      title.textContent = '引擎日志中的警告'
      box.append(title, list(extra))
    }
  }

  function renderSummary(result) {
    const s = result.summary
    const job = result.job
    const rows = []
    if (s) {
      const status = s.status === 'running' && job?.status && job.status !== 'running' ? job.status : s.status
      rows.push(['状态', STATUS_LABELS[status] || status || '—'])
      rows.push(['场景后端', labelled(BACKEND_LABELS, s.backend)])
      if (s.motionBackend) rows.push(['运动后端', labelled(MOTION_LABELS, s.motionBackend)])
      rows.push(['分辨率', s.width && s.height ? `${s.width} × ${s.height}` : '—'])
      if (Number.isFinite(s.fps)) rows.push(['帧率', `${formatNumber(s.fps)} fps`])
      if (Number.isInteger(s.firstFrame)) rows.push(['帧范围', `${s.firstFrame}–${s.lastFrame ?? '—'}`])
      if (Number.isInteger(s.timelineFrameCount)) rows.push(['时间轴帧数', String(s.timelineFrameCount)])
      if (Number.isFinite(s.impactTime)) rows.push(['撞击时刻', `${formatNumber(s.impactTime)} s`])
      if (s.backend === 'meshSequence') {
        rows.push(['可见光 RGB', s.optical ? '已输出' : '未输出'])
        rows.push(['相机观测', s.cameraObservation?.enabled ? '已输出' : '未输出'])
      }
      rows.push(['物理验证', s.physicsValidated === true ? '是' : s.physicsValidated === false ? '否 · physicsValidated = false' : '未声明'])
      if (s.device) rows.push(['计算设备', s.device, true])
      if (Number.isFinite(s.wallSeconds)) rows.push(['引擎用时', `${formatNumber(s.wallSeconds, 2)} s`])
    } else rows.push(['状态', '运行清单缺失或无法识别'])
    if (job?.status && (!s || job.status !== s.status)) {
      rows.push(['运行记录', `${STATUS_LABELS[job.status] || job.status}${Number.isInteger(job.exitCode) ? ` · exit ${job.exitCode}` : ''}`])
      if (job.error) rows.push(['原因', job.error, true])
    }
    if (job?.input) rows.push(['输入文件', job.input, true])
    if (job?.engine) rows.push(['引擎文件', `${job.engine}${job.engineOverride ? ` (${ENGINE_OVERRIDE_VARIABLE})` : ''}`, true])
    const list = definitionList(rows)
    list.id = 'sceneAnimationSummary'
    $('#sceneAnimationSummary').replaceWith(list)
  }

  function renderFiles(result) {
    const list = $('#sceneAnimationFiles')
    list.replaceChildren()
    for (const entry of result.files || []) {
      const row = document.createElement('li')
      row.innerHTML = '<span></span><em></em>'
      row.querySelector('span').textContent = entry.type === 'directory' ? `${entry.name}/` : entry.name
      row.querySelector('em').textContent = entry.type === 'directory' ? `${entry.entries}` : formatBytes(entry.bytes || 0)
      list.append(row)
    }
  }

  function renderResult(result) {
    stopPlayback()
    state.result = result
    $('#sceneAnimationEmpty').hidden = true
    $('#sceneAnimationResultContent').hidden = false
    $('#sceneAnimationOutputDir').textContent = result.outputDir
    renderWarnings(result)
    renderSummary(result)
    renderImpact(result.summary)
    renderFiles(result)
    state.kinds = [...(result.frames || [])]
    if (result.views?.length) state.kinds.push({ kind: 'views', count: result.views.length, frames: result.views.map((_, index) => index + 1), names: result.views })
    // Primary camera kinds first (fixed order), then each additional camera (cameras/<id>/<kind>) in the same order.
    const rank = (kind) => { const match = /^cameras\/([^/]+)\/(.+)$/.exec(kind); return match ? [1, match[1], KIND_ORDER.indexOf(match[2])] : [0, '', KIND_ORDER.indexOf(kind)] }
    state.kinds.sort((a, b) => { const x = rank(a.kind), y = rank(b.kind); return x[0] - y[0] || x[1].localeCompare(y[1]) || x[2] - y[2] })
    const label = (kind) => { const match = /^cameras\/([^/]+)\/(.+)$/.exec(kind); return match ? `相机 ${match[1]} · ${KIND_LABELS[match[2]] || match[2]}` : KIND_LABELS[kind] || kind }
    const select = $('#sceneAnimationKind')
    select.innerHTML = state.kinds.map((item) => `<option value="${escapeHtml(item.kind)}">${escapeHtml(label(item.kind))}</option>`).join('')
    select.disabled = state.kinds.length < 2
    const preferred = state.kinds.find((item) => item.kind === state.kind) || state.kinds[0]
    $('#sceneAnimationViewer').hidden = !preferred
    $('#sceneAnimationNoFrames').hidden = Boolean(preferred)
    state.index = -1
    // A new result opens on its last frame, which usually shows the end state of the sequence.
    if (preferred) selectKind(preferred.kind, preferred.frames.length - 1)
  }

  // ---------- frame viewer ----------
  const currentKind = () => state.kinds.find((item) => item.kind === state.kind)

  function selectKind(kind, fallbackIndex = 0) {
    const previous = state.index >= 0 ? currentKind()?.frames[state.index] : undefined
    state.kind = kind
    $('#sceneAnimationKind').value = kind
    const frames = currentKind()?.frames || []
    let index = previous === undefined || kind === 'views' ? fallbackIndex : frames.indexOf(previous)
    if (index < 0) index = frames.findIndex((value) => value >= previous)
    if (index < 0) index = frames.length - 1
    const slider = $('#sceneAnimationSlider')
    slider.max = String(Math.max(0, frames.length - 1))
    slider.disabled = frames.length < 2
    $('#sceneAnimationPlayBtn').disabled = frames.length < 2
    showIndex(Math.max(0, Math.min(frames.length - 1, index)))
  }

  function showIndex(index) {
    const entry = currentKind()
    if (!entry || !state.result) return
    state.index = index
    $('#sceneAnimationSlider').value = String(index)
    const number = entry.frames[index]
    const image = $('#sceneAnimationFrame')
    const token = ++state.frameToken
    // Playback advances only after the current PNG has loaded (or failed), so slow disks never pile up requests.
    image.onload = () => {
      if (token !== state.frameToken) return
      // Small renders are enlarged with visible pixels rather than smoothed (object-fit: contain scale).
      const scale = image.naturalWidth && image.naturalHeight ? Math.min(image.clientWidth / image.naturalWidth, image.clientHeight / image.naturalHeight) : 0
      image.classList.toggle('upscaled', scale * (window.devicePixelRatio || 1) > 1.5)
      scheduleNext()
    }
    image.onerror = () => { if (token === state.frameToken) scheduleNext() }
    image.src = api.sceneAnimationFrameUrl(state.result.outputDir, entry.kind, number)
    const fps = state.result.summary?.fps
    const caption = $('#sceneAnimationFrameLabel')
    if (entry.kind === 'views') {
      caption.innerHTML = `<span>静态视图</span> ${index + 1} / ${entry.frames.length} · <code></code>`
      caption.querySelector('code').textContent = entry.names[index]
    } else {
      // impact-v1 slow motion: the video clock is (n-1)/fps, the physical time is that times presentationTimeScale.
      const scale = state.result.summary?.presentationTimeScale
      const timing = Number.isFinite(fps) && fps > 0
        ? (Number.isFinite(scale) && scale > 0 && scale !== 1
          ? ` · 物理 t = ${escapeHtml(formatNumber((number - 1) / fps * scale))} s（慢放 ×${escapeHtml(formatNumber(1 / scale))}）`
          : ` · t = ${escapeHtml(formatNumber((number - 1) / fps))} s`)
        : ''
      caption.innerHTML = `<span>帧</span> <b>${number}</b> / ${escapeHtml(entry.frames[entry.frames.length - 1])}${timing} · ${index + 1} / ${entry.frames.length}`
    }
  }

  function scheduleNext() {
    clearTimeout(state.timer)
    if (!state.playing) return
    const entry = currentKind()
    if (!entry || entry.frames.length < 2) { stopPlayback(); return }
    const fps = Number.isFinite(state.result?.summary?.fps) && state.result.summary.fps > 0 ? state.result.summary.fps : 8
    const delay = 1000 / (fps * Number($('#sceneAnimationSpeed').value || 1))
    state.timer = setTimeout(() => showIndex((state.index + 1) % entry.frames.length), delay)
  }

  function renderPlayButton() {
    $('#sceneAnimationPlayBtn').innerHTML = state.playing ? '<svg><use href="#i-stop"/></svg><span>暂停</span>' : '<svg><use href="#i-play"/></svg><span>播放</span>'
  }

  function stopPlayback() {
    state.playing = false
    clearTimeout(state.timer)
    renderPlayButton()
  }

  function togglePlayback() {
    if (state.playing) { stopPlayback(); return }
    const entry = currentKind()
    if (!entry || entry.frames.length < 2) return
    state.playing = true
    renderPlayButton()
    if (state.index >= entry.frames.length - 1) showIndex(0)
    else scheduleNext()
  }

  // ---------- open / close ----------
  async function open(options = {}) {
    // The interface entry is hidden while SCENE_ANIMATION_UI_ENABLED is false; refuse to open from any caller
    // (menu, example, shortcut) so nothing is dispatched through the UI.
    if (!SCENE_ANIMATION_UI_ENABLED) { toast('场景动画（研究功能）界面入口当前已隐藏'); return false }
    root.hidden = false
    state.open = true
    $('#closeSceneAnimationBtn').focus()
    if (options.input) $('#sceneAnimationInput').value = options.input
    else if (!$('#sceneAnimationInput').value) $('#sceneAnimationInput').value = stored(STORAGE.input)
    if (options.outputRoot !== undefined) $('#sceneAnimationOutputRoot').value = options.outputRoot
    else if (!$('#sceneAnimationOutputRoot').value) $('#sceneAnimationOutputRoot').value = stored(STORAGE.outputRoot)
    if (options.label) appendLog(`${t('已载入示例')}：${t(options.label)}`, 'success')
    try { syncStatus(await api.sceneAnimationStatus()) } catch (error) { renderEngine(null); appendLog(error.message, 'error') }
    if (!state.running && $('#sceneAnimationInput').value) await inspectInput({ quiet: !options.input })
    if (!state.result && stored(STORAGE.result)) await loadResult(stored(STORAGE.result), { quiet: true })
  }

  function close() {
    stopPlayback()
    root.hidden = true
    state.open = false
  }

  function handleEvent(event) {
    if (event.type === 'scene-animation-status') syncStatus(event)
    else if (event.type === 'scene-animation-started') {
      beginRun({ runId: event.runId, outputDir: event.outputDir, first: event.first, last: event.last, startedAt: Date.now() })
      addLog(`${t('场景动画')}：${t('已启动')} · PID ${event.pid ?? '—'} · ${event.outputDir}`, 'success')
    } else if (event.type === 'scene-animation-progress') {
      if (state.runId && event.runId !== state.runId && state.running) return
      if (!state.running || state.runId !== event.runId) beginRun({ runId: event.runId, startedAt: Date.now() })
      state.progress = { ...(state.progress || {}), ...event }
      renderProgress('正在运行')
    } else if (event.type === 'scene-animation-log') {
      if (!state.runId || event.runId === state.runId) appendLog(event.text, event.level)
    } else if (event.type === 'scene-animation-closed') finishRun(event)
  }

  $('#closeSceneAnimationBtn').addEventListener('click', close)
  root.addEventListener('click', (event) => { if (event.target === root) close() })
  window.addEventListener('keydown', (event) => { if (event.key === 'Escape' && state.open) close() })
  $('#sceneAnimationBrowseInputBtn').addEventListener('click', () => browse('input'))
  $('#sceneAnimationBrowseOutputBtn').addEventListener('click', () => browse('output'))
  $('#sceneAnimationOpenResultBtn').addEventListener('click', () => browse('result'))
  $('#sceneAnimationInput').addEventListener('change', () => inspectInput())
  $('#sceneAnimationInput').addEventListener('keydown', (event) => { if (event.key === 'Enter') { event.preventDefault(); inspectInput() } })
  $('#sceneAnimationRunBtn').addEventListener('click', run)
  $('#sceneAnimationStopBtn').addEventListener('click', stop)
  $('#sceneAnimationOpenFolderBtn').addEventListener('click', () => {
    if (state.result?.outputDir) api.openPath(state.result.outputDir).catch((error) => toast('无法打开输出文件夹', error.message, 'error'))
  })
  $('#sceneAnimationKind').addEventListener('change', (event) => selectKind(event.target.value))
  $('#sceneAnimationSlider').addEventListener('input', (event) => { stopPlayback(); showIndex(Number(event.target.value)) })
  $('#sceneAnimationPlayBtn').addEventListener('click', togglePlayback)
  $('#sceneAnimationSpeed').addEventListener('change', () => { if (state.playing) scheduleNext() })
  $('#sceneAnimationNotice').textContent = SCENE_ANIMATION_RESEARCH_NOTICE
  setRunning(false)
  renderProgress('待命')

  return { open, close, run, isOpen: () => state.open, handleEvent }
}
