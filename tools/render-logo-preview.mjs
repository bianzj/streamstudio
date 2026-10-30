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
