import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { mkdtempSync, mkdirSync, writeFileSync, existsSync, rmSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

const [fixture, binary, mode] = process.argv.slice(2);
const scratch = mkdtempSync(join(tmpdir(), 'arara-portal-'));
copyFileSync('assets/arara-desktop.png', join(scratch, 'image.png'));
const server = spawn(fixture, [mode], { env: { ...process.env, PORTAL_FIXTURE_IMAGE: join(scratch, 'image.png') }, stdio: ['ignore', 'pipe', 'inherit'] });
const logs = [];
const serverLines = createInterface({ input: server.stdout });
const ready = new Promise(resolve => serverLines.on('line', line => {
  logs.push(line);
  if (line === 'READY') resolve();
}));
const application = 'arara-isolated-launcher';
mkdirSync(join(scratch, 'applications'));
writeFileSync(join(scratch, 'launch.sh'), 'printf "protocol-must-stay-clean\\n"\nprintf ready > "$ARARA_LAUNCH_RECEIPT"\n');
writeFileSync(join(scratch, 'applications', `${application}.desktop`), `[Desktop Entry]\nType=Application\nName=Arara Fixture\nExec=/bin/sh "${join(scratch, 'launch.sh')}"\n`);
let native;
let lines;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
try {
  await ready;
  native = spawn(binary, [], { env: { ...process.env, WAYLAND_DISPLAY: 'isolated-wayland', DISPLAY: '', XDG_DATA_HOME: scratch, ARARA_LAUNCH_RECEIPT: join(scratch, 'receipt') }, stdio: ['pipe', 'pipe', 'inherit'] });
  lines = createInterface({ input: native.stdout });
  const pending = [];
  lines.on('line', line => pending.shift()?.(JSON.parse(line)));
  const call = request => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Timeout: ${request.action}`)), 8000);
    pending.push(result => { clearTimeout(timer); resolve(result); });
    native.stdin.write(`${JSON.stringify({ version: 1, ...request })}\n`);
  });
  const before = (await call({ action: 'status' })).result;
  assert.equal(before.portalAvailable, true);
  assert.equal(before.portalRequired, true);
  assert.equal(before.input, false);
  assert.ok(!logs.includes('METHOD CreateSession'), 'status must not request permission');
  assert.equal((await call({ action: 'launch_app', application })).result.requested, true);
  for (let i = 0; i < 50 && !existsSync(join(scratch, 'receipt')); i++) await sleep(10);
  assert.equal(existsSync(join(scratch, 'receipt')), true, 'Wayland must launch an installed desktop entry without input permission');
  for (const application of ['missing-entry', '-bad', 'x;touch', '../escape']) assert.ok((await call({ action: 'launch_app', application })).error);
  const settingUp = call({ action: 'setup' });
  const initial = await settingUp;
  if (!['empty', 'invalid'].includes(mode)) {
    for (let i = 0; i < 100 && !logs.includes('METHOD Start'); i++) await sleep(10);
    assert.ok(logs.includes('METHOD Start'), 'The native event loop must keep progressing while no tool request is running');
  }
  let observed = (await call({ action: 'status' })).result;
  for (let i = 0; i < 100 && observed.permissionPending && mode !== 'pending' && mode !== 'delayed'; i++) {
    await sleep(10); observed = (await call({ action: 'status' })).result;
  }
  if (mode === 'pending') {
    assert.equal(initial.result.permissionPending, true);
    assert.equal((await call({ action: 'status' })).result.permissionPending, true);
    const count = logs.filter(line => line === 'METHOD Start').length;
    assert.equal((await call({ action: 'setup' })).result.permissionPending, true);
    assert.equal(logs.filter(line => line === 'METHOD Start').length, count);
    assert.equal((await call({ action: 'stop' })).result.stopped, true);
    assert.ok(logs.includes('METHOD Close'));
  } else if (mode === 'delayed') {
    assert.equal(initial.result.permissionPending, true);
    await sleep(1800);
    const status = (await call({ action: 'status' })).result;
    assert.equal(status.input, true);
    assert.equal(status.permissionPending, false);
    assert.equal(logs.filter(line => line === 'METHOD Start').length, 1);
  } else if (mode === 'ready') {
    assert.equal(initial.error, undefined);
    const status = (await call({ action: 'status' })).result;
    assert.equal(status.input, true);
    assert.equal(status.portalRequired, false);
    const screens = (await call({ action: 'list_windows' })).result.windows.filter(window => window.surface === 'screen');
    assert.equal(screens.length, 1);
    assert.equal(screens[0].x, 0);
    assert.equal(screens[0].y, 0);
    const captured = (await call({ action: 'take_screenshot', windowId: screens[0].windowId })).result;
    assert.equal(Buffer.from(captured.images[0].data, 'base64').subarray(1, 4).toString(), 'PNG');
    assert.equal((await call({ action: 'stop' })).result.stopped, true);
    assert.equal((await call({ action: 'status' })).result.input, false);
  } else {
    assert.equal((await call({ action: 'setup' })).error, ['denied', 'partial'].includes(mode) ? 'portal_denied' : 'portal_unavailable');
    assert.equal((await call({ action: 'status' })).result.input, false);
    if (mode === 'denied') {
      const count = logs.filter(line => line === 'METHOD Start').length;
      assert.equal((await call({ action: 'setup' })).error, 'portal_denied');
      assert.equal(logs.filter(line => line === 'METHOD Start').length, count, 'Do not repeat a denied request in the same helper');
    }
  }
  assert.ok(logs.includes('SCREENSHOT interactive=0'));
  assert.ok(!logs.includes('METHOD SelectSources'), 'Never ask the person to select a monitor');
  console.log(`PASS isolated Wayland portal ${mode}, application launch and protocol isolation`);
} finally {
  native?.stdin.end();
  native?.kill();
  server.kill();
  lines?.close();
  serverLines.close();
  rmSync(scratch, { recursive: true, force: true });
}
