// 桌面版本地服务进程入口：由 desktop/main.mjs 通过 Electron utilityProcess.fork 启动。
// 运行准备、FacetRT/FacetEB 出图、ENVI 转换和结果读取都在这里执行，不再阻塞窗口所在的主进程。
// 与主进程的消息（process.parentPort）：
//   子 → 主 { type: 'listening', url } | { type: 'startup-error', message }
//           { type: 'choose-project-file', id, currentProjectFile }
//   主 → 子 { type: 'choose-project-file-result', id, path, error } | { type: 'shutdown' }
import { startDesktopServer } from './local-server.mjs'

const parentPort = process.parentPort
if (!parentPort) throw new Error('desktop/server-process.mjs 只能由 Electron utilityProcess 启动')

const post = (message) => { try { parentPort.postMessage(message) } catch {} }
const pendingChoices = new Map()
let nextChoice = 0
let stopEngines = () => {}
let server = null

function shutdown(exitCode = 0) {
  try { stopEngines() } catch (error) { console.error(`Stream Studio: 停止引擎失败：${error.message}`) }
  for (const { reject } of pendingChoices.values()) reject(new Error('本地服务正在退出'))
  pendingChoices.clear()
  if (!server) process.exit(exitCode)
  server.close(() => process.exit(exitCode))
  server.closeAllConnections()
  // SSE 等长连接之外的情况兜底，避免关闭回调迟迟不来。
  setTimeout(() => process.exit(exitCode), 1500).unref()
}

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
  const module = await import('../server.mjs')
  server = module.server
  stopEngines = module.stopEngines
  // 文件对话框必须由主进程弹出（需要窗口作为父窗口）。
  module.setProjectFileChooser((currentProjectFile) => new Promise((resolve, reject) => {
    const id = ++nextChoice
    pendingChoices.set(id, { resolve, reject })
    post({ type: 'choose-project-file', id, currentProjectFile: currentProjectFile || '' })
  }))
  const url = await startDesktopServer(server)
  post({ type: 'listening', url, pid: process.pid })
} catch (error) {
  const message = error?.stack || String(error)
  console.error(message)
  post({ type: 'startup-error', message })
  setTimeout(() => process.exit(1), 100)
}
