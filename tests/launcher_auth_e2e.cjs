const path = require('node:path')
const fs = require('node:fs')
const Module = require('node:module')
const { pathToFileURL } = require('node:url')
const assert = require('node:assert/strict')
const { app } = require('electron')
let injector
let frida
let phase = 'input'
let completing = false
const owned = []
function diagnostic(stage, details = {}) {
  console.log(JSON.stringify({ event: 'e2e-diagnostic', stage, at: Date.now(), ...details }))
}
const control = require('./launcher_auth_control.cjs').createOwnershipControl(
  require('node:net').connect(process.argv[2]), {
    publish: pid => console.log(JSON.stringify({ event: 'e2e-owned-spawn', pid })),
    onCancel: () => { void fail() }
  })
async function cleanup() {
  await injector?.close()
  if (!frida || !owned.length) return
  const device = await frida.getLocalDevice()
  const deadline = Date.now() + 5000
  do {
    if (!(await device.enumerateProcesses({ pids: owned })).length) return
    await new Promise(resolve => setTimeout(resolve, 100))
  } while (Date.now() < deadline)
  throw new Error('Owned process cleanup failed')
}
async function fail() {
  if (completing) return
  completing = true
  console.error(JSON.stringify({ event: 'e2e-failed', phase }))
  diagnostic('failure-cleanup-begin')
  try { await cleanup() } catch { console.error('e2e-owned-cleanup-failed') }
  app.exit(1)
}
process.on('uncaughtException', fail)
process.on('unhandledRejection', fail)
app.whenReady().then(async () => {
  console.log(JSON.stringify({ event: 'e2e-phase', phase: 'input-ready' }))
  const input = await control.input
  const { launcherRoot, host, authPort, gamePort, clanPort, certificate, wrongCertificate, gameClient, username } = input
  console.log(JSON.stringify({ event: 'e2e-phase', phase: 'input-received' }))
  function load(entry) {
    const built = require(path.join(launcherRoot, 'node_modules/esbuild')).buildSync({
      entryPoints: [path.join(launcherRoot, entry)], bundle: true, platform: 'node', format: 'cjs',
      external: ['frida'], write: false
    })
    const compiled = new Module(path.join(launcherRoot, 'Artifacts/e2e-memory.cjs'), module)
    compiled.filename = compiled.id
    compiled.paths = Module._nodeModulePaths(launcherRoot)
    compiled._compile(built.outputFiles[0].text, compiled.filename)
    return compiled.exports
  }
  const { KrAuthClient } = load('src/main/kr-auth-client.ts')
  const { launchGame } = load('src/main/launcher.ts')
  const { createKrClientInjector } = load('src/main/kr-client-injector.ts')
  const client = new KrAuthClient(host, authPort, certificate)
  phase = 'wrong-certificate'
  await assert.rejects(new KrAuthClient(host, authPort, wrongCertificate).health(), error => error.code === 'certificate')
  phase = 'register-login'
  assert.deepEqual(await client.health(), { status: 'ok' })
  assert.deepEqual(await client.register(username, input.password), { status: 'ok' })
  await assert.rejects(client.register(username, input.password), error => error.code === 'duplicate')
  await assert.rejects(client.login(username, input.password + 'wrong'), error => error.code === 'invalid_credentials')
  const reply = await client.login(username, input.password)
  input.password = ''
  assert.equal(reply.username, username)
  assert.match(reply.ticket, /^BFS1\.[a-f0-9]{64}$/)
  assert.ok(reply.expiresAt > Math.floor(Date.now() / 1000))
  console.log(JSON.stringify({ event: 'e2e-auth-passed', checks: 6, ticketChars: reply.ticket.length }))
  frida = await import(pathToFileURL(path.join(launcherRoot, 'node_modules/frida/build/src/frida.js')).href)
  const config = { serverHost: host, serverPort: gamePort, clanPort, gameClientPath: gameClient, account: username, password: '' }
  const session = { token: reply.ticket, username, expires: reply.expiresAt, host, port: gamePort, clanPort }
  for (const attempt of ['first', 'reuse']) {
    phase = `launch-${attempt}`
    injector = createKrClientInjector({
      hookPath: path.join(launcherRoot, 'runtime/kr-passport.js'),
      loadFrida: async () => ({
        ...frida,
        spawn: async (...args) => {
          if (control.cancelled) throw new Error('Owned test cancelled.')
          diagnostic('spawn-start')
          const pid = await frida.spawn(...args)
          owned.push(pid)
          diagnostic('spawn-returned', { pid })
          await control.handoff(pid, pid => frida.kill(pid))
          diagnostic('ownership-acknowledged', { pid })
          return pid
        },
        attach: async pid => {
          const attached = await frida.attach(pid)
          diagnostic('attached', { pid })
          attached.detached.connect(reason => diagnostic('detached', { reason: ['process-terminated', 'application-requested', 'connection-terminated', 'device-lost', 'process-replaced'].includes(reason) ? reason : 'other' }))
          return {
            detached: attached.detached,
            detach: () => attached.detach(),
            createScript: async source => {
              const script = await attached.createScript(source)
              return { destroyed: script.destroyed, load: () => script.load(), unload: () => script.unload(),
                exports: {
                  initialize: async input => {
                    const result = await script.exports.initialize(input)
                    diagnostic('initialize-returned', { ready: result?.ready === true })
                    return result
                  },
                  waitForLogin: async () => {
                    const started = Date.now()
                    diagnostic('wait-start')
                    try {
                      const result = await script.exports.waitForLogin()
                      diagnostic('wait-returned', { elapsed: Date.now() - started,
                        packetId: Number.isInteger(result?.packetId) ? result.packetId : null, ticketMatches: result?.ticketMatches === true })
                      return result
                    } catch (error) { diagnostic('wait-rejected', { elapsed: Date.now() - started }); throw error }
                  }
                } }
            }
          }
        },
        resume: async pid => { await frida.resume(pid); diagnostic('resumed', { pid }) }
      })
    })
    const result = await launchGame(config, session, [], {
      manifestPath: path.join(launcherRoot, 'runtime/kr-client-manifest.json'),
      isRunning: async () => false,
      injectAndLaunch: async input => {
        try { return await injector.injectAndLaunch(input) }
        catch (error) {
          const stage = /^KR1304 injection failed during (spawn|attach|createScript|load|initialize|resume|waitForLogin)\.$/.exec(error.message)?.[1] || 'pre-spawn'
          console.log(JSON.stringify({ event: 'e2e-injection-failed', stage }))
          throw error
        }
      }
    })
    if (!result.ok) console.log(JSON.stringify({ event: 'e2e-launch-failed',
      integrityOk: result.integrity?.ok, checked: result.integrity?.checked,
      missing: result.integrity?.missing.length, mismatched: result.integrity?.mismatched.length,
      unreadable: result.integrity?.unreadable.length, sessionValid: session.expires > Math.floor(Date.now() / 1000) }))
    assert.equal(result.ok, true)
    await new Promise(resolve => setTimeout(resolve, 4000))
    const alive = (await (await frida.getLocalDevice()).enumerateProcesses({ pids: [result.pid] })).length === 1
    console.log(JSON.stringify({ event: 'e2e-client-proof', attempt, aliveBeforeCleanup: alive }))
    if (attempt === 'first') assert.equal(alive, true)
    console.log(JSON.stringify({ event: 'e2e-cleanup-begin', attempt, at: Date.now() }))
    await cleanup()
    injector = undefined
  }
  session.token = ''
  reply.ticket = ''
  console.log(JSON.stringify({ event: 'e2e-owned-cleanup', remaining: 0 }))
  completing = true
  app.exit(0)
}).catch(fail)
