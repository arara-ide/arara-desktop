import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createInterface } from 'node:readline';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, readFileSync, existsSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
process.chdir(fileURLToPath(new URL('../', import.meta.url)));
const root = resolve('.');
const scratch = mkdtempSync(join(tmpdir(), 'arara-desktop-fixture-'));
const binary = process.platform === 'darwin' ? resolve('dist/Arara Desktop.app/Contents/MacOS/arara-desktop') : resolve(`dist/arara-desktop${process.platform === 'win32' ? '.exe' : ''}`);
const run = (command, args) => { const result = spawnSync(command, args, { encoding: 'utf8' }); assert.equal(result.status, 0, result.stderr); };
let fixtureBinary;
let fixtureArgs = [];
let application = process.platform === 'linux' ? 'AraraDesktopFixture' : 'arara-desktop';
if (process.platform === 'darwin') {
  const app = join(scratch, 'Desktop Fixture.app'); fixtureBinary = join(app, 'Contents/MacOS/fixture');
  mkdirSync(join(app, 'Contents/MacOS'), { recursive: true }); application = `br.app.code.arara.fixture.${process.pid}`;
  writeFileSync(join(app, 'Contents/Info.plist'), `<?xml version="1.0"?><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>${application}</string><key>CFBundleExecutable</key><string>fixture</string><key>CFBundlePackageType</key><string>APPL</string></dict></plist>`);
  run('xcrun', ['swiftc', 'native/macos/fixture.swift', '-o', fixtureBinary]);
  run('codesign', ['--force', '--sign', '-', app]);
} else if (process.platform === 'win32') { fixtureBinary = binary; fixtureArgs = ['--fixture']; }
else { fixtureBinary = join(scratch, 'fixture'); run('cc', ['native/linux/fixture.c', '-lX11', '-o', fixtureBinary]); }
const manager = process.platform === 'linux' ? spawn('openbox', [], { stdio: 'ignore' }) : undefined;
const eventFile = join(scratch, 'events.log');
const fixtureApp = process.platform === 'darwin' ? resolve(fixtureBinary, '../../..') : undefined;
const fixture = spawn(fixtureApp ? 'open' : fixtureBinary, fixtureApp ? ['-n', '-a', fixtureApp, '--stdout', eventFile, '--stderr', join(scratch, 'fixture.log')] : fixtureArgs, { stdio: ['ignore', 'pipe', 'inherit'], env: { ...process.env, ARARA_DESKTOP_FIXTURE_DATA: scratch } });
let fixturePid;
const events = [];
const readEvents = () => { if (process.platform === 'darwin' && existsSync(eventFile)) { events.length = 0; for (const line of readFileSync(eventFile, 'utf8').trim().split('\n')) { const [kind, at] = line.split(':'); events.push({ kind, at: Number(at) }); } } };
const eventLines = createInterface({ input: fixture.stdout });
eventLines.on('line', line => { const [kind, at] = line.split(':'); events.push({ kind, at: Number(at) }); });
const native = spawn(binary, [], { stdio: ['pipe', 'pipe', 'inherit'] });
const lines = createInterface({ input: native.stdout });
const pending = []; lines.on('line', line => pending.shift()?.(JSON.parse(line)));
const call = request => new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error(`Timeout: ${request.action}`)), 12000);
  pending.push(reply => { clearTimeout(timer); resolve(reply); }); native.stdin.write(`${JSON.stringify({ version: 1, ...request })}\n`);
});
console.log(`Native fixture starting (${process.platform})`);
const ok = reply => { assert.equal(reply.error, undefined, JSON.stringify(reply)); return reply.result; };
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
try {
  const status = ok(await call({ action: 'status' }));
  assert.equal(status.input, true, 'Real native input session/permission is required');
  assert.equal(status.screenshots, true, 'Real capture permission/session is required');
  let windows = [];
  for (let attempt = 0; attempt < 60 && !windows.length; attempt++) { await sleep(100); windows = ok(await call({ action: 'list_windows', application })).windows.filter(window => window.title === 'Arara Desktop Fixture'); }
  readEvents();
  assert.equal(windows.length, 1, `Isolated fixture window must be present; exit=${fixture.exitCode} events=${JSON.stringify(events)}`);
  fixturePid = windows[0].processId;
  console.log('PASS isolated fixture window found');
  const windowId = windows[0].windowId;
  ok(await call({ action: 'focus_window', windowId })); await sleep(300);
  const snapshot = ok(await call({ action: 'take_snapshot', windowId }));
  assert.ok(!JSON.stringify(snapshot).includes('fixture-secret'));
  if (process.platform !== 'linux') {
    const input = snapshot.nodes.find(node => node.name === 'Fixture input'); assert.ok(input);
    ok(await call({ action: 'act', windowId, uid: input.uid, operation: 'set_value', value: 'Isolated test' }));
    assert.ok((await call({ action: 'act', windowId, uid: input.uid, operation: 'set_value', value: 'stale' })).error);
    const second = ok(await call({ action: 'take_snapshot', windowId }));
    const button = second.nodes.find(node => node.name === 'Apply fixture'); assert.ok(button);
    ok(await call({ action: 'act', windowId, uid: button.uid, operation: 'press' }));
    const result = ok(await call({ action: 'take_snapshot', windowId }));
    assert.ok(result.nodes.some(node => String(node.value ?? node.name).includes('Applied: Isolated test')));
  }
  console.log('PASS native semantic flow');
  const frame = ok(await call({ action: 'take_screenshot', windowId }));
  assert.equal(Buffer.from(frame.images[0].data, 'base64').subarray(1, 4).toString(), 'PNG');
  ok(await call({ action: 'input', windowId, frameId: frame.frameId, operation: 'click', x: Math.round(frame.coordinateWidth * 0.6), y: Math.round(frame.coordinateHeight * 0.8), durationMs: 300 }));
  await sleep(100);
  console.log('PASS native mouse hold completed');
  readEvents();
  const down = events.find(event => event.kind === 'mouse_down'); const up = events.find(event => event.kind === 'mouse_up');
  assert.ok(down && up && up.at - down.at >= 0.25, JSON.stringify(events));
  assert.ok((await call({ action: 'input', windowId, frameId: frame.frameId, operation: 'click', x: 1, y: 1 })).error, 'Consumed frame must be rejected');
  const secondFrame = ok(await call({ action: 'take_screenshot', windowId }));
  const holding = call({ action: 'input', windowId, frameId: secondFrame.frameId, operation: 'keys', keys: ['W'], durationMs: 5000 });
  await sleep(200); const stopping = call({ action: 'stop' });
  assert.ok((await holding).error, 'Stop must interrupt the active hold'); ok(await stopping); await sleep(100);
  readEvents();
  const keyDown = events.find(event => event.kind === 'key_down'); const keyUp = events.find(event => event.kind === 'key_up');
  assert.ok(keyDown && keyUp && keyUp.at - keyDown.at < 1.5, JSON.stringify(events));
  console.log(`PASS real ${process.platform}: isolated accessibility, screenshot, canvas click hold, consumed frame rejection, keyboard hold cancellation and key release`);
} finally {
  if (fixturePid) { try { process.kill(fixturePid); } catch {} }
  if (process.platform === 'win32' && fixture.pid) spawnSync('taskkill', ['/PID', String(fixture.pid), '/T', '/F'], { stdio: 'ignore' });
  else fixture.kill();
  native.stdin.end(); manager?.kill(); lines.close(); eventLines.close(); fixture.stdout.destroy(); native.stdout.destroy();
  await sleep(100); native.kill();
  rmSync(scratch, { recursive: true, force: true });
  process.chdir(root);
}
