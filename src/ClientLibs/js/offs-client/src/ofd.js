import { encode, decode } from 'cbor-x';

/**
 * @typedef {Object} OfdFileEntry
 * @property {string} name
 * @property {boolean} isDirectory
 * @property {Uint8Array} fileHash
 * @property {Uint8Array} descriptorHash
 * @property {number} finalByte
 * @property {number} blockType
 * @property {number} tupleSize
 * @property {number} fileOffset
 */

/**
 * @typedef {Object} OfdDirectoryEntry
 * @property {string} name
 * @property {boolean} isDirectory
 * @property {Uint8Array} dirHash
 */

/**
 * @typedef {OfdFileEntry|OfdDirectoryEntry} OfdEntry
 */

const DEFAULT_BLOCK_TYPE = 128000;
const DEFAULT_TUPLE_SIZE = 3;

/**
 * Create a file OFD entry.
 * @param {Object} params
 * @param {string} params.name
 * @param {Uint8Array} params.fileHash
 * @param {Uint8Array} params.descriptorHash
 * @param {number} params.finalByte
 * @param {number} [params.blockType=128000]
 * @param {number} [params.tupleSize=3]
 * @param {number} [params.fileOffset=0]
 * @returns {OfdEntry}
 */
export function ofdFile({
  name,
  fileHash,
  descriptorHash,
  finalByte,
  blockType = DEFAULT_BLOCK_TYPE,
  tupleSize = DEFAULT_TUPLE_SIZE,
  fileOffset = 0
}) {
  return {
    name,
    isDirectory: false,
    fileHash,
    descriptorHash,
    finalByte,
    blockType,
    tupleSize,
    fileOffset
  };
}

/**
 * Create a directory OFD entry.
 * @param {Object} params
 * @param {string} params.name
 * @param {Uint8Array} params.dirHash
 * @returns {OfdEntry}
 */
export function ofdDirectory({ name, dirHash }) {
  return { name, isDirectory: true, dirHash };
}

/**
 * Build CBOR-encoded OFD bytes from a list of entries.
 * Format matches the Dart example client (examples/off_client/lib/services/ofd.dart).
 * @param {OfdEntry[]} entries
 * @returns {Uint8Array}
 */
export function buildOfdCbor(entries) {
  const entryMaps = entries.map((entry) => {
    const map = {
      n: entry.name,
      t: entry.isDirectory ? 1 : 0
    };
    if (entry.isDirectory) {
      map.d = entry.dirHash;
    } else {
      map.f = entry.fileHash;
      map.D = entry.descriptorHash;
      map.s = entry.finalByte;
      map.B = entry.blockType;
      map.T = entry.tupleSize;
      map.o = entry.fileOffset;
    }
    return map;
  });

  return encode({ v: 1, entries: entryMaps });
}

/**
 * Parse CBOR-encoded OFD bytes into a list of entries.
 * @param {Uint8Array} data
 * @returns {OfdEntry[]}
 */
export function parseOfdCbor(data) {
  const decoded = decode(data);
  if (!decoded || typeof decoded !== 'object') return [];

  const entries = decoded.entries;
  if (!Array.isArray(entries)) return [];

  return entries.map((entry) => {
    const isDirectory = entry.t === 1;
    if (isDirectory) {
      return ofdDirectory({
        name: String(entry.n),
        dirHash: asUint8Array(entry.d)
      });
    }
    return ofdFile({
      name: String(entry.n),
      fileHash: asUint8Array(entry.f),
      descriptorHash: asUint8Array(entry.D),
      finalByte: safeInt(entry.s),
      blockType: safeInt(entry.B),
      tupleSize: safeInt(entry.T),
      fileOffset: safeInt(entry.o)
    });
  }).filter(Boolean);
}

/**
 * @param {any} value
 * @returns {number}
 */
function safeInt(value) {
  if (typeof value === 'number') return value;
  if (typeof value === 'bigint') return Number(value);
  return 0;
}

/**
 * @param {any} value
 * @returns {Uint8Array}
 */
function asUint8Array(value) {
  if (value instanceof Uint8Array) return value;
  if (Array.isArray(value)) return new Uint8Array(value);
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (value && typeof value === 'object' && ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  return new Uint8Array(0);
}
