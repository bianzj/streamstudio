import { appStorage } from './storage.js'

// 结果读取接口的二进制格式（见 server/result-cache.mjs）：
// 'SSR1' | uint32 头长度 | 头 JSON { value, buffers } | 8 字节对齐的 Float32/Float64 数据块。
// 头 JSON 里的 { $ssBuffer: i } 换成对应的类型化数组；其余字段与 JSON 响应完全相同。
const BINARY_RESULT_TYPE = 'application/vnd.stream-studio.result-v1'

function decodeBinaryResult(buffer) {
  const bytes = new Uint8Array(buffer)
  if (String.fromCharCode(...bytes.subarray(0, 4)) !== 'SSR1') throw new Error('二进制结果格式无效')
  const headerLength = new DataView(buffer).getUint32(4, true)
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(8, 8 + headerLength)))
  const dataStart = Math.ceil((8 + headerLength) / 8) * 8
  const arrays = header.buffers.map(({ type, offset, length }) => type === 'f64'
    ? new Float64Array(buffer, dataStart + offset, length)
    : new Float32Array(buffer, dataStart + offset, length))
  const revive = (value) => {
    if (Array.isArray(value)) return value.map(revive)
    if (value && typeof value === 'object') {
      if (Number.isInteger(value.$ssBuffer) && Object.keys(value).length === 1) return arrays[value.$ssBuffer]
      for (const key of Object.keys(value)) value[key] = revive(value[key])
    }
    return value
  }
  return revive(header.value)
}

async function request(path, options) {
  const response = await fetch(path, options)
  if (response.ok && (response.headers.get('Content-Type') || '').startsWith(BINARY_RESULT_TYPE)) {
    return decodeBinaryResult(await response.arrayBuffer())
  }
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
    let value
    try { value = window.prompt(message, appStorage.getItem('histreamExecutable') || '') }
    catch { throw new Error('当前窗口不支持输入对话框，请在浏览器模式（npm start 后访问 http://127.0.0.1:4173）中设置自定义引擎路径') }
    if (value === null || value === undefined) return null
    const path = String(value).trim()
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
  readResult: (path, band = 0) => request('/api/results/read', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Accept: `${BINARY_RESULT_TYPE}, application/json` },
    body: JSON.stringify({ path, band })
  }),
  compareResults: (firstPath, secondPath, band = 0, maxScatterPoints = 4000) => post('/api/results/change', { firstPath, secondPath, band, maxScatterPoints }),
  histreamProxyInfo: () => request('/api/histream-proxy/info'),
  runHistreamProxy: (data) => post('/api/histream-proxy/run', data),
  run: (data) => post('/api/run', data),
  preflight: (data) => post('/api/run/preflight', data),
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
