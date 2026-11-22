import { app, BrowserWindow, dialog, ipcMain, screen, shell, utilityProcess } from 'electron'
import { execFile, spawnSync } from 'node:child_process'
import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs'
import { basename, dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createDesktopSettings } from './settings.mjs'

const sourceRoot = dirname(dirname(fileURLToPath(import.meta.url)))
const smokeTest = process.argv.includes('--smoke-test')
let mainWindow = null
// 本地 HTTP 服务运行在独立的 utilityProcess 中（desktop/server-process.mjs）。
// 主进程只管窗口、对话框和 IPC；准备运行、出图、ENVI 转换、读结果都不再冻结窗口。
let serverProcess = null
let serverQuitting = false
let appOrigin = ''
const serverCrashTimes = []

app.setName('Stream Studio')

const appDataRoot = process.env.APPDATA
const userData = appDataRoot ? join(appDataRoot, 'Stream Studio') : ''
if (userData) {
  mkdirSync(userData, { recursive: true })
  app.setPath('userData', userData)
}

const MIGRATION_SKIPPED = new Set([
  'Cache', 'Code Cache', 'GPUCache', 'DawnCache', 'DawnGraphiteCache', 'DawnWebGPUCache',
  'GrShaderCache', 'ShaderCache', 'Crashpad', 'lockfile', 'SingletonLock', 'SingletonCookie', 'SingletonSocket'
])

// Preserve the Chromium profile across product renames (HiStream Studio 2.0). Copying into
// a staging folder first keeps a locked or partially readable old profile from leaving a broken one.
function migrateLegacyUserData() {
  if (!userData || existsSync(join(userData, 'Local State'))) return
  const legacyUserData = ['HiStream Studio']
    .map((name) => join(appDataRoot, name))
    .find((path) => existsSync(join(path, 'Local State')))
  if (!legacyUserData) return
  const staging = `${userData}.migrating`
  try {
    rmSync(staging, { recursive: true, force: true })
    cpSync(legacyUserData, staging, { recursive: true, filter: (source) => !MIGRATION_SKIPPED.has(basename(source)) })
    cpSync(staging, userData, { recursive: true, force: false })
  } catch (error) {
    console.error(`Stream Studio: 旧用户数据迁移失败：${error.message}`)
  } finally {
    rmSync(staging, { recursive: true, force: true })
  }
}

const hasSingleInstanceLock = app.requestSingleInstanceLock()

if (!hasSingleInstanceLock) {
  app.quit()
} else {
  migrateLegacyUserData()
  app.on('second-instance', () => {
    if (!mainWindow) return
    if (mainWindow.isMinimized()) mainWindow.restore()
    mainWindow.show()
    mainWindow.focus()
  })
  app.whenReady().then(createWindow).catch((error) => {
    const message = error?.stack || String(error)
    console.error(message)
    if (!smokeTest) dialog.showErrorBox('Stream Studio 启动失败', message)
    app.quit()
  })
  app.on('window-all-closed', () => app.quit())
  // 退出前先让服务进程停止引擎（histream / STREAM 子进程树）再结束自身；超时则强制结束整棵进程树。
  app.on('before-quit', (event) => {
    const record = serverProcess
    if (!record || record.exited) return
    event.preventDefault()
    if (serverQuitting) return
    serverQuitting = true
    stopServerProcess(record).finally(() => {
      serverProcess = null
      app.quit()
    })
  })
  // 兜底：主进程异常退出时也不留下服务进程与引擎。
  process.on('exit', () => {
    const record = serverProcess
    if (record && !record.exited && record.pid) killProcessTree(record.pid)
  })
}

function killProcessTree(pid) {
  if (!pid) return
  if (process.platform === 'win32') {
    try { spawnSync('taskkill.exe', ['/PID', String(pid), '/T', '/F'], { windowsHide: true, timeout: 10000 }) } catch {}
  } else {
    try { process.kill(pid, 'SIGKILL') } catch {}
  }
}

// 服务进程崩溃后，Windows 不保证它启动的引擎随之结束（实测 Electron 38 下通常会被一并结束，此处兜底）；
// 按父进程号和启动时间找出并结束这些孤儿进程树。
function killOrphanedChildren(record) {
  if (process.platform !== 'win32' || !record.pid) return Promise.resolve()
  const since = new Date(record.startedAt - 2000).toISOString()
  const script = `$since = [datetime]::Parse('${since}').ToUniversalTime(); ` +
    `Get-CimInstance Win32_Process -Filter 'ParentProcessId=${Number(record.pid)}' | ` +
    `Where-Object { $_.CreationDate.ToUniversalTime() -ge $since } | ` +
    `ForEach-Object { taskkill.exe /PID $_.ProcessId /T /F | Out-Null; $_.ProcessId }`
  return new Promise((resolve) => {
    execFile('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], { windowsHide: true, timeout: 20000, encoding: 'utf8' }, (error, stdout) => {
      const killed = String(stdout || '').trim().split(/\s+/).filter(Boolean)
      if (killed.length) console.error(`Stream Studio: 已结束本地服务遗留的子进程 ${killed.join(', ')}`)
      if (error) console.error(`Stream Studio: 清理本地服务子进程失败：${error.message}`)
      resolve()
    })
  })
}

