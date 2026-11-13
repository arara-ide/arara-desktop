import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
const catalogs = ['en', 'pt', 'es'].map(locale => JSON.parse(readFileSync(new URL(`../src/locales/${locale}.json`, import.meta.url), 'utf8')));
for (const catalog of catalogs) assert.deepEqual(Object.keys(catalog), Object.keys(catalogs[0]));
console.log('PASS catalog parity');
