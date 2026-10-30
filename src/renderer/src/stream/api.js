// HTTP client and event channel of the STREAM image workspace (/api/stream/*).
async function request(path, options) {
  // FIXED[低] 错误处理: fetch 没有超时/AbortSignal，后端卡住（如 pick 对话框或同步读取大 TIFF）时调用方会一直等待，界面无法取消；网络错误时抛出原始 TypeError “Failed to fetch”，不是中文提示；已修复：fetch 包 try/catch，网络失败转换为可读中文错误（调用方传入的 options.signal 原本就会透传给 fetch，可用于取消）；未加默认超时，避免引入新的超时数值常量
  let response
  try {
    response = await fetch(path, options)
  } catch (error) {
    throw new Error(`无法连接本地服务：${error?.message || path}`)
  }
  const text = await response.text()
  let result
  try { result = JSON.parse(text) } catch {
    throw new Error(`请求失败：${response.status}${response.ok ? '（响应不是 JSON）' : ''}`)
  }
  if (!response.ok) throw new Error(result.error || `请求失败：${response.status}`)
  return result
}

const post = (path, value = {}) => request(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(value) })

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
