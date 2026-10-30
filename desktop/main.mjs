import { app, BrowserWindow, dialog, ipcMain, screen, shell } from 'electron'
import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs'
import { basename, dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { startDesktopServer } from './local-server.mjs'
import { createDesktopSettings } from './settings.mjs'

const sourceRoot = dirname(dirname(fileURLToPath(import.meta.url)))
const smokeTest = process.argv.includes('--smoke-test')
let mainWindow = null

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

// Preserve the Chromium profile across product renames (StreamSim 1.x, HiStream Studio 2.0). Copying into
// a staging folder first keeps a locked or partially readable old profile from leaving a broken one.
function migrateLegacyUserData() {
  if (!userData || existsSync(join(userData, 'Local State'))) return
  const legacyUserData = ['HiStream Studio', 'StreamSim']
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
}

function resourceRoot() {
  return app.isPackaged ? process.resourcesPath : sourceRoot
}

// FIXED[低] 错误处理: 单次 fetch 没有超时，若连接建立后服务端迟迟不响应，循环无法回到 deadline 判断，启动会无限挂起；已修复：fetch 传入 signal: AbortSignal.timeout(剩余 deadline)，超时后立即回到 deadline 判断（不新增超时数值）
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
  process.env.STREAM_STUDIO_RESOURCE_ROOT = root
  process.env.STREAM_STUDIO_DATA_ROOT = dataRoot
  process.env.STREAM_STUDIO_SERVER_AUTOSTART = '0'
  process.env.HISTREAM_ROOT = join(root, 'engine')
  process.env.PATH = `${join(root, 'engine')};${process.env.PATH || ''}`

  const { server, setProjectFileChooser, stopEngines } = await import('../server.mjs')
  // Electron 退出不发 SIGINT/SIGTERM，服务端的退出钩子不会运行；Windows 上子进程也不会随父进程结束。
  app.once('before-quit', stopEngines)
  // Resolve the owner when a request arrives: the server starts before the
  // BrowserWindow exists, and the window can also close while it is serving.
  setProjectFileChooser(async (currentProjectFile) => {
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
  })
  const url = await startDesktopServer(server)
  await waitForServer(url)
  const settings = createDesktopSettings(app.getPath('userData'))
  const appOrigin = new URL(url).origin
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

  // FIXED[低] 安全: 页面中任何 http(s) 链接（包括结果或工程内容渲染出来的链接）都会不经确认直接交给系统浏览器打开，openExternal 返回的 Promise 也没有 catch；另外没有设置 session 的 setPermissionRequestHandler。已修复：两处 shell.openExternal 都补上 .catch() 并记录错误；白名单域名/确认弹窗与权限请求处理器属产品决策，DEFERRED
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
