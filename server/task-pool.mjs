// 后台计算线程：把读大文件、解析面元 JSON、TIFF/ENVI 解码与编码、出图等同步重活移出
// HTTP 服务的事件循环，执行期间其他请求（状态、事件流、轻量接口）照常响应。
//
// - createTaskPool：常驻线程池（默认 2 个线程）。带 key 的任务按 key 固定到同一线程，
//   使线程内的原始数据/解码缓存（例如同一 TIFF 的多个波段）继续命中；空闲一段时间后退出释放内存。
// - runTaskOnce：一次性线程，任务完成并等线程完全退出（堆与 ArrayBuffer 已释放）后才返回，
//   用于运行结束后的出图、ENVI 转换这类大内存任务。
// - serveTasks / serveTaskOnce：线程端入口。任务结果里的 bytes（Uint8Array/Buffer）以
//   Transferable ArrayBuffer 传回，避免复制。
//
// 线程启动失败（例如打包环境无法加载线程脚本）时退回主线程同步执行（与旧版行为相同）并打印一次日志；
// 线程在任务中途崩溃时只让该任务以清晰的错误失败，服务端继续运行，下次任务重新创建线程。

const STARTUP_RETRIES = 2

// 返回独占底层 ArrayBuffer 的 Uint8Array（Buffer 池里的小块或切片会被复制），可安全转移。
export function ownedBytes(value) {
  const bytes = value instanceof Uint8Array ? value : new Uint8Array(0)
  if (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength && !(bytes.buffer instanceof SharedArrayBuffer)) return bytes
  return new Uint8Array(bytes)
}

function transferList(value) {
  const bytes = value?.bytes
  return bytes instanceof Uint8Array && bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
    && bytes.byteLength > 0 && !(bytes.buffer instanceof SharedArrayBuffer) ? [bytes.buffer] : []
}

function prepareResult(result) {
  if (result && result.bytes instanceof Uint8Array) result.bytes = ownedBytes(result.bytes)
  return result
}

// 主线程收到的 bytes 是 Uint8Array；包成共享同一内存的 Buffer。
export function receivedBytes(value) {
  const bytes = value instanceof Uint8Array ? value : new Uint8Array(0)
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength)
}

function taskError(message) {
  return new Error(String(message || '后台计算失败'))
}

function crashError(error) {
  const detail = error?.message || String(error || '')
  return new Error(`后台计算线程异常退出${detail ? `（${detail}）` : ''}，本次操作未完成，请重试`)
}

function keyIndex(key, size) {
  let hash = 2166136261
  const text = String(key).toLowerCase()
  for (let index = 0; index < text.length; index += 1) hash = Math.imul(hash ^ text.charCodeAt(index), 16777619)
  return (hash >>> 0) % size
}

export function createTaskPool({ createWorker, runInline, size = 2, idleMs = 5 * 60 * 1000, log = (message) => console.error(message) }) {
  const slots = Array.from({ length: Math.max(0, Math.floor(Number(size) || 0)) }, () => null)
  let nextId = 1
  let startupFailures = 0
  let inline = slots.length === 0

  function retire(slot) {
    if (slot.dead) return
    slot.retired = true
    slot.dead = true
    if (slots[slot.index] === slot) slots[slot.index] = null
    void slot.worker.terminate().catch(() => {})
  }

  function armIdle(slot) {
    clearTimeout(slot.idleTimer)
    slot.idleTimer = null
    if (slot.pending.size || slot.dead || !(idleMs > 0)) return
    slot.idleTimer = setTimeout(() => { if (!slot.pending.size) retire(slot) }, idleMs)
    slot.idleTimer.unref?.()
  }

  function spawn(index) {
    let worker
    try { worker = createWorker() } catch (error) {
      startupFailures += 1
      throw error
    }
    const slot = { index, worker, ready: false, dead: false, retired: false, pending: new Map(), idleTimer: null }
    slots[index] = slot
    worker.on('message', (message) => {
      if (message?.type === 'ready') { slot.ready = true; startupFailures = 0; return }
      const pending = slot.pending.get(message?.id)
      if (!pending) return
      slot.pending.delete(message.id)
      armIdle(slot)
      if (message.ok) pending.resolve(message.result)
      else pending.reject(taskError(message.error))
    })
    const fail = (error) => {
      if (slot.dead && slot.retired) return
      const startup = !slot.ready
      slot.dead = true
      clearTimeout(slot.idleTimer)
      if (slots[slot.index] === slot) slots[slot.index] = null
      if (startup) startupFailures += 1
      const waiting = [...slot.pending.values()]
      slot.pending.clear()
      for (const pending of waiting) pending.fail(error, startup)
    }
    worker.on('error', fail)
    worker.on('exit', (code) => fail(new Error(`退出码 ${code}`)))
    return slot
  }

  function pick(key) {
    if (key !== undefined && key !== null && key !== '') return keyIndex(key, slots.length)
    let best = -1, bestLoad = Infinity
    for (let index = 0; index < slots.length; index += 1) {
      const load = slots[index] ? slots[index].pending.size : 0
      if (load < bestLoad) { best = index; bestLoad = load }
    }
    return best
  }

  function runLocally(task, args) {
    return Promise.resolve().then(() => runInline(task, args))
  }

  function dispatch(task, args, options, resolveTask, rejectTask) {
    if (inline) { runLocally(task, args).then(resolveTask, rejectTask); return }
    const index = pick(options.key)
    let slot = slots[index]
    try { if (!slot || slot.dead) slot = spawn(index) } catch (error) {
      return fallback(error, task, args, options, resolveTask, rejectTask)
    }
    const id = nextId++
    clearTimeout(slot.idleTimer)
    slot.idleTimer = null
    slot.pending.set(id, {
      resolve: resolveTask,
      reject: rejectTask,
      fail: (error, startup) => startup
        ? fallback(error, task, args, options, resolveTask, rejectTask)
        : rejectTask(crashError(error))
    })
    const transfer = options.transfer ? transferList(args) : []
    try { slot.worker.postMessage({ id, task, args }, transfer) } catch (error) {
      slot.pending.delete(id)
      rejectTask(taskError(error?.message))
    }
  }

  function fallback(error, task, args, options, resolveTask, rejectTask) {
    if (startupFailures >= STARTUP_RETRIES) {
      if (!inline) log(`STREAM STUDIO: 后台计算线程无法启动（${error?.message || error}），改为在服务线程内同步执行`)
      inline = true
      runLocally(task, args).then(resolveTask, rejectTask)
      return
    }
    dispatch(task, args, options, resolveTask, rejectTask)
  }

  return {
    // options.key：同一 key 固定到同一线程；options.transfer：转移 args.bytes 的内存（调用方之后不能再用它）。
    run(task, args = {}, options = {}) {
      return new Promise((resolveTask, rejectTask) => dispatch(task, args, options, resolveTask, rejectTask))
    },
    broadcast(message) {
      for (const slot of slots) if (slot && !slot.dead) { try { slot.worker.postMessage(message) } catch {} }
    },
    get inline() { return inline },
    get size() { return slots.length },
    terminate() {
      for (const slot of slots) if (slot) retire(slot)
    }
  }
}

