// ==========================================================================
// 文件：result-cache.mjs
// 功能：服务端读取结果文件时用的内存缓存（按字节计费的 LRU），以及把大数值数组抽成
//   二进制块的结果编码/解码
// 主要模块：
//   1. 常量与缓存容量：BINARY_RESULT_TYPE、MAGIC、resultCacheLimitBytes（环境变量，MB）
//   2. fileStamp：文件“身份”（路径 + mtime + size + ctime），用作缓存键并使旧条目失效
//   3. ByteLru：按字节计费的 LRU（get/set/resize/delete/clear），支持“依赖文件身份变化即失效”
//   4. 二进制结果编码：numericArray / packArray / encodeBinaryResult
//   5. decodeBinaryResult：编码的逆过程（测试与 Node 端工具用）
// 二进制格式（SSR1，小端）：
//   [0..3] 魔数 'SSR1'；[4..7] uint32 头部 JSON 字节数 L；[8..8+L) 头部 JSON { value, buffers }；
//   数据区起点 = ceil((8+L)/8)*8；每个数组按 8 字节对齐，offset 相对数据区起点。
//   value 里被抽走的数组换成 { $ssBuffer: 序号 }，buffers[i] = { type: f32|f64, offset, length }。
// 与其他文件的关系：由 server.mjs 的结果接口使用（待核对具体调用点）；浏览器端解码在
//   src/renderer/src/web-api.js；不带 Accept 头的旧客户端仍拿到原 JSON
// 约定：非数值（null/undefined）在二进制块里记为 NaN；缓存键由调用方给出，
//   ByteLru 只负责容量淘汰与依赖失效
// ==========================================================================

// 结果读取缓存与二进制结果编码。
//
// - ByteLru：按字节计费的 LRU。键里带文件的 (路径, mtime, size)，运行结束或文件被覆盖后
//   mtime/size 改变，旧条目不会再命中；同一路径写入新身份时顺手清掉旧条目。
// - encodeBinaryResult：把结果对象中的大数值数组（几何坐标、面元/体元指标）抽成
//   Float32/Float64 连续块，其余字段仍是 JSON。前端按 Content-Type 自动解码；不带
//   Accept 头的旧客户端继续拿到原来的 JSON。
import { statSync } from 'node:fs'
import { resolve } from 'node:path'

// 二进制结果的 Content-Type，以及文件起始的 4 字节魔数。
export const BINARY_RESULT_TYPE = 'application/vnd.stream-studio.result-v1'
const MAGIC = 'SSR1'

// 作用：由环境变量 STREAM_STUDIO_RESULT_CACHE_MB（MB）得到缓存上限（字节）。
// 缺省 512 MB；非数值或 ≤ 0 时返回 0，表示关闭缓存（ByteLru 的 limit 为 0 时不存任何条目）。
export function resultCacheLimitBytes(env = process.env) {
  const raw = env.STREAM_STUDIO_RESULT_CACHE_MB
  const megabytes = raw === undefined || raw === '' ? 512 : Number(raw)
  return Number.isFinite(megabytes) && megabytes > 0 ? Math.floor(megabytes * 1024 * 1024) : 0
}

// 模块：文件身份（缓存键的组成部分）。
// 文件身份：解析后的绝对路径 + mtime + size。文件不存在时返回 null。
export function fileStamp(path) {
  const resolved = resolve(path)
  try {
    const info = statSync(resolved)
    if (!info.isFile()) return null
    // 2026-11-24: ctimeMs joins the key (same stat call, no extra I/O): it changes on every write and cannot be restored by
    // utimes / copy-preserving tools, so an in-place rewrite that keeps mtime and size still invalidates. A head/tail content
    // hash was evaluated and not added: it needs two synchronous reads per lookup and misses edits in the middle of the file.
    return { path: resolved, mtimeMs: info.mtimeMs, size: info.size, key: `${resolved}|${info.mtimeMs}|${info.size}|${info.ctimeMs}` }
  } catch { return null }
}

