import { mkdirSync, writeFileSync, cpSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const root = fileURLToPath(new URL('../', import.meta.url));
const output = join(root, 'dist');
mkdirSync(output, { recursive: true });
function run(command, args) {
  const result = spawnSync(command, args, { stdio: 'inherit', cwd: root });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} failed`);
}
if (process.platform === 'darwin') {
  const bundle = join(output, 'Arara Desktop.app');
  const executable = join(bundle, 'Contents/MacOS/arara-desktop');
  mkdirSync(join(bundle, 'Contents/MacOS'), { recursive: true });
  mkdirSync(join(bundle, 'Contents/Resources'), { recursive: true });
  cpSync(join(root, 'assets/arara-desktop.icns'), join(bundle, 'Contents/Resources/arara-desktop.icns'));
  cpSync(join(root, 'src/locales'), join(bundle, 'Contents/Resources/locales'), { recursive: true });
  writeFileSync(join(bundle, 'Contents/Info.plist'), '<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>br.app.code.arara.desktop</string><key>CFBundleName</key><string>Arara Desktop</string><key>CFBundleExecutable</key><string>arara-desktop</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleIconFile</key><string>arara-desktop.icns</string><key>CFBundleVersion</key><string>1</string><key>LSUIElement</key><true/><key>NSPrincipalClass</key><string>NSApplication</string></dict></plist>');
  run('xcrun', ['swiftc', '-O', '-target', `${process.arch === 'arm64' ? 'arm64' : 'x86_64'}-apple-macosx13.0`, 'native/macos/main.swift', '-o', executable]);
  run('codesign', ['--force', '--sign', process.env.ARARA_DESKTOP_SIGN_IDENTITY || '-', '--identifier', 'br.app.code.arara.desktop', bundle]);
} else if (process.platform === 'win32') {
  run('dotnet', ['publish', 'native/windows/AraraDesktop.csproj', '-c', 'Release', '-r', `win-${process.arch}`, '--self-contained', 'true', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true', '-o', output]);
} else if (process.platform === 'linux') {
  run('cc', ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', 'native/linux/main.c', '-lX11', '-lXtst', '-ldl', '-lz', '-pthread', '-o', join(output, 'arara-desktop')]);
} else throw new Error('Unsupported platform');
if (process.platform === 'linux') cpSync(join(root, 'assets/arara-desktop.png'), join(output, 'arara-desktop.png'));
