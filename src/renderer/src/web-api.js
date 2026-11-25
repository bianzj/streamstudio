// ==========================================================================
// 文件：web-api.js
// 功能：主界面（非 STREAM 工作区）访问本地服务 /api/* 的统一客户端 webApi
// 主要模块：
//   1. 二进制结果解码 decodeBinaryResult（SSR1 格式，见 server/result-cache.mjs）
//   2. request/post：对共用 http.js 的薄封装（带二进制结果解码器）
//   3. chooseFile：浏览器文件选择对话框，读取文本内容
//   4. webApi：工程/示例/导入/保存/结果读取与对比/运行控制/仿真事件订阅
// 数据流：界面模块 → webApi → server.mjs 中的 HTTP 接口；仿真进度经 SSE（/api/events）推回
// 关系：自定义引擎路径通过 storage.js 的 appStorage 保存（键 histreamExecutable）
// ==========================================================================

import { appStorage } from './storage.js'
import { request as httpRequest, post as httpPost } from './http.js'

// 结果读取接口的二进制格式（见 server/result-cache.mjs）：
// 'SSR1' | uint32 头长度 | 头 JSON { value, buffers } | 8 字节对齐的 Float32/Float64 数据块。
// 头 JSON 里的 { $ssBuffer: i } 换成对应的类型化数组；其余字段与 JSON 响应完全相同。
const BINARY_RESULT_TYPE = 'application/vnd.stream-studio.result-v1'

// 作用：解码 SSR1 二进制响应（小端）：魔数、头长度、头 JSON、8 字节对齐的数据区。
// 输出：与 JSON 响应同构的对象，其中 { $ssBuffer: i } 被替换为对应类型化数组（零拷贝视图）。
function decodeBinaryResult(buffer) {
  const bytes = new Uint8Array(buffer)
  if (String.fromCharCode(...bytes.subarray(0, 4)) !== 'SSR1') throw new Error('二进制结果格式无效')
  const headerLength = new DataView(buffer).getUint32(4, true)
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(8, 8 + headerLength)))
  // 数据区起点向上对齐到 8 字节，Float64Array 要求偏移按 8 字节对齐
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

// 作用：发起请求；响应为二进制结果类型时走 decodeBinaryResult，否则按 JSON 解析。
// 失败时抛出带中文提示的 Error（服务端 error 字段优先），实现见 http.js。
const BINARY = { type: BINARY_RESULT_TYPE, decode: decodeBinaryResult }
const request = (path, options) => httpRequest(path, options, BINARY)
const post = (path, value = {}) => httpPost(path, value, BINARY)

// 作用：弹出浏览器文件选择框，返回 { path: 文件名, content: 文本 }；取消或读取失败返回 null。
// 注意：浏览器拿不到真实路径，path 只是文件名。
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

// 模块：webApi 接口表。多数为 POST + JSON；readResult 通过 Accept 头请求二进制格式。
export const webApi = {
  defaults: () => request('/api/defaults'),
  devices: (executable, refresh = false) => request(`/api/devices?${new URLSearchParams({ executable: executable || '', refresh: refresh ? '1' : '0' })}`),
  openProject: (path, mode) => post('/api/project/open', { path, mode }),
  listExamples: () => request('/api/examples'),
  openExample: (id, directory) => post('/api/examples/open', { id, directory }),
  // 说明：用 window.prompt 输入自定义引擎路径并存入 appStorage；留空则清除；取消返回 null。
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
  // 说明：订阅仿真事件流（SSE），忽略无法解析的消息；返回关闭连接的函数。
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
