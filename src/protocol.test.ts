import { describe, it, expect } from 'vitest';
import { validateRequest } from './protocol.js';
const base = { version: 1, action: 'input', frameId: 'f', windowId: 'w' };
describe('native input boundaries', () => {
  it.each([null, [], {}, { version: 2, action: 'status' }, { version: 1, action: 'status', command: 'x' }, { ...base, operation: 'click', x: -1, y: 1 }, { ...base, operation: 'keys', keys: ['W'], durationMs: 5001 }, { ...base, operation: 'keys', keys: ['password'] }, { ...base, operation: 'relative_move', dx: Infinity, dy: 0 }, { ...base, frameId: '', operation: 'click', x: 1, y: 2 }])('rejects %j', value => expect(() => validateRequest(value)).toThrow('invalid_request'));
  it.each([{ operation: 'click', x: 5, y: 10, durationMs: 3000 }, { operation: 'keys', keys: ['W', 'SHIFT'], durationMs: 5000 }, { operation: 'relative_move', dx: 20, dy: -10 }, { operation: 'text', value: 'hello' }])('supports bounded native input %j', input => expect(validateRequest({ ...base, ...input })).toEqual({ ...base, ...input }));
});
