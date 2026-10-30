// 内置示例端到端验证脚本（只通过 HTTP API 驱动，不直接启动引擎）
// 用法: node review/run-examples.mjs <baseUrl> [exampleId]
// 结果写入 review/examples-run/<timestamp>/

import { mkdirSync, writeFileSync, readdirSync, readFileSync, statSync, existsSync } from 'node:fs'
import { join, dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const base = process.argv[2] || 'http://127.0.0.1:4799'
const onlyId = process.argv[3] || ''
const stamp = new Date().toISOString().replace(/[:.]/g, '-')
const outRoot = join(here, 'examples-run', stamp)
const examplesRoot = join(outRoot, 'projects')
mkdirSync(examplesRoot, { recursive: true })

const RUN_TIMEOUT_MS = Number(process.env.RUN_TIMEOUT_MS || 25 * 60 * 1000)
const results = []

function log(id, message) {
  const line = `[${new Date().toISOString().slice(11, 19)}] ${message}`
  console.log(id ? `(${id}) ${line}` : line)
  if (id) appendLog(id, line)
}
function appendLog(id, line) {
  try { writeFileSync(join(outRoot, `${id}.log`), line + '\n', { flag: 'a' }) } catch {}
}

async function api(method, path, payload) {
  const response = await fetch(base + path, {
    method,
    headers: { 'content-type': 'application/json' },
    body: method === 'GET' ? undefined : JSON.stringify(payload ?? {})
  })
  const text = await response.text()
  let data
  try { data = JSON.parse(text) } catch { data = { raw: text } }
  if (!response.ok) throw new Error(`${method} ${path} -> ${response.status} ${data?.error || text.slice(0, 300)}`)
  return data
}

// ---- SSE 采集：把引擎 stdout/stderr 落到日志文件 ----
async function openSse(path, id) {
  const controller = new AbortController()
  const done = (async () => {
    try {
      const response = await fetch(base + path, { signal: controller.signal, headers: { accept: 'text/event-stream' } })
      if (!response.ok || !response.body) return
      const reader = response.body.getReader()
      const decoder = new TextDecoder()
      let buffer = ''
      for (;;) {
        const { value, done: streamDone } = await reader.read()
        if (streamDone) break
        buffer += decoder.decode(value, { stream: true })
        let index
        while ((index = buffer.indexOf('\n\n')) >= 0) {
          const frame = buffer.slice(0, index)
          buffer = buffer.slice(index + 2)
          const data = frame.split('\n').filter((line) => line.startsWith('data:')).map((line) => line.slice(5).trim()).join('')
          if (!data) continue
          let event
          try { event = JSON.parse(data) } catch { continue }
          const text = event.text ?? event.error ?? ''
          if (text) appendLog(id, `  <${event.type}> ${String(text).trimEnd()}`)
          else if (event.type) appendLog(id, `  <${event.type}>`)
        }
      }
    } catch { /* abort 时正常退出 */ }
  })()
  return { close: () => { controller.abort(); return done } }
}

// ---- 递归找最新的 run-manifest.json ----
function newestManifest(root, since) {
  let best = null
  const walk = (dir, depth) => {
    if (depth > 4) return
    let entries
    try { entries = readdirSync(dir, { withFileTypes: true }) } catch { return }
    for (const entry of entries) {
      if (entry.name === 'node_modules' || entry.name === '.histream-runtime') continue
      const full = join(dir, entry.name)
      if (entry.isDirectory()) { walk(full, depth + 1); continue }
      if (entry.name !== 'run-manifest.json') continue
      let stats
      try { stats = statSync(full) } catch { continue }
      if (stats.mtimeMs < since) continue
      if (!best || stats.mtimeMs > best.mtimeMs) best = { path: full, mtimeMs: stats.mtimeMs }
    }
  }
  walk(root, 0)
  return best
}

function readManifest(path) {
  try { return JSON.parse(readFileSync(path, 'utf8')) } catch { return null }
}

async function waitForManifestAt(outputDir, id) {
  const deadline = Date.now() + RUN_TIMEOUT_MS
  const manifestPath = join(outputDir, 'run-manifest.json')
  let lastStatus = ''
  while (Date.now() < deadline) {
    const manifest = existsSync(manifestPath) ? readManifest(manifestPath) : null
    const status = manifest?.status || ''
    if (status && status !== lastStatus) { lastStatus = status; log(id, `run-manifest 状态: ${status}`) }
    if (status && status !== 'running' && status !== 'queued') return { manifest, path: manifestPath }
    await new Promise((r) => setTimeout(r, 1500))
  }
  return { manifest: null, path: manifestPath, timeout: true }
}

async function runExample(entry) {
  const id = entry.id
  const startedAt = Date.now()
  const record = { id, label: entry.label, mode: entry.recommendedMode, steps: [] }
  const step = (name, ok, detail) => {
    record.steps.push({ name, ok, detail })
    log(id, `${ok ? 'OK  ' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`)
  }
  let sse = null
  try {
    log(id, `===== ${entry.label} (${entry.recommendedMode}) =====`)
    const opened = await api('POST', '/api/examples/open', { id, directory: examplesRoot })
    const projectPath = opened.path || opened.projectPath
    if (!projectPath) throw new Error('打开示例未返回工程路径')
    record.projectPath = projectPath
    step('打开示例', true, projectPath)

    const isStream = entry.recommendedMode === 'STREAM EB' || Boolean(opened.streamProject)
    const projectRoot = dirname(projectPath)

    if (isStream) {
      sse = await openSse('/api/stream/events', id)
      const scene = opened.streamProject?.scene || opened.project?.scene
      if (!scene) throw new Error('STREAM 示例缺少 scene 段')
      const job = await api('POST', '/api/stream/run', { projectPath, scene })
      step('提交任务', true, `job ${job.id} ${job.status || ''}`)
      // status() 返回 { running, queue }，运行期才知道 outputDir
      const deadline = Date.now() + RUN_TIMEOUT_MS
      let lastRunning = null
      let idle = false
      while (Date.now() < deadline) {
        const status = await api('GET', '/api/stream/status')
        if (status.running) lastRunning = status.running
        if (!status.running && !(status.queue || []).length) { idle = true; break }
        await new Promise((r) => setTimeout(r, 2000))
      }
      record.outputDir = lastRunning?.outputDir || null
      if (!idle) { record.status = 'timeout'; step('任务结束', false, '超时'); try { await api('POST', '/api/stream/stop', { id: job.id }) } catch {} }
      else if (record.outputDir && existsSync(join(record.outputDir, 'run-manifest.json'))) {
        const manifest = readManifest(join(record.outputDir, 'run-manifest.json'))
        record.status = manifest?.status || 'unknown'
        record.exitCode = manifest?.exitCode ?? null
        record.error = manifest?.error || null
        step('任务结束', record.status === 'done' || record.status === 'finished' || record.exitCode === 0,
          `status=${record.status} exitCode=${record.exitCode}${record.error ? ' error=' + record.error : ''}`)
      } else {
        record.status = 'unknown'
        step('任务结束', false, '未找到 run-manifest.json')
      }
      // 读 run-info
      if (record.outputDir) {
        try {
          const info = await api('POST', '/api/stream/run-info', { outputDir: record.outputDir })
          record.products = (info.products || []).map((p) => p.split(/[\\/]/).pop())
          record.summaryMode = info.summary?.mode
          step('读取结果', record.products.length > 0, `${record.products.length} 个产品`)
        } catch (error) { step('读取结果', false, error.message) }
      }
    } else {
      sse = await openSse('/api/events', id)
      const runInfo = await api('POST', '/api/run', { inputPath: projectPath, mode: entry.recommendedMode })
      step('提交运行', true, `${entry.recommendedMode} runId=${runInfo.runId}`)
      record.outputDir = runInfo.outputDir
      const { manifest, timeout } = await waitForManifestAt(runInfo.outputDir, id)
      if (timeout) { record.status = 'timeout'; step('运行结束', false, '超时'); try { await api('POST', '/api/stop', {}) } catch {} }
      else {
        record.status = manifest?.status || 'unknown'
        record.exitCode = manifest?.exitCode ?? null
        record.error = manifest?.error || null
        record.outputDir = manifest?.outputDir || (path ? dirname(path) : null)
        step('运行结束', record.status === 'finished', `status=${record.status} exitCode=${record.exitCode}${record.error ? ' error=' + record.error : ''}`)
      }
    }
  } catch (error) {
    record.status = 'error'
    record.error = error.message
    step('异常', false, error.message)
  } finally {
    if (sse) await sse.close()
    record.elapsedMs = Date.now() - startedAt
    results.push(record)
    log(id, `===== 完成: ${record.status} (${(record.elapsedMs / 1000).toFixed(1)}s) =====`)
  }
}

const catalog = await api('GET', '/api/examples')
const entries = (catalog.examples || []).filter((item) => !onlyId || item.id === onlyId)
log(null, `示例目录: ${catalog.examples?.length || 0} 个，本次执行 ${entries.length} 个 → ${outRoot}`)
for (const entry of entries) {
  await runExample(entry).catch((error) => log(entry.id, `未捕获异常: ${error.message}`))
}

const summaryPath = join(outRoot, 'summary.json')
writeFileSync(summaryPath, JSON.stringify({ base, startedAt: stamp, results }, null, 2), 'utf8')
console.log('\n=== 汇总 ===')
for (const item of results) {
  console.log(`${item.id}\t${item.status}\t${(item.elapsedMs / 1000).toFixed(1)}s\t${item.error || ''}`)
}
console.log(`\n详见 ${summaryPath}`)
