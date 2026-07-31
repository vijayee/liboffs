import { describe, it, expect } from 'vitest';
import {
  base58Encode,
  base58Decode,
  parseOffUrl,
  offUrlToHttpUrl,
  mimeFromExtension,
  normalizeFolderEntries
} from '../../src/util.js';

describe('base58', () => {
  it('round-trips zero bytes', () => {
    const input = new Uint8Array([0, 0, 0]);
    const encoded = base58Encode(input);
    expect(encoded).toBe('111');
    expect(base58Decode(encoded)).toEqual(input);
  });

  it('round-trips arbitrary bytes', () => {
    const input = new Uint8Array([0x00, 0x01, 0x02, 0xfd, 0xfe, 0xff]);
    const encoded = base58Encode(input);
    expect(encoded).not.toBe('');
    expect(base58Decode(encoded)).toEqual(input);
  });

  it('returns null for invalid characters', () => {
    expect(base58Decode('0')).toBeNull();
    expect(base58Decode('lO')).toBeNull();
    expect(base58Decode('')).toBeNull();
  });
});

describe('parseOffUrl', () => {
  it('parses a valid OFFS URL', () => {
    const url = 'http://localhost:23402/offsystem/v3/offsystem/123/abc/def/hello%20world.txt';
    const parsed = parseOffUrl(url);
    expect(parsed).not.toBeNull();
    expect(parsed.fileHashB58).toBe('abc');
    expect(parsed.descriptorHashB58).toBe('def');
    expect(parsed.streamLength).toBe(123);
    expect(parsed.fileName).toBe('hello world.txt');
  });

  it('returns null for malformed URLs', () => {
    expect(parseOffUrl('http://localhost:23402/offsystem')).toBeNull();
    expect(parseOffUrl('http://localhost:23402/offsystem/v3/a')).toBeNull();
  });
});

describe('mimeFromExtension', () => {
  it('maps known extensions', () => {
    expect(mimeFromExtension('photo.png')).toBe('image/png');
    expect(mimeFromExtension('video.mp4')).toBe('video/mp4');
    expect(mimeFromExtension('index.html')).toBe('text/html');
  });

  it('falls back to octet-stream', () => {
    expect(mimeFromExtension('data.unknown')).toBe('application/octet-stream');
    expect(mimeFromExtension('noext')).toBe('application/octet-stream');
  });
});

describe('normalizeFolderEntries', () => {
  it('normalizes a record to entries', () => {
    const blob = new Blob(['a']);
    const entries = normalizeFolderEntries({ 'folder/file.txt': blob });
    expect(entries).toHaveLength(1);
    expect(entries[0].path).toBe('folder/file.txt');
    expect(entries[0].file).toBe(blob);
  });

  it('normalizes an array of objects', () => {
    const blob = new Blob(['b']);
    const entries = normalizeFolderEntries([{ path: 'x/y.txt', file: blob }]);
    expect(entries).toHaveLength(1);
    expect(entries[0].path).toBe('x/y.txt');
  });
});

describe('offUrlToHttpUrl', () => {
  it('returns an already HTTP URL unchanged', () => {
    const url = 'http://localhost:23402/offsystem/v3/text/plain/10/abc/def/file.txt';
    expect(offUrlToHttpUrl(url)).toBe(url);
  });

  it('prepends baseUrl to a relative OFF path', () => {
    expect(offUrlToHttpUrl('/offsystem/v3/text/plain/10/abc/def/file.txt'))
      .toBe('http://localhost:23402/offsystem/v3/text/plain/10/abc/def/file.txt');
  });

  it('converts an offs:// URL to HTTP', () => {
    expect(offUrlToHttpUrl('offs://localhost/offsystem/v3/text/plain/10/abc/def/file.txt'))
      .toBe('http://localhost:23402/offsystem/v3/text/plain/10/abc/def/file.txt');
  });

  it('uses a custom baseUrl', () => {
    expect(offUrlToHttpUrl('/offsystem/v3/text/plain/10/abc/def/file.txt', 'https://example.com'))
      .toBe('https://example.com/offsystem/v3/text/plain/10/abc/def/file.txt');
  });
});
