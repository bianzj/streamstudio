// ==========================================================================
// 文件：run-examples.mjs
// 功能：内置示例的端到端验证脚本。只经 HTTP API 驱动已运行的 StreamStudio 服务，不直接启动引擎
// 用法：node review/run-examples.mjs <baseUrl> [exampleId]（baseUrl 缺省 http://127.0.0.1:4799）
// 主要步骤：
//   1. 读取参数并建立输出目录 .test/examples-run/<时间戳>/（日志与 summary.json，不入库）
//   2. 工具函数：log/appendLog（控制台与逐示例日志）、api（JSON 请求）、openSse（采集事件流）
//   3. 清单读取辅助：newestManifest、readManifest、waitForManifestAt
//   4. runExample：打开示例 → STREAM 示例走 /api/stream/*，其余走 /api/run，等待结束并记录
//   5. 主流程：GET /api/examples 取目录，逐个运行，写 summary.json 并在控制台打印汇总
// 数据流：服务端 /api/examples/open 在 examplesRoot 下生成工程 → 提交运行 → 轮询 run-manifest.json
//   或 /api/stream/status → 结果写入 results → summary.json
// 与其他文件的关系：依赖 server.mjs 及 server/ 提供的 /api/examples、/api/run、/api/stream/*
//   接口（接口细节待核对）；run-one-project.mjs 是单工程版本
// 约定：每个示例的超时由环境变量 RUN_TIMEOUT_MS 控制，缺省 25 分钟；时间戳取 UTC ISO 串
// ==========================================================================

// 内置示例端到端验证脚本（只通过 HTTP API 驱动，不直接启动引擎）
// 用法: node review/run-examples.mjs <baseUrl> [exampleId]
// 结果写入 .test/examples-run/<timestamp>/（项目忽略目录，原始日志不入库）

import { mkdirSync, writeFileSync, readdirSync, readFileSync, statSync, existsSync } from 'node:fs'
import { join, dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const base = process.argv[2] || 'http://127.0.0.1:4799'
const onlyId = process.argv[3] || ''
const stamp = new Date().toISOString().replace(/[:.]/g, '-')
const outRoot = join(here, '..', '.test', 'examples-run', stamp)
const examplesRoot = join(outRoot, 'projects')
mkdirSync(examplesRoot, { recursive: true })

// 单个示例的最长等待时间（毫秒）；results 收集每个示例的运行记录。
const RUN_TIMEOUT_MS = Number(process.env.RUN_TIMEOUT_MS || 25 * 60 * 1000)
const results = []

// 作用：打印带 UTC 时分秒的日志；有 id 时同时追加写入该示例的日志文件。
function log(id, message) {
  const line = `[${new Date().toISOString().slice(11, 19)}] ${message}`
  console.log(id ? `(${id}) ${line}` : line)
  if (id) appendLog(id, line)
}
function appendLog(id, line) {
  try { writeFileSync(join(outRoot, `${id}.log`), line + '\n', { flag: 'a' }) } catch {}
}

// 作用：调用服务端 JSON 接口。GET 不带请求体，其余方法发送 JSON（缺省 {}）。
// 输出：解析后的 JSON；响应不是 JSON 时返回 { raw: 文本 }；状态码非 2xx 时抛错（含 error 字段）。
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
// 作用：订阅 SSE 事件流，把事件文本（text 或 error 字段）逐行写入该示例的日志。
// 说明：帧以空行分隔、只取 data: 行；close() 通过 AbortController 终止并等待读取循环退出。
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
// 作用：在 root 下最多 4 层深度内，找修改时间不早于 since 的最新 run-manifest.json。
// 说明：跳过 node_modules 与 .histream-runtime；当前文件内未被调用（待核对是否遗留）。
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

// 作用：每 1.5 s 轮询 outputDir 下的 run-manifest.json，直到状态既不是 running 也不是
// queued 为止；超过 RUN_TIMEOUT_MS 则返回 timeout: true。
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

// 作用：运行一个示例并把各步骤（打开、提交、结束、读取结果）的成败写入 record。
// 分支：STREAM 示例（推荐模式为 STREAM EB 或打开结果含 streamProject）走 /api/stream/*，
// 靠 status 轮询判定空闲；其他示例走 /api/run，靠 run-manifest.json 判定结束。
// 说明：任何异常都记为 status=error 并继续下一个示例；sse 在 finally 中关闭。
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
      // 步骤：每 2 s 轮询一次；运行中记下 running（含 outputDir），running 为空且队列为空即空闲。
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
        const passed = record.exitCode === 0 || ['completed', 'finished', 'done'].includes(record.status)
        step('运行结束', passed, `status=${record.status} exitCode=${record.exitCode}${record.error ? ' error=' + record.error : ''}`)
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

// 主流程：取示例目录，可按 exampleId 过滤，顺序逐个运行（不并发），最后写汇总。
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
