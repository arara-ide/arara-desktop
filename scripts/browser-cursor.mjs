import { readFileSync, existsSync, cpSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, join } from 'node:path';

export function browserCursorDirectory() {
  const root = fileURLToPath(new URL('../', import.meta.url));
  const candidates = process.env.ARARA_BROWSER_SOURCE ? [resolve(process.env.ARARA_BROWSER_SOURCE)] : [resolve(root, '../arara-browser'), join(root, '.work/arara-browser')];
  const directory = candidates.find(path => existsSync(join(path, 'extension/cursor.js')) && existsSync(join(path, 'LICENSE')));
  if (!directory) throw new Error('Prepare the arara-browser checkout on the same branch before building desktop feedback.');
  return directory;
}

export function copyBrowserCursor(destination) {
  const directory = browserCursorDirectory();
  const source = readFileSync(join(directory, 'extension/cursor.js'), 'utf8');
  if (!source.includes('globalThis.AraraCursor =') || !source.includes('moveTo')) throw new Error('Unsupported Arara Browser cursor contract');
  cpSync(join(directory, 'extension/cursor.js'), join(destination, 'browser-cursor.js'));
  cpSync(join(directory, 'LICENSE'), join(destination, 'browser-cursor-LICENSE'));
}
