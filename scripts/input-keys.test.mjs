import { describe, it, expect } from 'vitest';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';

describe('native modifier state', () => {
  it.runIf(process.platform === 'darwin')('preserves chord modifiers and removes released modifiers', () => {
    const directory = mkdtempSync(join(tmpdir(), 'arara-key-flags-'));
    try {
      const main = join(directory, 'main.swift');
      writeFileSync(main, 'import CoreGraphics\nprecondition(desktopModifierFlags([]).isEmpty)\nprecondition(desktopModifierFlags([5]).isEmpty)\nprecondition(desktopModifierFlags([55, 56, 5]) == [.maskCommand, .maskShift])\nprecondition(desktopModifierFlags([58, 59]) == [.maskAlternate, .maskControl])\nvar keys: Set<CGKeyCode> = [55, 56, 5]\nkeys.remove(5)\nprecondition(desktopModifierFlags(keys) == [.maskCommand, .maskShift])\nkeys.remove(55)\nprecondition(desktopModifierFlags(keys) == [.maskShift])\nkeys.remove(56)\nprecondition(desktopModifierFlags(keys).isEmpty)\n');
      const binary = join(directory, 'keys');
      const built = spawnSync('xcrun', ['swiftc', resolve('native/macos/InputKeys.swift'), main, '-o', binary], { encoding: 'utf8' });
      expect(built.status, built.stderr).toBe(0);
      const tested = spawnSync(binary, [], { encoding: 'utf8' });
      expect(tested.status, tested.stderr).toBe(0);
    } finally { rmSync(directory, { recursive: true }); }
  });
});
