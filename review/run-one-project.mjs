// ==========================================================================
// 文件：run-one-project.mjs
// 功能：单工程执行器。读取本地 project.json 判断是 STREAM 还是 HiStream 工程，经 HTTP API 提交
//   运行并等待结束，最后向 stdout 打印一行 JSON 结果（供外层批处理脚本解析）
// 用法：node review/run-one-project.mjs <baseUrl> <projectPath> [mode]
// 主要步骤：
//   1. 解析参数与工程类型（kind 为 stream-image-project 即 STREAM），确定 mode
//   2. api / watchSse / readManifest / waitManifest：请求、事件流错误行采集、清单轮询
//   3. STREAM 分支：project/open → /api/stream/run → 轮询 /api/stream/status 至空闲 → 读清单
//   4. HiStream 分支：/api/run → 轮询 run-manifest.json → 记录状态、产品数、节点数
//   5. finally：关闭事件流，输出带耗时与前 8 条诊断行的 JSON
// 与其他文件的关系：与 run-examples.mjs 共用同一套 API 与清单约定，本脚本一次只跑一个工程
// 约定：超时由环境变量 RUN_TIMEOUT_MS 控制，缺省 20 分钟；工程文件允许带 UTF-8 BOM
// ==========================================================================

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
// 模式：命令行显式给出优先；STREAM 工程固定记为 EB；HiStream 工程取 project.mode，
// 缺省 eVoxelEB（兼容未写 mode 的旧工程）。STREAM 分支实际不使用该值，仅用于结果记录。
const mode = modeOverride || (isStream ? 'EB' : (project.mode || 'eVoxelEB'))

const label = projectPath
const started = Date.now()
// events：事件流中筛出的疑似错误/警告行，结果里只带前 8 条。
const events = []

// 作用：调用服务端 JSON 接口；GET 不带请求体，其余发送 JSON（缺省 {}）。
// 输出：解析后的 JSON，非 JSON 时返回 { raw: 前 400 字符 }；状态码非 2xx 时抛错。
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

// 作用：订阅 SSE 事件流，只把包含 error/失败/警告/容差/不收敛 等关键字的文本
// （截断到 300 字符）推入 sink，用于结果中的诊断摘要。close() 终止读取并等待退出。
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

// 作用：每 1.5 s 读一次 outputDir/run-manifest.json，状态不再是 running/queued 时返回；
// 超过 TIMEOUT_MS 返回 null。
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

// 主流程：按工程类型分支提交运行，所有结果汇总到 result，异常记为 status=error。
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
    // 步骤：每 2 s 轮询 /api/stream/status，记下最近一次 running（含 outputDir），
    // running 为空且队列为空即视为任务结束（超时则直接跳出，此时 status 取自清单或 unknown）。
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
    // 超时：清单未在期限内进入终态时标记 timeout，并请求服务端停止当前运行。
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
