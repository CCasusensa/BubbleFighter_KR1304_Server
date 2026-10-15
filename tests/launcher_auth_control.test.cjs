const { test } = require('node:test')
const assert = require('node:assert/strict')
const { PassThrough } = require('node:stream')
const { createOwnershipControl } = require('./launcher_auth_control.cjs')
test('spawn handoff waits for parent acknowledgment before returning', async () => {
  const pipe = new PassThrough()
  const events = []
  const control = createOwnershipControl(pipe, { publish: pid => events.push(pid), ackTimeoutMs: 100 })
  pipe.write('{"input":true}\n')
  assert.deepEqual(await control.input, { input: true })
  let returned = false
  const handoff = control.handoff(41, async () => assert.fail('Unexpected kill')).then(() => { returned = true })
  await new Promise(resolve => setImmediate(resolve))
  assert.deepEqual(events, [41])
  assert.equal(returned, false)
  pipe.write('{"command":"owned","pid":41}\n')
  await handoff
  assert.equal(returned, true)
  pipe.destroy()
})
test('cancellation before native spawn completion kills only its eventual returned PID', async () => {
  const pipe = new PassThrough()
  const killed = []
  const control = createOwnershipControl(pipe, { publish() {}, ackTimeoutMs: 50 })
  pipe.write('{}\n{"command":"cancel"}\n')
  await control.input
  await assert.rejects(control.handoff(42, async pid => killed.push(pid)))
  assert.deepEqual(killed, [42])
  pipe.destroy()
})
test('cancellation after publication rejects a pending acknowledgment and cleans the returned PID', async () => {
  const pipe = new PassThrough()
  const killed = []
  const control = createOwnershipControl(pipe, { publish() {}, ackTimeoutMs: 50 })
  pipe.write('{}\n')
  await control.input
  const handoff = control.handoff(44, async pid => killed.push(pid))
  pipe.write('{"command":"cancel"}\n')
  await assert.rejects(handoff)
  assert.deepEqual(killed, [44])
  pipe.destroy()
})
test('a PID returned after cancellation is published even if local cleanup fails', async () => {
  const pipe = new PassThrough()
  const published = []
  const control = createOwnershipControl(pipe, { publish: pid => published.push(pid), ackTimeoutMs: 20 })
  pipe.write('{}\n{"command":"cancel"}\n')
  await control.input
  await assert.rejects(control.handoff(45, async () => { throw new Error('Expected local kill failure') }))
  assert.deepEqual(published, [45])
  pipe.destroy()
})
test('closed pipe and missing acknowledgment fail and clean their own PID', async () => {
  for (const mode of ['close', 'timeout']) {
    const pipe = new PassThrough()
    const killed = []
    const control = createOwnershipControl(pipe, { publish() {}, ackTimeoutMs: 10 })
    pipe.write('{}\n')
    await control.input
    const handoff = control.handoff(43, async pid => killed.push(pid))
    if (mode === 'close') pipe.destroy()
    await assert.rejects(handoff)
    assert.deepEqual(killed, [43])
    pipe.destroy()
  }
})
