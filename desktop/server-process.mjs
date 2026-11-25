// ==========================================================================
// 文件：desktop/server-process.mjs（补充总起说明；原有说明见下方注释）
// 功能：utilityProcess 子进程的入口脚本，把 server.mjs 的 HTTP 服务跑在独立进程里。
// 主要模块：
//   1. 与主进程通信：post 发消息、parentPort 收 shutdown / 文件选择结果
//   2. 文件选择代理：pendingChoices 以自增 id 关联“请求—结果”（对话框只能在主进程弹出）
//   3. shutdown：停止引擎 → 拒绝未完成的选择 → 关闭 HTTP 服务 → 退出（带 1.5 s 兜底）
//   4. 启动：动态 import ../server.mjs → 注册文件选择器 → startDesktopServer → 回报 URL
// 数据流：desktop/main.mjs fork 本脚本 → 本脚本回报 listening{url,pid} → 主进程载入页面。
// 与其他文件：依赖 desktop/local-server.mjs 监听；server.mjs 需导出 server、stopEngines、
//         setProjectFileChooser；环境变量由 main.mjs 的 serverEnvironment 设置
//         （其中 STREAM_STUDIO_SERVER_AUTOSTART=0，防止 server.mjs 自行监听固定端口）。
// ==========================================================================

// 桌面版本地服务进程入口：由 desktop/main.mjs 通过 Electron utilityProcess.fork 启动。
// 运行准备、FacetRT/FacetEB 出图、ENVI 转换和结果读取都在这里执行，不再阻塞窗口所在的主进程。
// 与主进程的消息（process.parentPort）：
//   子 → 主 { type: 'listening', url } | { type: 'startup-error', message }
//           { type: 'choose-project-file', id, currentProjectFile }
//   主 → 子 { type: 'choose-project-file-result', id, path, error } | { type: 'shutdown' }
import { startDesktopServer } from './local-server.mjs'

const parentPort = process.parentPort
if (!parentPort) throw new Error('desktop/server-process.mjs 只能由 Electron utilityProcess 启动')

// 状态：post 忽略父进程已断开时的异常；pendingChoices 保存等待主进程回复的文件选择请求。
const post = (message) => { try { parentPort.postMessage(message) } catch {} }
const pendingChoices = new Map()
let nextChoice = 0
let stopEngines = () => {}
let server = null

// 作用：有序退出。先停引擎（histream / STREAM 子进程），再让等待中的选择请求失败，
// 最后关闭 HTTP 服务并退出进程；任何一步卡住都由底部的 1.5 s 定时器强制退出。
function shutdown(exitCode = 0) {
  try { stopEngines() } catch (error) { console.error(`Stream Studio: 停止引擎失败：${error.message}`) }
  for (const { reject } of pendingChoices.values()) reject(new Error('本地服务正在退出'))
  pendingChoices.clear()
  if (!server) process.exit(exitCode)
  server.close(() => process.exit(exitCode))
  // 主动断开所有连接（含 SSE 长连接），否则 server.close 的回调会一直等不到。
  server.closeAllConnections()
  // SSE 等长连接之外的情况兜底，避免关闭回调迟迟不来。
  setTimeout(() => process.exit(exitCode), 1500).unref()
}

// 消息分发：shutdown 触发退出；choose-project-file-result 按 id 完成对应的 Promise。
parentPort.on('message', ({ data } = {}) => {
  if (data?.type === 'shutdown') return shutdown(0)
  if (data?.type === 'choose-project-file-result') {
    const pending = pendingChoices.get(data.id)
    if (!pending) return
    pendingChoices.delete(data.id)
    if (data.error) pending.reject(new Error(data.error))
    else pending.resolve(data.path || null)
  }
})

try {
  // 启动步骤：动态导入 server.mjs（加载本身不监听端口，监听由下面的 startDesktopServer 完成），
  // 再注册文件选择器并监听端口；任何失败都回报 startup-error 并以退出码 1 结束。
  const module = await import('../server.mjs')
  server = module.server
  stopEngines = module.stopEngines
  // 文件对话框必须由主进程弹出（需要窗口作为父窗口）。
  module.setProjectFileChooser((currentProjectFile) => new Promise((resolve, reject) => {
    const id = ++nextChoice
    pendingChoices.set(id, { resolve, reject })
    post({ type: 'choose-project-file', id, currentProjectFile: currentProjectFile || '' })
  }))
  // 监听成功后把 URL 和本进程 pid 发给主进程；pid 供主进程崩溃/超时时结束进程树。
  const url = await startDesktopServer(server)
  post({ type: 'listening', url, pid: process.pid })
} catch (error) {
  const message = error?.stack || String(error)
  console.error(message)
  post({ type: 'startup-error', message })
  setTimeout(() => process.exit(1), 100)
}
