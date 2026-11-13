import { describe, it, expect } from 'vitest';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { browserCursorDirectory, copyBrowserCursor } from './browser-cursor.mjs';

describe('shared browser feedback resources', () => {
  it.runIf(process.platform === 'darwin')('copies the original cursor and its license without visual changes', () => {
    const destination = mkdtempSync(join(tmpdir(), 'arara-cursor-'));
    try {
      copyBrowserCursor(destination);
      const source = browserCursorDirectory();
      expect(readFileSync(join(destination, 'browser-cursor.js'))).toEqual(readFileSync(join(source, 'extension/cursor.js')));
      expect(readFileSync(join(destination, 'browser-cursor-LICENSE'))).toEqual(readFileSync(join(source, 'LICENSE')));
      const wrapper = readFileSync(new URL('../assets/cursor.html', import.meta.url), 'utf8');
      expect(wrapper).toContain('src="browser-cursor.js"');
      expect(wrapper).not.toMatch(/https?:\/\//);
    } finally { rmSync(destination, { recursive: true }); }
  });
});
