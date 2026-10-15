function createOwnershipControl(stream, { publish, ackTimeoutMs = 10000, onCancel = () => {} }) {
  let text = ''
  let received = false
  let cancelled = false
  let resolveInput, rejectInput
  const pending = new Map()
  const input = new Promise((resolve, reject) => { resolveInput = resolve; rejectInput = reject })
  function cancel() {
    if (cancelled) return
    cancelled = true
    if (!received) rejectInput(new Error('Controlled input closed.'))
    for (const reject of pending.values()) reject(new Error('Ownership handoff cancelled.'))
    onCancel()
  }
  stream.setEncoding('utf8')
  stream.on('data', chunk => {
    text += chunk
    if (text.length > 65536) { cancel(); return }
    while (text.includes('\n')) {
      const boundary = text.indexOf('\n')
      const line = text.slice(0, boundary); text = text.slice(boundary + 1)
      try {
        const message = JSON.parse(line)
        if (!received) { received = true; resolveInput(message) }
        else if (message.command === 'cancel') cancel()
        else if (message.command === 'owned' && Number.isInteger(message.pid)) pending.get(message.pid)?.(null)
      } catch { cancel() }
    }
  })
  stream.on('error', cancel)
  stream.on('close', cancel)
  async function handoff(pid, kill) {
    let announced = false
    const announce = () => {
      if (announced) return
      announced = true
      publish(pid)
    }
    try {
      if (cancelled) throw new Error('Ownership handoff cancelled.')
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => { pending.delete(pid); reject(new Error('Ownership acknowledgment timed out.')) }, ackTimeoutMs)
        pending.set(pid, error => {
          clearTimeout(timer); pending.delete(pid)
          error ? reject(error) : resolve()
        })
        announce()
      })
      if (cancelled) throw new Error('Ownership handoff cancelled.')
    } catch {
      // The parent keeps ownership of every published PID. A cancelled or
      // unanswered handoff must still publish the returned PID before the local
      // best-effort kill, otherwise a failed local kill would leak the process.
      announce()
      try { await kill(pid) } catch { /* Parent drain owns the cleanup attempt. */ }
      throw new Error('Ownership handoff failed.')
    }
  }
  return { input, handoff, get cancelled() { return cancelled } }
}
module.exports = { createOwnershipControl }
