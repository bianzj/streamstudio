import { appStorage } from './storage.js'

async function request(path, options) {
  const response = await fetch(path, options)
  const text = await response.text()
  let result
  try { result = JSON.parse(text) } catch {
    throw new Error(`请求失败：${response.status}${response.ok ? '（响应不是 JSON）' : ''}`)
  }
  if (!response.ok) throw new Error(result.error || `请求失败：${response.status}`)
  return result
}

function post(path, value = {}) {
  return request(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(value) })
}

function chooseFile(accept) {
  return new Promise((resolve) => {
    const input = document.createElement('input')
    input.type = 'file'
    input.accept = accept
    input.oncancel = () => resolve(null)
    // FIXED[低] 错误处理: file.text() 失败（文件被占用/过大）时 async 回调抛出的异常不会传给外层 Promise，chooseFile 永远不 resolve，调用方一直挂起；oncancel 在旧版 Chromium 中不触发，也会一直挂起；已修复：onchange 内 try/catch，读取失败 resolve(null) 并 console.warn，Promise 任意路径都会结算，不再挂起
    input.onchange = async () => {
      const file = input.files?.[0]
      if (!file) return resolve(null)
      try {
        resolve({ path: file.name, content: await file.text() })
      } catch (error) {
        console.warn('[web-api] 读取文件失败', file.name, error)
        resolve(null)
      }
    }
    input.click()
  })
}

export const webApi = {
  // DEFERRED[低] 可维护性: request/post 与 stream/api.js 中的实现完全重复，错误处理、超时等改进需要改两处；建议：抽成共享的 http.js —— 抽出共享模块属于跨文件重构/改名，本轮明确禁止，保留待确认
  defaults: () => request('/api/defaults'),
  devices: (executable, refresh = false) => request(`/api/devices?${new URLSearchParams({ executable: executable || '', refresh: refresh ? '1' : '0' })}`),
  openProject: (path, mode) => post('/api/project/open', { path, mode }),
  listExamples: () => request('/api/examples'),
  openExample: (id, directory) => post('/api/examples/open', { id, directory }),
  chooseExecutable: async (message) => {
    const value = window.prompt(message, appStorage.getItem('histreamExecutable') || '')
    if (value === null) return null
    const path = value.trim()
    if (path) appStorage.setItem('histreamExecutable', path)
    else appStorage.removeItem('histreamExecutable')
    return path
  },
  chooseXml: async (mode) => {
    const result = await post('/api/project/pick', { mode })
    if (!result?.path || result.cancelled) return null
    return result
  },
  createProject: (project) => post('/api/project/create', project),
  chooseObj: () => chooseFile('.obj,text/plain'),
  importObj: ({ name, content }) => post('/api/project/import-obj', { name, content }),
  generateTarpCover: ({ fileName, name }) => post('/api/project/tarp-cover', { fileName, name }),
  importMeteo: ({ name, content }) => post('/api/project/import-meteo', { name, content }),
  importDem: ({ name, contentBase64, projectPath }) => post('/api/project/import-dem', { name, contentBase64, projectPath }),
  inspectDem: ({ path, projectPath }) => post('/api/project/dem-info', { path, projectPath }),
  importSpectrum: ({ name, content }) => post('/api/project/import-spectrum', { name, content }),
  importTexture: ({ name, contentBase64 }) => post('/api/project/import-texture', { name, contentBase64 }),
  saveDistribution: (data) => post("/api/project/distribution", data),
  readText: (path) => post('/api/project/read', { path }),
  saveXml: ({ path, content, project }) => post('/api/project/save', { path, content, project }),
  saveProjectAs: (project) => post('/api/project/save-as', project),
  openPath: async (path) => { await post('/api/open', { path }); return '' },
  openHelpDocument: (document) => post('/api/help/open', { document }),
  listResults: (path) => post('/api/results/list', { path }),
  deleteResults: (path, kind = 'image') => post('/api/results/delete-all', { path, kind }),
  readResult: (path, band = 0) => post('/api/results/read', { path, band }),
  compareResults: (firstPath, secondPath, band = 0, maxScatterPoints = 4000) => post('/api/results/change', { firstPath, secondPath, band, maxScatterPoints }),
  histreamProxyInfo: () => request('/api/histream-proxy/info'),
  runHistreamProxy: (data) => post('/api/histream-proxy/run', data),
  run: (data) => post('/api/run', data),
  stop: () => post('/api/stop'),
  reset: () => post('/api/reset'),
  onSimulationEvent: (callback) => {
    const events = new EventSource('/api/events')
    events.onmessage = (event) => {
      let payload
      try { payload = JSON.parse(event.data) } catch { return }
      callback(payload)
    }
    return () => events.close()
  }
}
