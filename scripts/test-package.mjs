import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
process.chdir(fileURLToPath(new URL('../', import.meta.url)));
const directory = mkdtempSync(join(tmpdir(), 'arara-desktop-package-'));
try {
  const archive = resolve('packages', `arara-desktop-${process.platform}-${process.arch}.tar.gz`);
  const unpack = spawnSync('tar', ['-xzf', archive, '-C', directory], { encoding: 'utf8' });
  assert.equal(unpack.status, 0, unpack.stderr);
  const executable = process.platform === 'darwin' ? join(directory, 'Arara Desktop.app/Contents/MacOS/arara-desktop') : join(directory, process.platform === 'win32' ? 'arara-desktop.exe' : 'arara-desktop');
  const result = spawnSync(executable, [], { cwd: directory, input: `${JSON.stringify({ version: 1, action: 'status' })}\n`, encoding: 'utf8', timeout: 15000, env: { ...process.env, DOTNET_BUNDLE_EXTRACT_BASE_DIR: join(directory, 'runtime') } });
  assert.equal(result.status, 0, `${result.error ?? ''}${result.stderr}`);
  const response = JSON.parse(result.stdout.trim());
  assert.equal(response.version, 1);
  assert.equal(response.error, undefined);
  assert.equal(response.result.platform, process.platform);
  assert.equal(response.result.audio, false);
  console.log(`PASS extracted ${process.platform}/${process.arch} package starts without build directory sidecars`);
} finally { rmSync(directory, { recursive: true, force: true }); }
