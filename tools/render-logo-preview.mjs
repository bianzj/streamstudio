// ==========================================================================
// 文件：render-logo-preview.mjs
// 功能：用 Electron 离屏窗口把 SVG 标志渲染为 1024×1024 透明背景 PNG（品牌图预览）
// 用法：electron tools/render-logo-preview.mjs [输入 svg] [输出 png]（待核对具体启动方式）
// 主要步骤：
//   1. 解析输入/输出路径（缺省为 design/branding/stream-studio-logo-concept.svg / .png）
//   2. 创建不显示、无边框、透明的离屏 BrowserWindow 并载入 SVG
//   3. capturePage 截图后写出 PNG，销毁窗口并退出应用
// ==========================================================================

import { app, BrowserWindow } from 'electron'
import { mkdir, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'

const input = resolve(process.argv[2] || 'design/branding/stream-studio-logo-concept.svg')
const output = resolve(process.argv[3] || 'design/branding/stream-studio-logo-concept.png')

await app.whenReady()
const window = new BrowserWindow({
  width: 1024,
  height: 1024,
  show: false,
  frame: false,
  transparent: true,
  webPreferences: { offscreen: true }
})
await window.loadFile(input)
const image = await window.webContents.capturePage()
await mkdir(dirname(output), { recursive: true })
await writeFile(output, image.toPNG())
window.destroy()
app.quit()
