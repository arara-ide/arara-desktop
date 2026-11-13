import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
process.chdir(fileURLToPath(new URL('../', import.meta.url)));
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PLAYWRIGHT_PATH || 'playwright');
const browser = await chromium.launch({ headless: true });
const entries = [];
let icon256;
try {
  const svg = readFileSync('assets/arara.svg', 'utf8');
  for (const [type, size] of [['icp4', 16], ['icp5', 32], ['icp6', 64], ['ic07', 128], ['ic08', 256], ['ic09', 512], ['ic10', 1024]]) {
    const page = await browser.newPage({ viewport: { width: size, height: size }, deviceScaleFactor: 1 });
    await page.setContent(`<html><body style="margin:0;background:transparent">${svg.replace('<svg ', `<svg width="${size}" height="${size}" `)}</body></html>`);
    const png = await page.screenshot({ omitBackground: true });
    const header = Buffer.alloc(8); header.write(type); header.writeUInt32BE(png.length + 8, 4); entries.push(Buffer.concat([header, png]));
    if (size === 256) icon256 = png;
    if (size === 512) writeFileSync(resolve('assets/arara-desktop.png'), png);
    await page.close();
  }
  const header = Buffer.alloc(8); header.write('icns'); header.writeUInt32BE(8 + entries.reduce((total, entry) => total + entry.length, 0), 4);
  writeFileSync('assets/arara-desktop.icns', Buffer.concat([header, ...entries]));
  const ico = Buffer.alloc(22); ico.writeUInt16LE(1, 2); ico.writeUInt16LE(1, 4); ico.writeUInt16LE(1, 10); ico.writeUInt16LE(32, 12); ico.writeUInt32LE(icon256.length, 14); ico.writeUInt32LE(22, 18);
  writeFileSync('assets/arara-desktop.ico', Buffer.concat([ico, icon256]));
  mkdirSync('dist', { recursive: true });
  console.log('Generated green Arara desktop icons');
} finally { await browser.close(); }
