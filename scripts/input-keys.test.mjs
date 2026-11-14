import { afterAll, beforeAll, describe, it, expect } from 'vitest';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { execFile, spawnSync } from 'node:child_process';
import { promisify } from 'node:util';

describe.runIf(process.platform === 'darwin')('native modifier state', () => {
  let directory;
  let binary;
  beforeAll(async () => {
    directory = mkdtempSync(join(tmpdir(), 'arara-key-flags-'));
    const main = join(directory, 'main.swift');
    writeFileSync(main, 'import CoreGraphics\nprecondition(desktopModifierFlags([]).isEmpty)\nprecondition(desktopModifierFlags([5]).isEmpty)\nprecondition(desktopModifierFlags([55, 56, 5]) == [.maskCommand, .maskShift])\nprecondition(desktopModifierFlags([58, 59]) == [.maskAlternate, .maskControl])\nvar keys: Set<CGKeyCode> = [55, 56, 5]\nkeys.remove(5)\nprecondition(desktopModifierFlags(keys) == [.maskCommand, .maskShift])\nkeys.remove(55)\nprecondition(desktopModifierFlags(keys) == [.maskShift])\nkeys.remove(56)\nprecondition(desktopModifierFlags(keys).isEmpty)\n');
    binary = join(directory, 'keys');
    await promisify(execFile)('xcrun', ['swiftc', resolve('native/macos/InputKeys.swift'), main, '-o', binary], { encoding: 'utf8', timeout: 90000 });
  }, 100000);
  afterAll(() => { if (directory) rmSync(directory, { recursive: true }); });
  it('preserves chord modifiers and removes released modifiers', () => {
    const tested = spawnSync(binary, [], { encoding: 'utf8', timeout: 3000 });
    expect(tested.status, tested.stderr).toBe(0);
  });
});
