export function applicationVersion(version) {
  if (typeof version !== 'string' || !/^\d+\.\d+\.\d+$/.test(version)) throw new Error('Invalid application version');
  const components = version.split('.').map(Number);
  if (components.some(value => value > 65535)) throw new Error('Invalid application version');
  return { version, fileVersion: `${version}.0` };
}
