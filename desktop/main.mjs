// ==========================================================================
// 文件：desktop/main.mjs
// 功能：Stream Studio 桌面版（Electron）主进程入口：管理窗口、本地服务进程和桌面设置 IPC。
// 主要模块/步骤：
//   1. 应用身份与用户数据目录（%APPDATA%/Stream Studio），旧 HiStream Studio 配置迁移
//   2. 单实例锁；第二实例只把已有窗口唤到前台
//   3. 进程树清理：killProcessTree / terminateServer / killOrphanedChildren / stopServerProcess
//   4. 本地服务进程：serverEnvironment 组装环境，startServerProcess 用 utilityProcess
//      启动 desktop/server-process.mjs 并处理其消息（listening / startup-error /
//      choose-project-file）
//   5. 崩溃恢复：handleServerExit（1 分钟内最多自动重启 2 次）
//   6. createWindow：启动服务 → 等待就绪 → 注册设置 IPC → 创建 16:9 窗口 → 载入页面
// 数据流：主进程 fork 服务进程 → 服务进程监听回环端口并回报 URL → 主进程等待
//         GET /api/defaults 成功 → BrowserWindow 载入该 URL；工程文件选择对话框
//         由服务进程请求、主进程弹出（需要窗口作为父窗口）。
// 与其他文件：desktop/server-process.mjs（子进程入口）、desktop/local-server.mjs（监听）、
//         desktop/settings.mjs（桌面设置存取）、desktop/preload.cjs（渲染进程预加载）、
//         server.mjs（HTTP 服务本体）。
// 约定：时间单位为毫秒；窗口与页面只允许访问同源（appOrigin）的 URL，其余 http(s) 链接
//         交给系统浏览器；命令行带 --smoke-test 时不显示窗口，载入成功即输出标记并退出。
// ==========================================================================

import { app, BrowserWindow, dialog, ipcMain, screen, shell, utilityProcess } from 'electron'
import { execFile, spawnSync } from 'node:child_process'
import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs'
import { basename, dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createDesktopSettings } from './settings.mjs'

// 全局状态：sourceRoot 为源码根目录（本文件位于 desktop/ 下，故取上两级）。
const sourceRoot = dirname(dirname(fileURLToPath(import.meta.url)))
const smokeTest = process.argv.includes('--smoke-test')
let mainWindow = null
// 本地 HTTP 服务运行在独立的 utilityProcess 中（desktop/server-process.mjs）。
// 主进程只管窗口、对话框和 IPC；准备运行、出图、ENVI 转换、读结果都不再冻结窗口。
let serverProcess = null
let serverQuitting = false
let appOrigin = ''
// serverCrashTimes：记录服务进程最近的崩溃时刻（毫秒时间戳），用于限制自动重启频率。
const serverCrashTimes = []

app.setName('Stream Studio')

// 用户数据目录固定为 %APPDATA%/Stream Studio；必须在 app ready 之前设置，
// 否则 Chromium 已按默认路径创建配置。无 APPDATA 时沿用 Electron 默认值。
const appDataRoot = process.env.APPDATA
const userData = appDataRoot ? join(appDataRoot, 'Stream Studio') : ''
if (userData) {
  mkdirSync(userData, { recursive: true })
  app.setPath('userData', userData)
}

// 迁移时跳过的条目：各类缓存可重建，Singleton* 与 lockfile 是旧实例的锁文件，复制会导致误判占用。
const MIGRATION_SKIPPED = new Set([
  'Cache', 'Code Cache', 'GPUCache', 'DawnCache', 'DawnGraphiteCache', 'DawnWebGPUCache',
  'GrShaderCache', 'ShaderCache', 'Crashpad', 'lockfile', 'SingletonLock', 'SingletonCookie', 'SingletonSocket'
])

// 作用：产品改名后保留旧 HiStream Studio 的 Chromium 配置（localStorage、窗口状态等）。
// 条件：新目录里还没有 Local State 且旧目录里有，才迁移（只迁一次）。
// 副作用：创建并删除 `<userData>.migrating` 暂存目录；失败只记日志，不阻止启动。
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

