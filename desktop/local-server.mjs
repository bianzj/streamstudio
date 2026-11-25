// ==========================================================================
// 文件：desktop/local-server.mjs
// 功能：桌面版本地 HTTP 服务的监听辅助函数 startDesktopServer。
// 主要步骤：
//   1. 在 127.0.0.1 上监听端口 0（由系统分配空闲端口）
//   2. 用 once('error') / once('listening') 把监听结果转成 Promise
//   3. 返回 http://127.0.0.1:<端口> 形式的服务地址
// 数据流：desktop/server-process.mjs 把 server.mjs 导出的 http.Server 传入本函数，
//         拿到 URL 后经 parentPort 通知主进程 desktop/main.mjs 载入页面。
// 注意：只绑定回环地址，不对外网开放；不使用固定端口，也不探测后重绑。
// ==========================================================================

// Bind directly to a system-assigned loopback port: no probe/rebind race,
// and never reuse a web service owned by a different Stream Studio instance.
// 作用：让 server 监听系统分配的回环端口，监听成功后返回基础 URL。
// 输入：尚未监听的 http.Server；输出：形如 http://127.0.0.1:50321 的字符串。
// 失败：监听出错（如权限/地址问题）时 Promise 以该 error 拒绝。
export async function startDesktopServer(server) {
  await new Promise((resolve, reject) => {
    // 两个一次性回调互相注销，保证无论成功还是失败都不残留另一个监听器。
    const onError = (error) => {
      server.off('listening', onListening)
      reject(error)
    }
    const onListening = () => {
      server.off('error', onError)
      resolve()
    }
    server.once('error', onError)
    server.once('listening', onListening)
    server.listen(0, '127.0.0.1')
  })
  // 端口为 0 时的实际端口只能在 listening 之后从 address() 读取。
  return `http://127.0.0.1:${server.address().port}`
}
