// ==========================================================================
// 文件：dev.mjs
// 功能：开发模式启动器。同时拉起 API 后端（server.mjs --api-only）与 Vite 开发服务器，
//       并让两个子进程同生共死，方便用 `node dev.mjs` 一条命令做前端热更新调试。
// 主要步骤：
//   1. 以本文件所在目录为项目根，子进程继承终端输出（stdio: 'inherit'）
//   2. 启动 bridge：node server.mjs --api-only（只提供 /api，不托管 gui 静态页面）
//   3. 启动 vite：node_modules/vite/bin/vite.js（前端源码 src/renderer，端口见 vite.config.js）
//   4. 统一的 stop()：收到 SIGINT/SIGTERM 或任一子进程退出/出错时，终止另一个子进程
// 与其他文件的关系：
//   - server.mjs：--api-only 时不提供页面，访问根路径返回提示去 5173 端口
//   - vite.config.js：监听 127.0.0.1:5173，并把 /api 代理到 http://127.0.0.1:4173，
//     因此后端须监听 4173（server.mjs 中 STREAM_STUDIO_PORT 的缺省值）
// 注意：stdio 继承 + windowsHide 使子进程不弹出额外控制台窗口（Windows）。
// ==========================================================================

import { spawn } from 'node:child_process'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

// 项目根目录：本文件所在目录；两个子进程都以它为 cwd，保证相对路径与生产模式一致。
const PROJECT_ROOT = dirname(fileURLToPath(import.meta.url))
const PROCESS_OPTIONS = { cwd: PROJECT_ROOT, stdio: 'inherit', windowsHide: true }
// 步骤 1：用当前 Node 可执行文件启动 API 后端（--api-only：仅 /api，页面由 Vite 提供）。
const bridge = spawn(process.execPath, [join(PROJECT_ROOT, 'server.mjs'), '--api-only'], PROCESS_OPTIONS)
// 步骤 2：直接用 Node 运行 vite.js（不经 npx/.cmd 包装），kill() 作用于 Vite 本进程。
const vite = spawn(process.execPath, [join(PROJECT_ROOT, 'node_modules', 'vite', 'bin', 'vite.js')], PROCESS_OPTIONS)

// 统一收尾：stopping 标志防止重复清理，也让后续 exit 回调知道这是主动停止而非异常退出。
let stopping = false
function stop() {
  if (stopping) return
  stopping = true
  bridge.kill()
  vite.kill()
}

// 步骤 3：Ctrl+C / 终止信号时同时关闭两个子进程。
process.on('SIGINT', stop)
process.on('SIGTERM', stop)
// 后端意外退出：没有 API 前端无法工作，故停止 Vite 并沿用后端的退出码（缺省 1）。
bridge.on('exit', (code) => {
  if (stopping) return
  console.error(`Stream Studio: API 后端已退出（code ${code ?? 'null'}），停止开发服务器`)
  stop()
  process.exit(code ?? 1)
})
// 后端启动失败（如 spawn 出错）：报错、清理并以 1 退出。
bridge.on('error', (error) => {
  console.error(`Stream Studio: API 后端启动失败：${error.message}`)
  stop()
  process.exit(1)
})
vite.on('error', (error) => {
  console.error(`Stream Studio: Vite 启动失败：${error.message}`)
  stop()
  process.exit(1)
})
// Vite 退出（用户关闭或崩溃）：一并关闭后端，退出码沿用 Vite 的（缺省 0）。
vite.on('exit', (code) => {
  bridge.kill()
  process.exit(code ?? 0)
})
