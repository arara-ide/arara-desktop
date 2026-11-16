import { mkdirSync, writeFileSync, cpSync, readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { applicationVersion } from './version.mjs';
import { copyBrowserCursor } from './browser-cursor.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const output = join(root, 'dist');
const { version, fileVersion } = applicationVersion(JSON.parse(readFileSync(join(root, 'package.json'), 'utf8')).version);
const metadata = `${JSON.stringify({ version })}\n`;
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
  writeFileSync(join(bundle, 'Contents/Resources/version.json'), metadata);
  const versionSource = join(output, 'AppVersion.swift');
  writeFileSync(versionSource, `let appVersion = ${JSON.stringify(version)}\n`);
  cpSync(join(root, 'assets/arara-desktop.icns'), join(bundle, 'Contents/Resources/arara-desktop.icns'));
  cpSync(join(root, 'src/locales'), join(bundle, 'Contents/Resources/locales'), { recursive: true });
  copyBrowserCursor(join(bundle, 'Contents/Resources'));
  cpSync(join(root, 'assets/cursor.html'), join(bundle, 'Contents/Resources/cursor.html'));
  writeFileSync(join(bundle, 'Contents/Info.plist'), `<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>br.app.code.arara.desktop</string><key>CFBundleName</key><string>Arara Desktop</string><key>CFBundleExecutable</key><string>arara-desktop</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleIconFile</key><string>arara-desktop.icns</string><key>CFBundleVersion</key><string>${version}</string><key>CFBundleShortVersionString</key><string>${version}</string><key>LSUIElement</key><true/><key>NSPrincipalClass</key><string>NSApplication</string></dict></plist>`);
  run('xcrun', ['swiftc', '-O', '-target', `${process.arch === 'arm64' ? 'arm64' : 'x86_64'}-apple-macosx13.0`, 'native/macos/main.swift', 'native/macos/Feedback.swift', 'native/macos/InputKeys.swift', versionSource, '-o', executable]);
  run('codesign', ['--force', '--sign', process.env.ARARA_DESKTOP_SIGN_IDENTITY || '-', '--identifier', 'br.app.code.arara.desktop', bundle]);
} else if (process.platform === 'win32') {
  run('dotnet', ['publish', 'native/windows/AraraDesktop.csproj', '-c', 'Release', '-r', `win-${process.arch}`, '--self-contained', 'true', `-p:Version=${version}`, `-p:FileVersion=${fileVersion}`, '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true', '-o', output]);
} else if (process.platform === 'linux') {
  const messages = ['en', 'pt', 'es'].map(language => JSON.parse(readFileSync(join(root, `src/locales/${language}.json`), 'utf8'))['setup.linux']);
  writeFileSync(join(output, 'linux-locales.h'), `static const char *arara_linux_setup_messages[] = {${messages.map(value => JSON.stringify(value)).join(',')}};\n`);
  const flags = spawnSync('pkg-config', ['--cflags', 'gio-unix-2.0', 'libei-1.0'], { encoding: 'utf8' });
  const libraries = spawnSync('pkg-config', ['--libs', 'gio-unix-2.0'], { encoding: 'utf8' });
  if (flags.status !== 0 || libraries.status !== 0) throw new Error('Linux build requires GIO and libei development headers');
  run('cc', ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', `-DARARA_APP_VERSION=${JSON.stringify(version)}`, '-Idist', ...flags.stdout.trim().split(/\s+/), 'native/linux/main.c', 'native/linux/portal.c', 'native/linux/eis.c', 'native/linux/platform.c', ...libraries.stdout.trim().split(/\s+/), '-lX11', '-lXtst', '-ldl', '-lz', '-pthread', '-o', join(output, 'arara-desktop')]);
} else throw new Error('Unsupported platform');
if (process.platform === 'linux') cpSync(join(root, 'assets/arara-desktop.png'), join(output, 'arara-desktop.png'));
if (process.platform !== 'darwin') writeFileSync(join(output, 'version.json'), metadata);
