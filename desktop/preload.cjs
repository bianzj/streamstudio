const { contextBridge, ipcRenderer } = require('electron')

function request(operation, key, value) {
  const result = ipcRenderer.sendSync('stream-studio:settings', operation, key, value)
  if (!result?.ok) throw new Error(result?.error || '无法保存桌面设置')
  return result
}

contextBridge.exposeInMainWorld('streamStudioSettings', {
  read: (key) => request('get', String(key)),
  setItem: (key, value) => { request('set', String(key), String(value)) },
  removeItem: (key) => { request('remove', String(key)) }
})