// 单实例：拿不到锁说明已有实例在运行，直接退出；已有实例通过 second-instance 事件前置窗口。
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
  // 退出流程：第一次 before-quit 先阻止退出，等服务进程优雅停止后再 app.quit()；
  // serverQuitting 防止重复进入，第二次 quit 时 serverProcess 已置空而直接放行。
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
    if (record && !record.exited) terminateServer(record)
  })
}

// 作用：强制结束 pid 及其整棵子进程树（引擎 histream / STREAM 都是服务进程的子进程）。
// Windows 用 taskkill /T /F；其他平台只能 SIGKILL 该 pid 本身（不含子进程）。失败一律忽略。
function killProcessTree(pid) {
  if (!pid) return
  if (process.platform === 'win32') {
    try { spawnSync('taskkill.exe', ['/PID', String(pid), '/T', '/F'], { windowsHide: true, timeout: 10000 }) } catch {}
  } else {
    try { process.kill(pid, 'SIGKILL') } catch {}
  }
}

// 作用：强制结束一个服务进程记录（record）。
// 做法：已知 pid（spawn 事件已触发或已收到 listening）时按进程树结束；
// 若 spawn 事件还没触发、pid 仍为 0（killProcessTree 会直接返回），改用 utilityProcess 的 child.kill()
// 结束该进程本身，避免启动超时后留下无人管理的服务进程。此时进程尚未启动引擎，没有子进程树需要清理。
function terminateServer(record) {
  if (record.pid) killProcessTree(record.pid)
  else { try { record.child.kill() } catch {} }
}

// 作用：服务进程崩溃后清理它遗留的子进程树（仅 Windows）。
// 做法：用 PowerShell 查询父进程号等于崩溃进程的进程，并要求创建时间不早于
// startedAt - 2 s（只处理本次服务进程启动之后创建的子进程）；输出被杀的 PID 以便记录日志。
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

// 作用：请求服务进程优雅退出（发 shutdown 消息），超过 timeoutMs 仍未退出则强制结束进程树。
// 无论哪条路径都会 resolve，不会让应用卡在退出阶段。
function stopServerProcess(record, timeoutMs = 5000) {
  if (record.exited) return Promise.resolve()
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      console.error('Stream Studio: 本地服务未在时限内退出，强制结束进程树')
      terminateServer(record)
      resolve()
    }, timeoutMs)
    record.child.once('exit', () => { clearTimeout(timer); resolve() })
    try { record.child.postMessage({ type: 'shutdown' }) } catch { clearTimeout(timer); terminateServer(record); resolve() }
  })
}

