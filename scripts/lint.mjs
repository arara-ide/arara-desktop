import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
function check(directory) {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    if (['node_modules', 'dist', 'bin', 'obj', '.git', '.work'].includes(entry.name)) continue;
    const file = join(directory, entry.name);
    if (entry.isDirectory()) check(file);
    else if (/\.(ts|mjs|swift|cs|c)$/.test(file)) {
      const source = readFileSync(file, 'utf8');
      if (/\r|[ \t]+$/m.test(source) || !source.endsWith('\n')) throw new Error(`Invalid whitespace: ${file}`);
    }
  }
}
check('.');
