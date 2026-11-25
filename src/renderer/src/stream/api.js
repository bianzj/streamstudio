// ==========================================================================
// 文件：stream/api.js
// 功能：STREAM 影像工作区的 HTTP 客户端与服务端事件通道（/api/stream/*）
// 主要模块：
//   1. request/post：来自 ../http.js，统一把网络错误、非 JSON 响应、HTTP 错误转成 Error
//   2. streamApi：运行/停止、文件选择与检查、栅格读取、工程、ERA5、气象强迫元数据等接口
//   3. streamApi.onEvent：订阅 /api/stream/events（SSE），返回取消订阅函数
// 数据流：stream/*.js 界面模块 → streamApi → 本地服务 server/stream/index.mjs
// 关系：request/post 与 web-api.js 共用 ../http.js（此前各有一份重复实现，已合并）
// ==========================================================================

import { request, post } from '../http.js'

// HTTP client and event channel of the STREAM image workspace (/api/stream/*).

// 模块：streamApi 接口表。除 status 与 forcingPresets 为 GET 外，其余均为 POST + JSON。
export const streamApi = {
  status: () => request('/api/stream/status'),
  run: (scene) => post('/api/stream/run', { scene }),
  stop: (id) => post('/api/stream/stop', { id }),
  pick: (kind = 'file') => post('/api/stream/pick', { kind }),
  inspect: (paths) => post('/api/stream/inspect', { paths }),
  raster: (path, band = 0) => post('/api/stream/raster', { path, band }),
  runs: (outputRoot) => post('/api/stream/runs', { outputRoot }),
  runInfo: (outputDir) => post('/api/stream/run-info', { outputDir }),
  classStats: (outputDir) => post('/api/stream/class-stats', { outputDir }),
  openProject: (path) => post('/api/stream/project/open', { path }),
  saveProject: (data) => post('/api/stream/project/save', data),
  era5Packs: (include = []) => post('/api/stream/era5/list', { include }),
  registerEra5: (data) => post('/api/stream/era5/register', data),
  removeEra5: (path) => post('/api/stream/era5/remove', { path }),
  // Weather-forcing metadata (docs/forcing-metadata.md)
  forcingPresets: () => request('/api/stream/forcing/presets'),
  forcingPreset: (name) => post('/api/stream/forcing/preset', { name }),
  checkForcing: (data) => post('/api/stream/forcing/check', data),
  registerForcing: (data) => post('/api/stream/forcing/register', data),
  saveForcingMetadata: (data) => post('/api/stream/forcing/metadata/save', data),
  readForcingMetadata: (data) => post('/api/stream/forcing/metadata/read', data),
  openPath: (path) => post('/api/open', { path }),
  onEvent(callback) {
    // DEFERRED[低] 错误处理: 没有 onerror/onopen 处理，断线重连对调用方完全不可见，也无法在重连后触发状态重新同步；建议：onopen 时回调一个 { type: 'reconnected' } 事件（或在 status 事件中同步），连续失败时提示用户 —— 重连后的状态再同步已由服务端补发的 status 事件在 workspace.js 的 handleEvent 中修复；再新增 reconnected 事件类型会引入调用方未处理的新契约，断线提示也属新增文案，本轮不动
    const events = new EventSource('/api/stream/events')
    events.onmessage = (event) => {
      let payload
      try { payload = JSON.parse(event.data) } catch { return }
      callback(payload)
    }
    return () => events.close()
  }
}
