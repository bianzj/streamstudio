// Bind directly to a system-assigned loopback port: no probe/rebind race,
// and never reuse a web service owned by a different Stream Studio instance.
export async function startDesktopServer(server) {
  await new Promise((resolve, reject) => {
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
  return `http://127.0.0.1:${server.address().port}`
}
