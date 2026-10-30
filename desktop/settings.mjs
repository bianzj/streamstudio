import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'

const KEYS = new Set([
  'streamStudioLanguage', 'histreamStudioLanguage', 'streamsimLanguage',
  'histreamExampleDirectory', 'histreamProjectDirectory', 'histreamExecutable',
  'streamStudioImage', 'streamStudioScene', 'histreamStreamScene', 'streamStudioImageProject'
])

// A dedicated file survives changes to the loopback server's port. It contains
// only renderer preferences/drafts, never arbitrary paths supplied over IPC.
export function createDesktopSettings(directory) {
  const path = join(directory, 'renderer-settings.json')
  // FIXED[中] 错误处理: 设置文件损坏（手动编辑、磁盘错误、写入被截断）时 JSON.parse 或下方格式检查抛错，createWindow 失败导致应用每次启动都直接退出，用户无法自救；已修复：读取/解析/格式校验失败时把文件改名为 .corrupt 备份并以空设置继续启动
  let saved = {}
  if (existsSync(path)) {
    try {
      saved = JSON.parse(readFileSync(path, 'utf8'))
      if (!saved || typeof saved !== 'object' || Array.isArray(saved)) throw new Error('桌面设置文件格式错误')
    } catch (error) {
      saved = {}
      try {
        renameSync(path, `${path}.corrupt`)
        console.warn(`Stream Studio: 桌面设置文件损坏，已备份为 ${path}.corrupt：${error.message}`)
      } catch (renameError) {
        console.warn(`Stream Studio: 桌面设置文件损坏且备份失败（${renameError.message}），将以空设置继续：${error.message}`)
      }
    }
  }
  let values = Object.fromEntries(Object.entries(saved).filter(([key, value]) => KEYS.has(key) && (typeof value === 'string' || value === null)))

  return (operation, key, value) => {
    if (!KEYS.has(key)) throw new Error('不支持的桌面设置')
    if (operation === 'get') return { known: Object.hasOwn(values, key), value: values[key] ?? null }
    if (operation !== 'set' && operation !== 'remove') throw new Error('不支持的桌面设置操作')
    if (operation === 'set' && (typeof value !== 'string' || Buffer.byteLength(value, 'utf8') > 5 * 1024 * 1024))
      throw new Error('桌面设置内容无效或超过 5 MB')
    // A null tombstone prevents an old origin's value from being migrated again.
    // DEFERRED[低] 性能: 每次 set/remove 都在主进程同步序列化并重写整个文件（10 个键，每个最多 5MB，最大约 50MB），而渲染进程经 sendSync 调用，界面和主进程都会被阻塞；建议在内存中合并后做防抖异步写入，或按键拆分成多个文件。理由：异步防抖会改变 sendSync 的同步语义与 IPC 约定，属行为改动，本轮不做。
    const next = { ...values, [key]: operation === 'remove' ? null : value }
    mkdirSync(directory, { recursive: true })
    writeFileSync(`${path}.tmp`, `${JSON.stringify(next, null, 2)}\n`, 'utf8')
    renameSync(`${path}.tmp`, path)
    values = next
    return {}
  }
}
