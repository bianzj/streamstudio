// ==========================================================================
// 文件：task-pool.mjs
// 功能：服务端后台线程（worker_threads）任务池，供重 CPU/大内存的同步工作使用；
//   本文件只含线程管理与消息协议，不关心具体任务内容（任务表由调用方传入）
// 主要模块：
//   1. 字节转移辅助：ownedBytes / transferList / prepareResult / receivedBytes
//   2. 错误构造与线程选择：taskError / crashError / keyIndex（key 的 FNV-1a 哈希）
//   3. createTaskPool：常驻线程池（懒创建、按 key 固定线程、空闲回收、启动失败回退主线程）
//   4. runTaskOnce：一次性线程，等线程完全退出后才返回，用于大内存任务
//   5. serveTasks / serveTaskOnce：线程端入口，按任务名分派并回传结果
// 消息协议：主线程 → 线程 { id, task, args }、{ type: 'clear' }；
//   线程 → 主线程 { type: 'ready' }、{ id, ok, result | error }（一次性线程不带 id）
// 与其他文件的关系：线程脚本与任务表在别处定义并通过 createWorker/serveTasks 接入（待核对具体文件）
// 约定：result.bytes（Uint8Array）以可转移的 ArrayBuffer 回传，转移后发送方不可再用；
//   线程数缺省 2，空闲 5 分钟后线程退出；连续启动失败 STARTUP_RETRIES（2）次后整体改为同步执行
// ==========================================================================

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

// 线程在 ready 之前就失败的次数达到该值后，不再尝试线程，永久退回主线程内联执行。
const STARTUP_RETRIES = 2

// 模块：字节转移辅助。转移 ArrayBuffer 要求整块独占，否则会连带转走无关数据。
// 返回独占底层 ArrayBuffer 的 Uint8Array（Buffer 池里的小块或切片会被复制），可安全转移。
export function ownedBytes(value) {
  const bytes = value instanceof Uint8Array ? value : new Uint8Array(0)
  if (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength && !(bytes.buffer instanceof SharedArrayBuffer)) return bytes
  return new Uint8Array(bytes)
}

// 作用：给 postMessage 生成 transfer 列表：仅当 value.bytes 独占整块非共享、非空的
// ArrayBuffer 时才转移，否则返回空列表（改为结构化复制）。
function transferList(value) {
  const bytes = value?.bytes
  return bytes instanceof Uint8Array && bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
    && bytes.byteLength > 0 && !(bytes.buffer instanceof SharedArrayBuffer) ? [bytes.buffer] : []
}

// 作用：线程端回传前把 result.bytes 规整成可转移的独占缓冲。
function prepareResult(result) {
  if (result && result.bytes instanceof Uint8Array) result.bytes = ownedBytes(result.bytes)
  return result
}

// 主线程收到的 bytes 是 Uint8Array；包成共享同一内存的 Buffer。
export function receivedBytes(value) {
  const bytes = value instanceof Uint8Array ? value : new Uint8Array(0)
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength)
}

// 错误构造：任务自身报错（线程内抛出）与线程异常退出（crashError）分别包装成明确的中文信息。
function taskError(message) {
  return new Error(String(message || '后台计算失败'))
}

function crashError(error) {
  const detail = error?.message || String(error || '')
  return new Error(`后台计算线程异常退出${detail ? `（${detail}）` : ''}，本次操作未完成，请重试`)
}

// 作用：把 key（不区分大小写）哈希到 [0, size) 的线程序号（32 位 FNV-1a），
// 使同一文件的任务固定落在同一线程，命中其内部缓存。
function keyIndex(key, size) {
  let hash = 2166136261
  const text = String(key).toLowerCase()
  for (let index = 0; index < text.length; index += 1) hash = Math.imul(hash ^ text.charCodeAt(index), 16777619)
  return (hash >>> 0) % size
}

// 模块：常驻线程池。
// 输入：createWorker()（创建线程）、runInline(task, args)（主线程内联执行的回退实现）、
// size 线程数（≤ 0 则始终内联）、idleMs 空闲退出时间（毫秒，≤ 0 不回收）、log。
// 说明：线程按需懒创建；slots[i] 为线程槽位对象，记录 ready/dead/retired 与待决任务表 pending。
// 对外：run(task, args, { key, transfer })、broadcast、inline、size、terminate。
export function createTaskPool({ createWorker, runInline, size = 2, idleMs = 5 * 60 * 1000, log = (message) => console.error(message) }) {
  const slots = Array.from({ length: Math.max(0, Math.floor(Number(size) || 0)) }, () => null)
  let nextId = 1
  let startupFailures = 0
  let inline = slots.length === 0

  // 主动回收线程（空闲超时或 terminate）；retired 标记使其后续 exit 事件不被当作崩溃。
  function retire(slot) {
    if (slot.dead) return
    slot.retired = true
    slot.dead = true
    if (slots[slot.index] === slot) slots[slot.index] = null
    clearTimeout(slot.idleTimer)
    // Never leave a caller's Promise pending forever when the pool is torn
    // down or an idle thread is reclaimed with queued work.
    const waiting = [...slot.pending.values()]
    slot.pending.clear()
    for (const pending of waiting) pending.reject(new Error('后台计算线程已回收，本次操作未完成，请重试'))
    void slot.worker.terminate().catch(() => {})
  }

  // 重置空闲计时：线程上没有待决任务时才计时，到期且仍空闲则回收；unref 以免阻止进程退出。
  function armIdle(slot) {
    clearTimeout(slot.idleTimer)
    slot.idleTimer = null
    if (slot.pending.size || slot.dead || !(idleMs > 0)) return
    slot.idleTimer = setTimeout(() => { if (!slot.pending.size) retire(slot) }, idleMs)
    slot.idleTimer.unref?.()
  }

  // 创建线程并挂接消息/错误/退出处理。收到 ready 后清零启动失败计数；
  // error/exit 统一走 fail：该线程所有待决任务一并失败，启动期失败的任务回退重试，
  // 运行期崩溃的任务以 crashError 失败，槽位清空，下次任务重新建线程。
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

  // 选线程：有 key 时按哈希固定；无 key 时选当前待决任务最少的槽位（空槽位负载记 0）。
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

  // 作用：把任务投递到选中的线程；线程创建同步失败时走 fallback。
  // 说明：options.transfer 为真时转移 args.bytes，调用方之后不能再使用该缓冲。
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

  // 作用：线程启动失败后的处理：累计失败不足 STARTUP_RETRIES 次则重新投递（会再建线程），
  // 达到后切换到内联模式并只打印一次日志，行为与引入线程前的旧版相同。
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

// 模块：一次性线程。返回值在线程 exit 之后才 resolve，确保堆和 ArrayBuffer 已释放。
// 三种结果：收到 ok 结果 → resolve；收到失败消息 → reject；未 ready 就退出 → 回退内联执行；
// ready 之后无结果就退出 → 视为崩溃。
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

// 模块：线程端入口。用 Object.hasOwn 查任务表，避免原型链上的名字被当作任务。
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
