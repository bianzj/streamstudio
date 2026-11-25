// ==========================================================================
// 文件：http.js
// 功能：前端访问本地服务的共用 HTTP 封装（web-api.js 与 stream/api.js 共用）
// 主要模块：
//   1. request：fetch + JSON 解析 + 错误转换；可选地把二进制结果响应交给调用方提供的解码函数
//   2. post：以 JSON 请求体发起 POST
// 说明：错误统一转成带中文提示的 Error（网络失败、响应不是 JSON、HTTP 错误；服务端 error 字段优先）。
//   此前 web-api.js 与 stream/api.js 各有一份几乎相同的实现，现合并（web-api 的网络失败提示因此也变为中文）。
// ==========================================================================

// 作用：发起请求并解析 JSON；失败时抛出带中文提示的 Error（服务端 error 字段优先）。
// 参数：binary 可选 { type, decode }：响应成功且 Content-Type 以 type 开头时，用 decode(ArrayBuffer) 解码返回，
//   否则按 JSON 解析（stream/api.js 不传，只有 web-api.js 的结果读取使用二进制格式）。
export async function request(path, options, binary) {
  let response
  try {
    response = await fetch(path, options)
  } catch (error) {
    throw new Error(`无法连接本地服务：${error?.message || path}`)
  }
  if (binary && response.ok && (response.headers.get('Content-Type') || '').startsWith(binary.type)) {
    return binary.decode(await response.arrayBuffer())
  }
  const text = await response.text()
  let result
  try { result = JSON.parse(text) } catch {
    throw new Error(`请求失败：${response.status}${response.ok ? '（响应不是 JSON）' : ''}`)
  }
  if (!response.ok) throw new Error(result.error || `请求失败：${response.status}`)
  return result
}

// 作用：以 JSON 请求体发起 POST，binary 同 request。
export function post(path, value = {}, binary) {
  return request(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(value) }, binary)
}
