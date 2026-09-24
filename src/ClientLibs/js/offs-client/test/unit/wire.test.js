import { describe, it, expect } from 'vitest';
import { Encoder, decode } from 'cbor-x';
import * as wire from '../../src/wire.js';

const encoder = new Encoder({ tagUint8Array: false });

describe('bootstrap wire', () => {
  it('encodes add with an endpoint string', () => {
    const bytes = wire.encodeBootstrapAdd('[2001:db8::1]:8080');
    const arr = decode(bytes);
    expect(arr[0]).toBe(wire.MSG.BOOTSTRAP_ADD);
    expect(arr[1]).toBe('[2001:db8::1]:8080');
  });

  it('encodes remove with an endpoint string', () => {
    const bytes = wire.encodeBootstrapRemove('10.0.0.1:8080');
    const arr = decode(bytes);
    expect(arr[0]).toBe(wire.MSG.BOOTSTRAP_REMOVE);
    expect(arr[1]).toBe('10.0.0.1:8080');
  });

  it('encodes a list request and decodes the response', () => {
    const requestBytes = wire.encodeBootstrapListRequest();
    expect(decode(requestBytes)[0]).toBe(wire.MSG.BOOTSTRAP_LIST);

    const responseBytes = encoder.encode([wire.MSG.BOOTSTRAP_LIST_RESPONSE, [
      ['10.0.0.1', 8080, 0],
      ['2001:db8::1', 9090, 1],
    ]]);
    const entries = wire.decodeBootstrapListResponse(responseBytes);
    expect(entries).toHaveLength(2);
    expect(entries[1][0]).toBe('2001:db8::1');
    expect(entries[1][1]).toBe(9090);
    expect(entries[1][2]).toBe(1);
  });

  it('rejects a frame that is not a bootstrap list response', () => {
    const bytes = encoder.encode([wire.MSG.FRIEND_LIST_RESPONSE, []]);
    expect(() => wire.decodeBootstrapListResponse(bytes)).toThrow('Not a bootstrap list response');
  });
});