// 模块：按字节计费的 LRU 缓存。entries 是 Map（插入顺序即新旧顺序，最前面最旧）；
// 每个条目为 { value, bytes, dependencies }，dependencies 记录依赖文件的 小写路径 → 身份键；
// bytes 为当前总计费，hits/misses 为命中统计。
export class ByteLru {
  constructor(limitBytes) {
    this.limit = Math.max(0, Number(limitBytes) || 0)
    this.bytes = 0
    this.entries = new Map()
    this.hits = 0
    this.misses = 0
  }

  get(key) {
    const entry = this.entries.get(key)
    if (!entry) { this.misses += 1; return undefined }
    // Map 保持插入顺序：删除后重新插入即移到“最近使用”一端。
    this.entries.delete(key)
    this.entries.set(key, entry)
    this.hits += 1
    return entry.value
  }

  // stamps：条目依赖文件的 fileStamp；同一路径出现新的 mtime/size 时，依赖旧身份的条目一并删除。
  set(key, value, bytes, stamps = []) {
    const size = Math.max(0, Number(bytes) || 0)
    this.delete(key)
    const dependencies = new Map(stamps.filter(Boolean).map((stamp) => [stamp.path.toLowerCase(), stamp.key]))
    // 步骤：同一路径出现新身份（mtime/size/ctime 变化）时，依赖旧身份的其他条目一并作废。
    for (const [otherKey, other] of this.entries) {
      if ([...other.dependencies].some(([path, stamp]) => dependencies.has(path) && dependencies.get(path) !== stamp)) this.delete(otherKey)
    }
    // 缓存关闭或单个条目就超过上限时不存入（返回 false）；否则存入后从最旧端淘汰直至不超限。
    if (!this.limit || size > this.limit) return false
    this.entries.set(key, { value, bytes: size, dependencies })
    this.bytes += size
    while (this.bytes > this.limit && this.entries.size) this.delete(this.entries.keys().next().value)
    return true
  }

  // 条目内容增长（例如 TIFF 解码块缓存）后重新计费。
  resize(key, bytes) {
    const entry = this.entries.get(key)
    if (!entry) return
    this.bytes += Math.max(0, Number(bytes) || 0) - entry.bytes
    entry.bytes = Math.max(0, Number(bytes) || 0)
    if (entry.bytes > this.limit) { this.delete(key); return }
    while (this.bytes > this.limit && this.entries.size > 1) {
      const oldest = this.entries.keys().next().value
      if (oldest === key) break
      this.delete(oldest)
    }
  }

  delete(key) {
    const entry = this.entries.get(key)
    if (!entry) return false
    this.entries.delete(key)
    this.bytes -= entry.bytes
    return true
  }

  clear() {
    this.entries.clear()
    this.bytes = 0
  }
}

// 模块：二进制结果编码。以下 BINARY_KEYS 之外的字段始终保持 JSON；
// 短于 MIN_BINARY_LENGTH（256）的数组不值得打包，仍走 JSON。
// 允许转成二进制块的字段：面元/体元几何与逐面元/逐体元指标值。
const BINARY_KEYS = new Set(['vertexPositions', 'voxelPositions', 'values'])
const MIN_BINARY_LENGTH = 256

// 作用：判断 value 是否可整体打包：Float32/Float64 数组，或长度 ≥ 256 且元素只含
// 数字/null/undefined 的普通数组。
function numericArray(value) {
  if (value instanceof Float32Array || value instanceof Float64Array) return true
  if (!Array.isArray(value) || value.length < MIN_BINARY_LENGTH) return false
  for (let index = 0; index < value.length; index += 1) {
    const item = value[index]
    if (typeof item !== 'number' && item !== null && item !== undefined) return false
  }
  return true
}

// 全部值都能用 float32 精确表示时用 Float32，否则用 Float64，保证与 JSON 回传数值一致。
function packArray(value) {
  if (value instanceof Float32Array) return { type: 'f32', array: value }
  let exact32 = true
  const length = value.length
  for (let index = 0; index < length; index += 1) {
    const item = value[index]
    const number = typeof item === 'number' ? item : NaN
    if (Number.isFinite(number) && Math.fround(number) !== number) { exact32 = false; break }
  }
  const array = exact32 ? new Float32Array(length) : new Float64Array(length)
  for (let index = 0; index < length; index += 1) {
    const item = value[index]
    array[index] = typeof item === 'number' ? item : NaN
  }
  return { type: exact32 ? 'f32' : 'f64', array }
}

