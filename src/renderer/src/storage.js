// ==========================================================================
// 文件：storage.js
// 功能：渲染进程的键值持久化封装 appStorage，桌面版与浏览器版共用同一接口
// 主要模块：
//   1. desktop 句柄：来自 desktop/preload.cjs 暴露的 window.streamStudioSettings
//   2. appStorage.getItem/setItem/removeItem：桌面版走主进程设置，网页版走 localStorage
// 数据流：调用方（如 web-api.js 的 chooseExecutable）→ appStorage → 主进程设置文件 / localStorage
// 注意：桌面版首次读到未记录的键时，会把 localStorage 中的旧值迁移到桌面设置；
//       setItem 失败只告警并返回 false，不抛异常
// ==========================================================================

// Desktop state lives outside the HTTP origin; web deployments keep using the
// browser's storage. Migrate legacy values only when this origin has them.
const desktop = window.streamStudioSettings

// 模块：appStorage（键值存取）。getItem 返回字符串或 null；setItem 返回是否写入成功。
export const appStorage = {
  getItem(key) {
    if (!desktop) return window.localStorage.getItem(key)
    const stored = desktop.read(key)
    if (stored.known) return stored.value
    // 步骤：桌面设置中没有该键时，尝试从旧版 localStorage 迁移（localStorage 不可用则忽略）
    let previous = null
    try { previous = window.localStorage.getItem(key) } catch {}
    if (previous !== null) desktop.setItem(key, previous)
    return previous
  },
  setItem(key, value) {
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
