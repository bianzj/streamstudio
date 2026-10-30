// Desktop state lives outside the HTTP origin; web deployments keep using the
// browser's storage. Migrate legacy values only when this origin has them.
const desktop = window.streamStudioSettings

export const appStorage = {
  getItem(key) {
    if (!desktop) return window.localStorage.getItem(key)
    const stored = desktop.read(key)
    if (stored.known) return stored.value
    let previous = null
    try { previous = window.localStorage.getItem(key) } catch {}
    if (previous !== null) desktop.setItem(key, previous)
    return previous
  },
  setItem(key, value) {
    // FIXED[中] 错误处理: setItem 未捕获 QuotaExceededError / SecurityError 以及桌面 IPC 异常（preload 在失败时 throw），调用方（保存工程目录、切换语言）会直接中断；已修复：try/catch 包住两条写入路径，失败时 console.warn 告警并返回 false，成功返回 true，调用方不再中断
    try {
      if (desktop) desktop.setItem(key, value)
      else window.localStorage.setItem(key, value)
      return true
    } catch (error) {
      console.warn('[storage] 写入失败', key, error)
      return false
    }
  },
  removeItem(key) {
    if (desktop) desktop.removeItem(key)
    else window.localStorage.removeItem(key)
  }
}
