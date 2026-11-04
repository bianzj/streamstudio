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