// 作用：组装服务进程的环境变量。
// 输入：root = 资源根目录（打包后为 resourcesPath，开发时为源码根）；dataRoot = 用户数据根。
// 要点：把 <root>/engine 放到 PATH 最前，使 histream 等引擎及其 DLL 能被找到；
// STREAM_STUDIO_SERVER_AUTOSTART=0 让 server.mjs 不自行监听，由 server-process.mjs 控制。
function serverEnvironment(root, dataRoot) {
  const env = { ...process.env }
  // 开发环境可能带着 ELECTRON_RUN_AS_NODE；服务进程不需要它，也不应把它传给引擎。
  delete env.ELECTRON_RUN_AS_NODE
  // Windows 环境变量名大小写不敏感（可能是 Path），先找到实际的键名再拼接，避免出现两个 PATH。
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

// 作用：弹出“打开工程”对话框，供服务进程经 IPC 请求（对话框必须挂在主窗口上）。
// 输出：选中的文件路径；取消或窗口不存在时为 null。支持工程 JSON 与 OpenUSD 文件。
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

// 作用：用 utilityProcess 启动 desktop/server-process.mjs，等它回报监听地址。
// 输出：Promise<record>；record 记录子进程句柄、pid、url、就绪/退出状态和 stderr 末尾 4000 字符。
// 失败：30 s 内没有 listening、收到 startup-error、或就绪前退出，都会拒绝并附上 stderr 尾部。
// 就绪后若再退出则转入 handleServerExit 做崩溃恢复。
function startServerProcess(environment) {
  return new Promise((resolve, reject) => {
    const child = utilityProcess.fork(join(sourceRoot, 'desktop', 'server-process.mjs'), [], {
      env: environment, stdio: 'pipe', serviceName: 'Stream Studio Server'
    })
    const record = { child, pid: 0, url: '', ready: false, exited: false, exitCode: null, startedAt: Date.now(), stderrTail: '' }
    // 把子进程的 stdout/stderr 原样转发到主进程；stderr 另存尾部，用于出错时展示原因。
    const forward = (stream, target, keepTail) => stream?.on('data', (chunk) => {
      try { target.write(chunk) } catch {}
      if (keepTail) record.stderrTail = (record.stderrTail + chunk.toString()).slice(-4000)
    })
    forward(child.stdout, process.stdout, false)
    forward(child.stderr, process.stderr, true)
    const timer = setTimeout(() => {
      reject(new Error('Stream Studio 本地服务进程启动超时'))
      terminateServer(record)
    }, 30000)
    child.once('spawn', () => { record.pid = child.pid })
    // 消息协议见 desktop/server-process.mjs 文件头；就绪后以 listening 消息里的 pid 为准。
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

// 崩溃恢复流程：
// 步骤 1：确认不是正在退出且仍是当前记录；清理遗留引擎。
// 步骤 2：丢弃 60 s 之前的崩溃记录，再记入本次；超过 2 次（或 --smoke-test）则报错并退出应用。
// 步骤 3：重启服务进程、等待就绪，更新 appOrigin，让窗口重新载入并提示用户进行中的模拟已中断。
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

// 资源根目录：打包后在 resourcesPath（engine/、branding/、gui/ 等），开发时就是源码根。
function resourceRoot() {
  return app.isPackaged ? process.resourcesPath : sourceRoot
}

// 作用：轮询 GET <url>/api/defaults 直到返回成功，说明 HTTP 服务已能处理请求。
// 每 150 ms 重试一次；超时则抛出最后一次的错误（没有则抛通用超时错误）。
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

// 作用：应用 ready 后的主流程（见文件头第 6 点）。
// 步骤 1：确定资源根与数据根（%LOCALAPPDATA%/StreamStudio，缺省回退到 userData）。
// 步骤 2：启动本地服务进程并等待就绪；记录 appOrigin 作为同源判断依据。
// 步骤 3：注册桌面设置 IPC；步骤 4：计算窗口尺寸；步骤 5：创建窗口并限制导航；步骤 6：载入页面。
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
  // 桌面设置 IPC（同步调用，由 preload 暴露给页面）：只接受来自主窗口主框架、且同源的请求，
  // 防止被导航到别处的页面或子框架读写本机设置。结果通过 event.returnValue 返回。
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

  // 窗口尺寸：取主显示器可用区域内的最大 16:9 窗口并居中（先按高度算，宽度超出则按宽度算）。
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

  // 图标：打包版用 resources/branding 下的图标，开发版用 design/branding；文件不存在则用默认图标。
  const iconPath = app.isPackaged
    ? join(root, 'branding', 'stream-studio.ico')
    : join(sourceRoot, 'design', 'branding', 'stream-studio-logo.ico')
  // 窗口创建：show:false 等 ready-to-show 再显示以避免白屏闪烁；最小尺寸不超过工作区；
  // 页面隔离：开启 contextIsolation 与 sandbox，关闭 nodeIntegration，仅通过 preload 暴露受控接口。
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

  // 外链策略：window.open 与跨源导航都不在应用内打开，http(s) 链接交给系统默认浏览器。
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
  // 冒烟测试：页面载入成功即输出约定标记并退出，供自动检查使用。
  if (smokeTest) {
    console.log(`STREAM_STUDIO_SMOKE_TEST_OK ${url}`)
    app.quit()
  }
}
