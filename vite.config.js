import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { defineConfig } from 'vite'

const PROJECT_ROOT = dirname(fileURLToPath(import.meta.url))
const GUI_SOURCE_DIR = join(PROJECT_ROOT, 'src', 'renderer')
const GUI_OUTPUT_DIR = join(PROJECT_ROOT, 'gui')

export default defineConfig({
  root: GUI_SOURCE_DIR,
  server: {
    host: '127.0.0.1',
    port: 5173,
    proxy: {
      '/api': 'http://127.0.0.1:4173'
    }
  },
  build: {
    outDir: GUI_OUTPUT_DIR,
    // DEFERRED[低] 可维护性: outDir 位于 root（src/renderer）之外且显式 emptyOutDir: true，每次构建会无提示清空项目根下的 gui 目录，若路径常量被改错或有人在 gui 中手放文件会被误删；说明：gui/ 是纯构建产物目录，请勿手工放置文件（改 emptyOutDir 或构建脚本清理属行为改动，本轮不改）
    emptyOutDir: true
  }
})
