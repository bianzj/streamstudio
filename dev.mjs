import { spawn } from 'node:child_process'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const PROJECT_ROOT = dirname(fileURLToPath(import.meta.url))
const PROCESS_OPTIONS = { cwd: PROJECT_ROOT, stdio: 'inherit', windowsHide: true }
const bridge = spawn(process.execPath, [join(PROJECT_ROOT, 'server.mjs'), '--api-only'], PROCESS_OPTIONS)
const vite = spawn(process.execPath, [join(PROJECT_ROOT, 'node_modules', 'vite', 'bin', 'vite.js')], PROCESS_OPTIONS)

let stopping = false
function stop() {
  if (stopping) return
  stopping = true
  bridge.kill()
  vite.kill()
}

process.on('SIGINT', stop)
process.on('SIGTERM', stop)
bridge.on('exit', (code) => {
  if (stopping) return
  console.error(`Stream Studio: API 后端已退出（code ${code ?? 'null'}），停止开发服务器`)
  stop()
  process.exit(code ?? 1)
})
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
vite.on('exit', (code) => {
  bridge.kill()
  process.exit(code ?? 0)
})