function stopServerProcess(record, timeoutMs = 5000) {
  if (record.exited) return Promise.resolve()
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      console.error('Stream Studio: 本地服务未在时限内退出，强制结束进程树')
      killProcessTree(record.pid)
      resolve()
    }, timeoutMs)
    record.child.once('exit', () => { clearTimeout(timer); resolve() })
    try { record.child.postMessage({ type: 'shutdown' }) } catch { clearTimeout(timer); killProcessTree(record.pid); resolve() }
  })
}

function serverEnvironment(root, dataRoot) {
  const env = { ...process.env }
  // 开发环境可能带着 ELECTRON_RUN_AS_NODE；服务进程不需要它，也不应把它传给引擎。
  delete env.ELECTRON_RUN_AS_NODE
  const pathKey = Object.keys(env).find((key) => key.toUpperCase() === 'PATH') || 'PATH'
  env[pathKey] = `${join(root, 'engine')};${env[pathKey] || ''}`
  return {
    ...env,
    STREAM_STUDIO_RESOURCE_ROOT: root,
    STREAM_STUDIO_DATA_ROOT: dataRoot,
    STREAM_STUDIO_SERVER_AUTOSTART: '0',
    HISTREAM_ROOT: join(root, 'engine')
  }
}

async function chooseProjectFile(currentProjectFile) {
  if (!mainWindow || mainWindow.isDestroyed()) return null
  const { canceled, filePaths } = await dialog.showOpenDialog(mainWindow, {
    title: '打开 Stream Studio 工程',
    defaultPath: currentProjectFile || 'project.json',
    filters: [
      { name: 'Stream Studio 工程或 OpenUSD', extensions: ['json', 'usd', 'usda', 'usdc', 'usdz'] },
      { name: 'Stream Studio 工程 JSON', extensions: ['json'] },
      { name: 'OpenUSD', extensions: ['usd', 'usda', 'usdc', 'usdz'] }
    ],
    properties: ['openFile']
  })
  return canceled ? null : filePaths[0] || null
}

function startServerProcess(environment) {
  return new Promise((resolve, reject) => {
    const child = utilityProcess.fork(join(sourceRoot, 'desktop', 'server-process.mjs'), [], {
      env: environment, stdio: 'pipe', serviceName: 'Stream Studio Server'
    })
    const record = { child, pid: 0, url: '', ready: false, exited: false, exitCode: null, startedAt: Date.now(), stderrTail: '' }
    const forward = (stream, target, keepTail) => stream?.on('data', (chunk) => {
      try { target.write(chunk) } catch {}
      if (keepTail) record.stderrTail = (record.stderrTail + chunk.toString()).slice(-4000)
    })
    forward(child.stdout, process.stdout, false)
    forward(child.stderr, process.stderr, true)
    const timer = setTimeout(() => {
      reject(new Error('Stream Studio 本地服务进程启动超时'))
      killProcessTree(record.pid)
    }, 30000)
    child.once('spawn', () => { record.pid = child.pid })
    child.on('message', (message) => {
      if (message?.type === 'listening' && !record.ready) {
        clearTimeout(timer)
        record.ready = true
        record.url = message.url
        record.pid = message.pid || record.pid
        resolve(record)
      } else if (message?.type === 'startup-error' && !record.ready) {
        clearTimeout(timer)
        reject(new Error(message.message))
      } else if (message?.type === 'choose-project-file') {
        chooseProjectFile(message.currentProjectFile)
          .then((path) => ({ path, error: '' }), (error) => ({ path: null, error: error.message }))
          .then(({ path, error }) => {
            if (!record.exited) record.child.postMessage({ type: 'choose-project-file-result', id: message.id, path, error })
          })
      }
    })
    child.once('exit', (code) => {
      record.exited = true
      record.exitCode = code
      clearTimeout(timer)
      if (!record.ready) {
        const detail = record.stderrTail.trim()
        reject(new Error(`Stream Studio 本地服务进程退出（代码 ${code}）${detail ? `：\n${detail}` : ''}`))
        return
      }
      handleServerExit(record, environment).catch((error) => console.error(error))
    })
  })
}

