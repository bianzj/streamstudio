// ==========================================================================
// 文件：settings.mjs
// 功能：桌面（Electron）端渲染进程偏好/草稿的持久化存储，落盘为 userData 目录下的
//       renderer-settings.json（独立于本地回环服务端口，端口变化后设置不丢失）。
// 主要模块/步骤：
//   1. KEYS：允许保存的设置键白名单
//   2. createDesktopSettings(directory)：读取并校验已有文件（损坏则备份为 .corrupt），
//      返回 (operation, key, value) 形式的同步处理函数，支持 get / set / remove
//   3. 写入采用“先写 .tmp 再 rename”的原子替换，避免半写入文件
// 数据流：主进程 IPC（渲染进程经 sendSync 调用）→ 本函数 → renderer-settings.json
// 与其他文件关系：由 desktop 主进程入口创建并挂到 IPC 处理器（待核对具体调用点）。
// 约定：值只能是字符串或 null；null 作为“墓碑”，表示该键已被显式删除，
//       防止旧 origin 的 localStorage 值再次被迁移；单个值上限 5 MB（UTF-8 字节）。
// ==========================================================================

import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'

// 模块：设置键白名单。只有列出的键可读写，其他键一律拒绝（见下方 get/set/remove 的检查）。
const KEYS = new Set([
  'streamStudioLanguage', 'histreamStudioLanguage',
  'histreamExampleDirectory', 'histreamProjectDirectory', 'histreamExecutable',
  'streamStudioImage', 'streamStudioScene', 'histreamStreamScene', 'streamStudioImageProject'
])

// 模块：createDesktopSettings —— 创建设置存取函数。
// 输入：directory（设置文件所在目录，通常为 Electron userData）。
// 输出：闭包 (operation, key, value) => 结果；get 返回 { known, value }，set/remove 返回 {}。
// 副作用：set/remove 会同步创建目录并整体重写 renderer-settings.json。
// A dedicated file survives changes to the loopback server's port. It contains
// only renderer preferences/drafts, never arbitrary paths supplied over IPC.
export function createDesktopSettings(directory) {
  const path = join(directory, 'renderer-settings.json')
  // 步骤 1：读取已有设置文件；内容不是 JSON 对象则视为损坏，改名为 .corrupt 备份并以空设置继续。
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
  // 步骤 2：只保留白名单键且值为 string/null 的条目，丢弃文件里的其他内容（兼容旧版本或被手改的文件）。
  let values = Object.fromEntries(Object.entries(saved).filter(([key, value]) => KEYS.has(key) && (typeof value === 'string' || value === null)))

  // 步骤 3：返回同步的 get/set/remove 处理函数；先校验键和操作，再校验 set 的值类型与大小。
  return (operation, key, value) => {
    if (!KEYS.has(key)) throw new Error('不支持的桌面设置')
    if (operation === 'get') return { known: Object.hasOwn(values, key), value: values[key] ?? null }
    if (operation !== 'set' && operation !== 'remove') throw new Error('不支持的桌面设置操作')
    if (operation === 'set' && (typeof value !== 'string' || Buffer.byteLength(value, 'utf8') > 5 * 1024 * 1024))
      throw new Error('桌面设置内容无效或超过 5 MB')
    // A null tombstone prevents an old origin's value from being migrated again.
    // DEFERRED[低] 性能: 每次 set/remove 都在主进程同步序列化并重写整个文件（10 个键，每个最多 5MB，最大约 50MB），而渲染进程经 sendSync 调用，界面和主进程都会被阻塞；建议在内存中合并后做防抖异步写入，或按键拆分成多个文件。理由：异步防抖会改变 sendSync 的同步语义与 IPC 约定，属行为改动，本轮不做。
    // 步骤 4：remove 写入 null 而非删除键；以不可变方式生成新对象，写盘成功后才更新内存 values。
    const next = { ...values, [key]: operation === 'remove' ? null : value }
    mkdirSync(directory, { recursive: true })
    // 先写临时文件再 rename 覆盖，保证设置文件替换是原子的。
    writeFileSync(`${path}.tmp`, `${JSON.stringify(next, null, 2)}\n`, 'utf8')
    renameSync(`${path}.tmp`, path)
    values = next
    return {}
  }
}