// 作用：把 result 中指定键下的大数值数组抽成连续二进制块，其余内容保留为 JSON。
// 输出：Buffer（格式见文件头）或 null。walk 同时会丢弃 undefined/函数字段，
// 并把其他类型化数组转成普通数组。BINARY_KEYS 下“数组的数组”则每个子数组各打一块。
// 没有可打包的大数组时返回 null，调用方继续用 JSON。
export function encodeBinaryResult(result) {
  const buffers = []
  let dataBytes = 0
  const take = (value) => {
    const { type, array } = packArray(value)
    const offset = dataBytes
    buffers.push({ type, offset, length: array.length, array })
    // 每块按 8 字节对齐，保证解码侧 Float64Array 的起始偏移合法。
    dataBytes += Math.ceil(array.byteLength / 8) * 8
    return { $ssBuffer: buffers.length - 1 }
  }
  const walk = (value, key) => {
    if (BINARY_KEYS.has(key) && numericArray(value)) return take(value)
    if (BINARY_KEYS.has(key) && Array.isArray(value) && value.length && value.every((item) => numericArray(item))) return value.map((item) => take(item))
    if (Array.isArray(value)) return value.map((item) => walk(item, ''))
    if (ArrayBuffer.isView(value)) return Array.from(value)
    if (value && typeof value === 'object') {
      const output = {}
      for (const [childKey, child] of Object.entries(value)) {
        if (child !== undefined && typeof child !== 'function') output[childKey] = walk(child, childKey)
      }
      return output
    }
    return value
  }
  const value = walk(result, '')
  if (!buffers.length) return null
  const header = Buffer.from(JSON.stringify({ value, buffers: buffers.map(({ type, offset, length }) => ({ type, offset, length })) }), 'utf8')
  // 拼装：8 字节前缀 + 头部 JSON，数据区起点对齐到 8 字节，再依次拷入各数组。
  const dataStart = Math.ceil((8 + header.length) / 8) * 8
  const output = Buffer.alloc(dataStart + dataBytes)
  output.write(MAGIC, 0, 'ascii')
  output.writeUInt32LE(header.length, 4)
  header.copy(output, 8)
  for (const { offset, array } of buffers) {
    Buffer.from(array.buffer, array.byteOffset, array.byteLength).copy(output, dataStart + offset)
  }
  return output
}

// 模块：二进制结果解码（encodeBinaryResult 的逆过程）。
// 测试与 Node 端工具使用；浏览器端解码见 src/renderer/src/web-api.js。
export function decodeBinaryResult(buffer) {
  const bytes = buffer instanceof ArrayBuffer ? new Uint8Array(buffer) : new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength)
  if (String.fromCharCode(...bytes.subarray(0, 4)) !== MAGIC) throw new Error('二进制结果格式无效')
  const headerLength = new DataView(bytes.buffer, bytes.byteOffset, 8).getUint32(4, true)
  const header = JSON.parse(Buffer.from(bytes.subarray(8, 8 + headerLength)).toString('utf8'))
  const dataStart = Math.ceil((8 + headerLength) / 8) * 8
  // slice 复制出数据区，使起始偏移对齐，再按 type 建立 Float32/Float64 视图。
  const copy = bytes.slice(dataStart).buffer
  const arrays = header.buffers.map(({ type, offset, length }) => type === 'f64'
    ? new Float64Array(copy, offset, length) : new Float32Array(copy, offset, length))
  const revive = (value) => {
    if (Array.isArray(value)) return value.map(revive)
    if (value && typeof value === 'object') {
      if (Number.isInteger(value.$ssBuffer) && Object.keys(value).length === 1) return arrays[value.$ssBuffer]
      return Object.fromEntries(Object.entries(value).map(([key, child]) => [key, revive(child)]))
    }
    return value
  }
  return revive(header.value)
}