// 一次性线程：等线程退出后才返回结果。onWorker 收到线程对象（便于“停止”时终止它）。
export function runTaskOnce({ createWorker, runInline, task, args = {}, onWorker = () => {}, log = (message) => console.error(message) }) {
  return new Promise((resolveTask, rejectTask) => {
    let worker
    try { worker = createWorker({ task, args }) } catch (error) {
      log(`STREAM STUDIO: 后台计算线程无法启动（${error?.message || error}），改为在服务线程内同步执行`)
      Promise.resolve().then(() => runInline(task, args)).then(resolveTask, rejectTask)
      return
    }
    onWorker(worker)
    let ready = false, settled = false, outcome = null, failure = null
    worker.on('message', (message) => {
      if (message?.type === 'ready') { ready = true; return }
      outcome = message
    })
    worker.on('error', (error) => { failure = error })
    worker.on('exit', (code) => {
      onWorker(null)
      if (settled) return
      settled = true
      if (outcome?.ok) return resolveTask(outcome.result)
      if (outcome && !outcome.ok) return rejectTask(taskError(outcome.error))
      if (!ready) {
        log(`STREAM STUDIO: 后台计算线程无法启动（${failure?.message || `退出码 ${code}`}），改为在服务线程内同步执行`)
        Promise.resolve().then(() => runInline(task, args)).then(resolveTask, rejectTask)
        return
      }
      rejectTask(crashError(failure || new Error(`退出码 ${code}`)))
    })
  })
}

// 线程端：常驻服务。tasks[name](args) 可返回 Promise；onClear 处理“清空缓存”广播。
export function serveTasks(port, tasks, { onClear = () => {} } = {}) {
  port.on('message', async (message) => {
    if (message?.type === 'clear') { try { onClear() } catch {} return }
    const { id, task, args } = message || {}
    try {
      const handler = Object.hasOwn(tasks, task) ? tasks[task] : null
      if (!handler) throw new Error(`未知的后台任务：${task}`)
      const result = prepareResult(await handler(args || {}))
      port.postMessage({ id, ok: true, result }, transferList(result))
    } catch (error) {
      port.postMessage({ id, ok: false, error: error?.message || String(error) })
    }
  })
  port.postMessage({ type: 'ready' })
}

// 线程端：一次性任务，回传结果后关闭端口让线程退出。
export async function serveTaskOnce(port, tasks, { task, args } = {}) {
  port.postMessage({ type: 'ready' })
  try {
    const handler = Object.hasOwn(tasks, task) ? tasks[task] : null
    if (!handler) throw new Error(`未知的后台任务：${task}`)
    const result = prepareResult(await handler(args || {}))
    port.postMessage({ ok: true, result }, transferList(result))
  } catch (error) {
    port.postMessage({ ok: false, error: error?.message || String(error) })
  }
  port.close()
}
