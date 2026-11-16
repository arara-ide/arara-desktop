import { beforeAll, afterAll, expect, it } from 'vitest';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';

const linux = process.platform === 'linux';
let scratch;
let fixture;
beforeAll(() => {
  if (!linux) return;
  const built = spawnSync(process.execPath, ['scripts/build.mjs'], { encoding: 'utf8' });
  expect(built.status, `${built.stdout}\n${built.stderr}`).toBe(0);
  scratch = mkdtempSync(join(tmpdir(), 'arara-portal-test-'));
  fixture = join(scratch, 'portal-fixture');
  const flags = spawnSync('pkg-config', ['--cflags', '--libs', 'gio-2.0'], { encoding: 'utf8' });
  expect(flags.status, flags.stderr).toBe(0);
  const compiled = spawnSync('cc', ['-Wall', '-Wextra', '-Werror', 'native/linux/portal-fixture.c', ...flags.stdout.trim().split(/\s+/), '-o', fixture], { encoding: 'utf8' });
  expect(compiled.status, compiled.stderr).toBe(0);
});
afterAll(() => { if (scratch) rmSync(scratch, { recursive: true, force: true }); });
it.runIf(linux).each(['ready', 'denied', 'partial', 'empty', 'invalid', 'pending', 'delayed'])('handles the isolated Wayland portal: %s', mode => {
  const result = spawnSync('dbus-run-session', ['--', process.execPath, 'scripts/linux-portal-case.mjs', fixture, resolve('dist/arara-desktop'), mode], { encoding: 'utf8', timeout: 15000 });
  expect(result.status, `${result.stdout}\n${result.stderr}`).toBe(0);
  expect(result.stdout).toContain(`PASS isolated Wayland portal ${mode}`);
}, 16000);
