import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { applicationVersion } from './version.mjs';

describe('application version metadata', () => {
  it('uses the package version and Windows file version', () => {
    const { version } = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
    const lock = JSON.parse(readFileSync(new URL('../package-lock.json', import.meta.url), 'utf8'));
    expect(applicationVersion(version)).toEqual({ version, fileVersion: `${version}.0` });
    expect(lock.version).toBe(version);
    expect(lock.packages[''].version).toBe(version);
  });

  it.each([undefined, null, 1, '', '1.2', '1.2.3-beta', '1.2.3+build', '1.2.3\n', '1.2.65536', '1.2."3'])('rejects invalid native version %j', version => {
    expect(() => applicationVersion(version)).toThrow('Invalid application version');
  });
});
