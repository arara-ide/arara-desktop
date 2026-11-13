export const VERSION = 1;
export const ACTIONS = ['status', 'list_windows', 'focus_window', 'take_snapshot', 'read', 'act', 'take_screenshot', 'input', 'launch_app', 'stop'] as const;
export type NativeAction = typeof ACTIONS[number];
export interface NativeRequest {
  version: 1; action: NativeAction; application?: string; windowId?: string; uid?: string;
  frameId?: string; operation?: string; value?: string; x?: number; y?: number;
  dx?: number; dy?: number; durationMs?: number; button?: 'left' | 'right' | 'middle'; keys?: string[]; limit?: number; offset?: number;
}
export function validateRequest(value: unknown): NativeRequest {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('invalid_request');
  const input = value as Record<string, unknown>;
  const allowed = new Set(['version', 'action', 'application', 'windowId', 'uid', 'frameId', 'operation', 'value', 'x', 'y', 'dx', 'dy', 'durationMs', 'button', 'keys', 'limit', 'offset']);
  if (input.version !== VERSION || !ACTIONS.includes(input.action as NativeAction) || Object.keys(input).some(key => !allowed.has(key))) throw new Error('invalid_request');
  for (const key of ['application', 'windowId', 'uid', 'frameId', 'operation', 'value', 'button']) if (input[key] !== undefined && (typeof input[key] !== 'string' || (input[key] as string).length > (key === 'value' ? 8000 : 200))) throw new Error('invalid_request');
  for (const key of ['x', 'y', 'dx', 'dy', 'durationMs', 'limit', 'offset']) if (input[key] !== undefined && (typeof input[key] !== 'number' || !Number.isFinite(input[key]))) throw new Error('invalid_request');
  if (input.action === 'input') {
    if (!input.windowId || !input.frameId || !['move', 'relative_move', 'click', 'keys', 'text'].includes(String(input.operation))) throw new Error('invalid_request');
    if (input.durationMs !== undefined && (!Number.isInteger(input.durationMs) || Number(input.durationMs) < 0 || Number(input.durationMs) > 5000)) throw new Error('invalid_request');
    if (['move', 'click'].includes(String(input.operation)) && (typeof input.x !== 'number' || typeof input.y !== 'number' || input.x < 0 || input.y < 0)) throw new Error('invalid_request');
    if (input.operation === 'relative_move' && (typeof input.dx !== 'number' || typeof input.dy !== 'number' || Math.abs(input.dx) > 2000 || Math.abs(input.dy) > 2000)) throw new Error('invalid_request');
    if (input.operation === 'keys' && (!Array.isArray(input.keys) || !input.keys.length || input.keys.length > 6 || input.keys.some(key => typeof key !== 'string' || !/^(?:[A-Z0-9]|ENTER|TAB|SPACE|BACKSPACE|ESCAPE|META|SHIFT|ALT|CTRL|LEFT|RIGHT|DOWN|UP)$/i.test(key)))) throw new Error('invalid_request');
    if (input.operation === 'text' && typeof input.value !== 'string') throw new Error('invalid_request');
    if (input.button !== undefined && !['left', 'right', 'middle'].includes(String(input.button))) throw new Error('invalid_request');
  }
  if (input.action === 'launch_app' && !input.application) throw new Error('invalid_request');
  if (!['status', 'list_windows', 'launch_app', 'stop'].includes(String(input.action)) && !input.windowId) throw new Error('invalid_request');
  if (['act', 'read'].includes(String(input.action)) && !input.uid) throw new Error('invalid_request');
  return input as unknown as NativeRequest;
}
