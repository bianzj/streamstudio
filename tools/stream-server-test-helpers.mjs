// ==========================================================================
// 文件：stream-server-test-helpers.mjs
// 功能：STREAM 服务端测试共用的夹具：不经 socket 直接驱动路由表（makeService），以及“假引擎”
//   （node.exe 副本冒充 stream.exe，运行引擎目录下的无扩展名脚本 run）。
// 使用者：test-stream-server-bugs-20261124.mjs、test-stream-server-review-20261124.mjs。
// ==========================================================================

import { copyFileSync, mkdirSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { createStreamService } from '../server/stream/index.mjs'

const identity = (path) => path

// 作用：创建服务实例，返回 { service, call, events }。
//   call(method, path, data) 把 data 作为请求体直接调用 service.handle，返回 json() 回调捕获的 { status, payload }；
//   events 为订阅 /api/stream/events（SSE）后收到的全部事件对象（按到达顺序）。
export async function makeService({ resourceRoot, modelRoot, documentsRoot }) {
  let next = {}, reply = null
  const service = createStreamService({ resourceRoot, modelRoot, normalizeHostPath: identity, windowsPath: identity, documentsRoot,
    json: (_response, status, payload) => { reply = { status, payload }; return true }, body: async () => next, readTiff: () => { throw new Error('unused') } })
  const call = async (method, path, data = {}) => {
    next = data; reply = null
    await service.handle({ method, on() {} }, {}, new URL(`http://localhost${path}`))
    return reply
  }
  const events = []
  await service.handle({ method: 'GET', on() {} }, {
    writeHead() {}, on() {},
    write(line) { for (const part of String(line).split('\n\n')) if (part.startsWith('data: ')) events.push(JSON.parse(part.slice(6))) }
  }, new URL('http://localhost/api/stream/events'))
  return { service, call, events }
}

// 作用：在 root/<name> 下建一个最小的假引擎资源目录（engine/stream.exe = node 副本、stream-defined/classes.json、
//   engine/run = script），返回资源根目录；引擎被调用时执行 `node run <配置文件>`。
export function fakeEngine(root, name, script) {
  const resource = join(root, name)
  mkdirSync(join(resource, 'engine', 'stream-defined'), { recursive: true })
  copyFileSync(process.execPath, join(resource, 'engine', 'stream.exe'))
  writeFileSync(join(resource, 'engine', 'stream-defined', 'classes.json'), '{}')
  writeFileSync(join(resource, 'engine', 'run'), script)
  return resource
}

// 作用：等待 predicate() 为真，最多 timeoutMs；超时抛错（附 label）。
export async function waitFor(predicate, label, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`等待超时：${label}`)
    await new Promise((done) => setTimeout(done, 50))
  }
}
