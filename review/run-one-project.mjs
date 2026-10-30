// 单工程执行器：读取本地 project.json 判断类型，走对应 API 提交并等待结束
// 用法: node review/run-one-project.mjs <baseUrl> <projectPath> [mode]
import { readFileSync, existsSync } from 'node:fs'
import { join, dirname } from 'node:path'

const base = process.argv[2]
const projectPath = process.argv[3]
const modeOverride = process.argv[4] || ''
const TIMEOUT_MS = Number(process.env.RUN_TIMEOUT_MS || 20 * 60 * 1000)

const project = JSON.parse(readFileSync(projectPath, 'utf8').replace(/^\uFEFF/, ''))
const isStream = project.kind === 'stream-image-project'
const mode = modeOverride || (isStream ? 'EB' : (project.mode || 'eVoxelEB'))

const label = projectPath
const started = Date.now()
const events = []

async function api(method, path, payload) {
  const response = await fetch(base + path, {
    method,
    headers: { 'content-type': 'application/json' },
    body: method === 'GET' ? undefined : JSON.stringify(payload ?? {})
  })
  const text = await response.text()
  let data
  try { data = JSON.parse(text) } catch { data = { raw: text.slice(0, 400) } }
  if (!response.ok) throw new Error(`${method} ${path} -> ${response.status} ${data?.error || text.slice(0, 300)}`)
  return data
}

// 采集 SSE 里的错误行
async function watchSse(path, sink) {
  const controller = new AbortController()
  const run = (async () => {
    try {
      const response = await fetch(base + path, { signal: controller.signal })
      if (!response.ok || !response.body) return
      const reader = response.body.getReader()
      const decoder = new TextDecoder()
      let buffer = ''
      for (;;) {
        const { value, done } = await reader.read()
        if (done) break
        buffer += decoder.decode(value, { stream: true })
        let index
        while ((index = buffer.indexOf('\n\n')) >= 0) {
          const frame = buffer.slice(0, index)
          buffer = buffer.slice(index + 2)
          const data = frame.split('\n').filter((l) => l.startsWith('data:')).map((l) => l.slice(5).trim()).join('')
          if (!data) continue
          let event
          try { event = JSON.parse(data) } catch { continue }
          const text = String(event.text ?? event.error ?? '').trim()
          if (text && /error|Error|ERROR|失败|警告|Warning|容差|不收敛|failed/i.test(text)) sink.push(`<${event.type}> ${text.slice(0, 300)}`)
        }
      }
    } catch {}
  })()
  return { close: () => { controller.abort(); return run } }
}

function readManifest(path) {
  try { return JSON.parse(readFileSync(path, 'utf8')) } catch { return null }
}

async function waitManifest(outputDir) {
  const file = join(outputDir, 'run-manifest.json')
  const deadline = Date.now() + TIMEOUT_MS
  while (Date.now() < deadline) {
    if (existsSync(file)) {
      const manifest = readManifest(file)
      if (manifest?.status && !['running', 'queued'].includes(manifest.status)) return manifest
    }
    await new Promise((r) => setTimeout(r, 1500))
  }
  return null
}

const result = { project: label, kind: isStream ? 'stream' : 'histream', mode, status: 'unknown' }
let sse = null
try {
  if (isStream) {
    sse = await watchSse('/api/stream/events', events)
    // 服务端在 project/open 时才会把 scene 里的相对路径解析成绝对路径，直接提交原始 scene 会被拒绝
    const opened = await api('POST', '/api/stream/project/open', { path: projectPath })
    const scene = opened?.scene || project.scene
    if (!scene) throw new Error('STREAM 工程缺少 scene 段')
    const job = await api('POST', '/api/stream/run', { projectPath, scene })
    const deadline = Date.now() + TIMEOUT_MS
    let lastRunning = null
    while (Date.now() < deadline) {
      const status = await api('GET', '/api/stream/status')
      if (status.running) lastRunning = status.running
      if (!status.running && !(status.queue || []).length) break
      await new Promise((r) => setTimeout(r, 2000))
    }
    result.outputDir = lastRunning?.outputDir || null
    const manifest = result.outputDir ? await waitManifest(result.outputDir) : null
    result.status = manifest?.status || 'unknown'
    result.exitCode = manifest?.exitCode ?? null
    result.error = manifest?.error || null
    result.job = job.id
  } else {
    sse = await watchSse('/api/events', events)
    const runInfo = await api('POST', '/api/run', { inputPath: projectPath, mode })
    result.runId = runInfo.runId
    result.outputDir = runInfo.outputDir
    const manifest = await waitManifest(runInfo.outputDir)
    if (!manifest) { result.status = 'timeout'; try { await api('POST', '/api/stop', {}) } catch {} }
    else {
      result.status = manifest.status
      result.exitCode = manifest.exitCode ?? null
      result.error = manifest.error || null
      result.products = manifest.completed?.products?.length ?? null
      result.nodes = manifest.completed?.nodeCount ?? null
    }
  }
} catch (error) {
  result.status = 'error'
  result.error = error.message
} finally {
  if (sse) await sse.close()
  result.elapsedS = Number(((Date.now() - started) / 1000).toFixed(1))
  result.diagnosticLines = events.slice(0, 8)
  console.log(JSON.stringify(result))
}