// 服务进程意外退出：清理遗留引擎，1 分钟内最多自动重启 2 次并重新载入页面；再崩溃则报错退出。
async function handleServerExit(record, environment) {
  if (serverQuitting || record !== serverProcess) return
  console.error(`Stream Studio: 本地服务进程意外退出（代码 ${record.exitCode}）`)
  await killOrphanedChildren(record)
  const now = Date.now()
  while (serverCrashTimes.length && now - serverCrashTimes[0] > 60000) serverCrashTimes.shift()
  serverCrashTimes.push(now)
  const detail = record.stderrTail.trim().split(/\r?\n/).slice(-12).join('\n')
  if (serverCrashTimes.length > 2 || smokeTest) {
    if (!smokeTest) dialog.showErrorBox('Stream Studio 本地服务已停止', `本地服务进程反复意外退出（代码 ${record.exitCode}），程序将关闭。${detail ? `\n\n${detail}` : ''}`)
    serverProcess = null
    app.quit()
    return
  }
  try {
    const restarted = await startServerProcess(environment)
    if (serverQuitting) { await stopServerProcess(restarted); return }
    serverProcess = restarted
    await waitForServer(restarted.url)
    appOrigin = new URL(restarted.url).origin
    if (mainWindow && !mainWindow.isDestroyed()) {
      await mainWindow.loadURL(restarted.url)
      dialog.showMessageBox(mainWindow, {
        type: 'warning', title: 'Stream Studio',
        message: '本地服务进程意外退出，已自动重启。',
        detail: `退出代码 ${record.exitCode}。正在进行的模拟已中断，请检查结果目录后重新运行。${detail ? `\n\n${detail}` : ''}`
      }).catch(() => {})
    }
  } catch (error) {
    dialog.showErrorBox('Stream Studio 本地服务重启失败', error?.stack || String(error))
    serverProcess = null
    app.quit()
  }
}

function resourceRoot() {
  return app.isPackaged ? process.resourcesPath : sourceRoot
}

async function waitForServer(url, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs
  let lastError
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${url}/api/defaults`, { signal: AbortSignal.timeout(Math.max(0, deadline - Date.now())) })
      if (response.ok) return
    } catch (error) {
      lastError = error
    }
    await new Promise((resolve) => setTimeout(resolve, 150))
  }
  throw lastError || new Error('Stream Studio 本地服务启动超时')
}

async function createWindow() {
  const root = resourceRoot()
  const dataRoot = process.env.LOCALAPPDATA
    ? join(process.env.LOCALAPPDATA, 'StreamStudio')
    : app.getPath('userData')
  serverProcess = await startServerProcess(serverEnvironment(root, dataRoot))
  const url = serverProcess.url
  await waitForServer(url)
  const settings = createDesktopSettings(app.getPath('userData'))
  appOrigin = new URL(url).origin
  const sameOrigin = (target) => {
    try { return new URL(target).origin === appOrigin } catch { return false }
  }
  ipcMain.on('stream-studio:settings', (event, operation, key, value) => {
    try {
      if (!mainWindow || mainWindow.isDestroyed() || event.sender !== mainWindow.webContents ||
          event.senderFrame !== mainWindow.webContents.mainFrame || !sameOrigin(event.senderFrame?.url))
        throw new Error('桌面设置请求来源无效')
      event.returnValue = { ok: true, ...settings(operation, key, value) }
    } catch (error) {
      event.returnValue = { ok: false, error: error.message }
    }
  })

  const workArea = screen.getPrimaryDisplay().workArea
  // Use a conventional 16:9 startup window while keeping it inside the
  // usable display area. Users can still resize it after launch.
  const aspectRatio = 16 / 9
  let height = workArea.height
  let width = Math.floor(height * aspectRatio)
  if (width > workArea.width) {
    width = workArea.width
    height = Math.floor(width / aspectRatio)
  }

  const iconPath = app.isPackaged
    ? join(root, 'branding', 'stream-studio.ico')
    : join(sourceRoot, 'design', 'branding', 'stream-studio-logo.ico')
  mainWindow = new BrowserWindow({
    width,
    height,
    x: workArea.x + Math.round((workArea.width - width) / 2),
    y: workArea.y + Math.round((workArea.height - height) / 2),
    minWidth: Math.min(1100, workArea.width),
    minHeight: Math.min(720, workArea.height),
    icon: existsSync(iconPath) ? iconPath : undefined,
    show: false,
    backgroundColor: '#f3f6fa',
    autoHideMenuBar: true,
    webPreferences: {
      preload: join(sourceRoot, 'desktop', 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  })

  mainWindow.webContents.setWindowOpenHandler(({ url: target }) => {
    if (/^https?:/i.test(target)) shell.openExternal(target).catch((error) => console.error(`Stream Studio: 打开外部链接失败：${error.message}`))
    return { action: 'deny' }
  })
  mainWindow.webContents.on('will-navigate', (event, target) => {
    if (!sameOrigin(target)) {
      event.preventDefault()
      if (/^https?:/i.test(target)) shell.openExternal(target).catch((error) => console.error(`Stream Studio: 打开外部链接失败：${error.message}`))
    }
  })
  mainWindow.once('ready-to-show', () => {
    if (!smokeTest) mainWindow.show()
  })
  mainWindow.on('closed', () => { mainWindow = null })
  await mainWindow.loadURL(url)
  if (smokeTest) {
    console.log(`STREAM_STUDIO_SMOKE_TEST_OK ${url}`)
    app.quit()
  }
}
