import * as esbuild from 'esbuild';
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';

const outdir = 'dist-bundle';
const require = createRequire(import.meta.url);

await esbuild.build({
  entryPoints: ['src/main.ts'],
  bundle: true,
  platform: 'node',
  target: 'node22',
  outdir,
  format: 'esm',
  external: ['better-sqlite3'],
  sourcemap: true,
  minify: false,
  splitting: false,
});

const configDir = path.join('..', '..', 'config');
const appConfigDir = path.join('config');
const destConfigDir = path.join(outdir, 'config');
fs.mkdirSync(destConfigDir, { recursive: true });
for (const sourceDir of [configDir, appConfigDir]) {
  if (!fs.existsSync(sourceDir)) continue;
  const files = fs.readdirSync(sourceDir);
  files.forEach(file => {
    if (file.endsWith('.yaml')) fs.copyFileSync(path.join(sourceDir, file), path.join(destConfigDir, file));
  });
}

const publicDir = path.join('public');
if (fs.existsSync(publicDir)) {
  const destPublicDir = path.join(outdir, 'public');
  if (!fs.existsSync(destPublicDir)) {
    fs.mkdirSync(destPublicDir, { recursive: true });
  }
  function copyDir(src, dest) {
    if (!fs.existsSync(dest)) fs.mkdirSync(dest, { recursive: true });
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
      const srcPath = path.join(src, entry.name);
      const destPath = path.join(dest, entry.name);
      if (entry.isDirectory()) {
        copyDir(srcPath, destPath);
      } else {
        fs.copyFileSync(srcPath, destPath);
      }
    }
  }
  copyDir(publicDir, destPublicDir);
}

const migrationsDir = path.join('src', 'db', 'migrations');
if (fs.existsSync(migrationsDir)) {
  const destMigrationsDir = path.join(outdir, 'db', 'migrations');
  fs.mkdirSync(destMigrationsDir, { recursive: true });
  for (const file of fs.readdirSync(migrationsDir)) {
    if (file.endsWith('.sql')) fs.copyFileSync(path.join(migrationsDir, file), path.join(destMigrationsDir, file));
  }
}

try {
  const sqlJsMain = require.resolve('sql.js');
  const wasmPath = path.join(path.dirname(sqlJsMain), 'sql-wasm.wasm');
  if (fs.existsSync(wasmPath)) fs.copyFileSync(wasmPath, path.join(outdir, 'sql-wasm.wasm'));
} catch (err) {
  console.warn('Could not locate sql-wasm.wasm:', err.message);
}

console.log('Bundle created in', outdir);
