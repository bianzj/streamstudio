// ==========================================================================
// 文件：vite.config.js
// 功能：Vite 前端构建/开发服务器配置（渲染进程页面 src/renderer）。
// 主要模块：
//   1. 路径常量：项目根、GUI 源码目录（src/renderer）、GUI 构建输出目录（gui/）
//   2. 开发服务器：127.0.0.1:5173，/api 代理到 server.mjs 的默认端口 4173
//   3. 构建：输出到项目根下的 gui/，构建前清空该目录
// 数据流：开发时浏览器 → Vite(5173) → /api 转发到 server.mjs(4173)；
//         发布时 gui/ 由 server.mjs 作为静态资源提供给浏览器或 Electron 窗口。
// 注意：gui/ 是纯构建产物目录，不要手工放文件（emptyOutDir 会清空）。
// ==========================================================================

import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { defineConfig } from 'vite'

// 路径常量：以本文件所在目录为项目根（ESM 中没有 __dirname，故由 import.meta.url 推得）。
const PROJECT_ROOT = dirname(fileURLToPath(import.meta.url))
const GUI_SOURCE_DIR = join(PROJECT_ROOT, 'src', 'renderer')
const GUI_OUTPUT_DIR = join(PROJECT_ROOT, 'gui')

// 配置对象：root 指向 src/renderer，因此 index.html 与前端源码均相对该目录解析。
export default defineConfig({
  root: GUI_SOURCE_DIR,
  server: {
    host: '127.0.0.1',
    port: 5173,
    // 开发代理：前端请求的 /api 转发给本地服务（端口与 server.mjs 默认 4173 一致），避免跨域。
    proxy: {
      '/api': 'http://127.0.0.1:4173'
    }
  },
  build: {
    // outDir 在 root 之外，所以必须显式给出绝对路径，并显式允许清空（见下方 emptyOutDir）。
    outDir: GUI_OUTPUT_DIR,
    // DEFERRED[低] 可维护性: outDir 位于 root（src/renderer）之外且显式 emptyOutDir: true，每次构建会无提示清空项目根下的 gui 目录，若路径常量被改错或有人在 gui 中手放文件会被误删；说明：gui/ 是纯构建产物目录，请勿手工放置文件（改 emptyOutDir 或构建脚本清理属行为改动，本轮不改）
    emptyOutDir: true
  }
})
