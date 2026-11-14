import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
process.chdir(fileURLToPath(new URL('../', import.meta.url)));
const { version: appVersion } = JSON.parse(readFileSync('package.json', 'utf8'));
const directory = mkdtempSync(join(tmpdir(), 'arara-desktop-package-'));
try {
  const archive = resolve('packages', `arara-desktop-${process.platform}-${process.arch}.tar.gz`);
  const unpack = spawnSync('tar', ['-xzf', archive, '-C', directory], { encoding: 'utf8' });
  assert.equal(unpack.status, 0, unpack.stderr);
  const executable = process.platform === 'darwin' ? join(directory, 'Arara Desktop.app/Contents/MacOS/arara-desktop') : join(directory, process.platform === 'win32' ? 'arara-desktop.exe' : 'arara-desktop');
  const metadataPath = process.platform === 'darwin' ? join(directory, 'Arara Desktop.app/Contents/Resources/version.json') : join(directory, 'version.json');
  assert.deepEqual(JSON.parse(readFileSync(metadataPath, 'utf8')), { version: appVersion });
  if (process.platform === 'darwin') {
    const plist = join(directory, 'Arara Desktop.app/Contents/Info.plist');
    for (const key of ['CFBundleVersion', 'CFBundleShortVersionString']) {
      const property = spawnSync('/usr/libexec/PlistBuddy', ['-c', `Print :${key}`, plist], { encoding: 'utf8' });
      assert.equal(property.status, 0, property.stderr);
      assert.equal(property.stdout.trim(), appVersion);
    }
    const signature = spawnSync('codesign', ['--verify', '--deep', '--strict', join(directory, 'Arara Desktop.app')], { encoding: 'utf8' });
    assert.equal(signature.status, 0, signature.stderr);
  }
  if (process.platform === 'win32') {
    const fileVersion = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', '(Get-Item -LiteralPath $env:ARARA_TEST_EXECUTABLE).VersionInfo.FileVersion'], { encoding: 'utf8', env: { ...process.env, ARARA_TEST_EXECUTABLE: executable } });
    assert.equal(fileVersion.status, 0, fileVersion.stderr);
    assert.equal(fileVersion.stdout.trim(), `${appVersion}.0`);
  }
  const workingDirectory = join(directory, 'empty-working-directory');
  mkdirSync(workingDirectory);
  const result = spawnSync(executable, [], { cwd: workingDirectory, input: `${JSON.stringify({ version: 1, action: 'status' })}\n`, encoding: 'utf8', timeout: 15000, env: { ...process.env, DOTNET_BUNDLE_EXTRACT_BASE_DIR: join(directory, 'runtime') } });
  assert.equal(result.status, 0, `${result.error ?? ''}${result.stderr}`);
  const response = JSON.parse(result.stdout.trim());
  assert.equal(response.version, 1);
  assert.equal(response.result.appVersion, appVersion);
  assert.equal(response.error, undefined);
  assert.equal(response.result.platform, process.platform);
  assert.equal(response.result.audio, false);
  console.log(`PASS extracted ${process.platform}/${process.arch} package metadata and embedded application version match package.json`);
} finally { rmSync(directory, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); }
