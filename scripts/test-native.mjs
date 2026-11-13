import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { resolve } from 'node:path';
const binary = process.platform === 'darwin' ? resolve('dist/Arara Desktop.app/Contents/MacOS/arara-desktop') : resolve(`dist/arara-desktop${process.platform === 'win32' ? '.exe' : ''}`);
const child = spawn(binary, [], { stdio: ['pipe', 'pipe', 'inherit'] });
const lines = createInterface({ input: child.stdout });
const pending = [];
lines.on('line', line => pending.shift()?.(JSON.parse(line)));
const call = request => new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error('Native response timeout')), 12000);
  pending.push(reply => { clearTimeout(timer); assert.equal(reply.version, 1); resolve(reply); });
  child.stdin.write(`${JSON.stringify({ version: 1, ...request })}\n`);
});
try {
  const status = await call({ action: 'status' });
  assert.equal(status.result.platform, process.platform);
  assert.equal(status.result.audio, false);
  assert.equal(typeof status.result.accessibility, 'boolean');
  assert.ok((await call({ action: 'unknown' })).error);
  assert.ok((await call({ action: 'input', windowId: 'missing', frameId: 'missing', operation: 'keys', keys: ['W'], durationMs: 99999 })).error);
  assert.equal((await call({ action: 'stop' })).result.stopped, true);
  console.log(`PASS native protocol, capabilities, malformed target rejection and stop (${process.platform})`);
  if (!process.argv.includes('--status')) {
    assert.equal(status.result.accessibility, true, 'Native accessibility permission/session is required; this scenario is not approved');
    assert.equal(status.result.input, true, 'Native input session is required; this scenario is not approved');
    const result = await call({ action: 'list_windows', application: 'AraraDesktopFixture' });
    assert.ok(Array.isArray(result.result.windows));
    console.log('PASS native isolated window enumeration');
  }
} finally {
  child.stdin.end();
  await new Promise(resolve => { child.once('exit', resolve); setTimeout(() => { child.kill(); resolve(); }, 1500).unref(); });
  lines.close();
}
