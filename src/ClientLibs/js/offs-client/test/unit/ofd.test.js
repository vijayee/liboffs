import { describe, it, expect } from 'vitest';
import { ofdFile, ofdDirectory, buildOfdCbor, parseOfdCbor } from '../../src/ofd.js';

describe('OFD encode/decode', () => {
  it('round-trips a file entry', () => {
    const entry = ofdFile({
      name: 'hello.txt',
      fileHash: new Uint8Array([1, 2, 3]),
      descriptorHash: new Uint8Array([4, 5, 6]),
      finalByte: 12
    });

    const bytes = buildOfdCbor([entry]);
    const parsed = parseOfdCbor(bytes);

    expect(parsed).toHaveLength(1);
    expect(parsed[0].name).toBe('hello.txt');
    expect(parsed[0].isDirectory).toBe(false);
    expect(parsed[0].fileHash).toEqual(new Uint8Array([1, 2, 3]));
    expect(parsed[0].descriptorHash).toEqual(new Uint8Array([4, 5, 6]));
    expect(parsed[0].finalByte).toBe(12);
    expect(parsed[0].blockType).toBe(128000);
    expect(parsed[0].tupleSize).toBe(3);
    expect(parsed[0].fileOffset).toBe(0);
  });

  it('round-trips a directory entry', () => {
    const entry = ofdDirectory({
      name: 'subdir',
      dirHash: new Uint8Array([7, 8, 9])
    });

    const bytes = buildOfdCbor([entry]);
    const parsed = parseOfdCbor(bytes);

    expect(parsed).toHaveLength(1);
    expect(parsed[0].name).toBe('subdir');
    expect(parsed[0].isDirectory).toBe(true);
    expect(parsed[0].dirHash).toEqual(new Uint8Array([7, 8, 9]))
    expect(parsed[0].fileHash).toBeUndefined();
  });

  it('round-trips mixed entries', () => {
    const fileEntry = ofdFile({
      name: 'a.txt',
      fileHash: new Uint8Array([10]),
      descriptorHash: new Uint8Array([20]),
      finalByte: 5,
      blockType: 256000,
      tupleSize: 5,
      fileOffset: 100
    });
    const dirEntry = ofdDirectory({ name: 'nested', dirHash: new Uint8Array([30]) });

    const bytes = buildOfdCbor([fileEntry, dirEntry]);
    const parsed = parseOfdCbor(bytes);

    expect(parsed).toHaveLength(2);
    expect(parsed[0].name).toBe('a.txt');
    expect(parsed[0].blockType).toBe(256000);
    expect(parsed[0].tupleSize).toBe(5);
    expect(parsed[0].fileOffset).toBe(100);
    expect(parsed[1].name).toBe('nested');
  });
});
