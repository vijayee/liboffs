import { Encoder, decode } from 'cbor-x';

const encoder = new Encoder({ tagUint8Array: false });

// Message types (must match client_api_wire.h)
export const MSG = {
  PUT_REQUEST: 1,
  PUT_DATA: 2,
  PUT_END: 3,
  PUT_RESPONSE: 4,
  GET_REQUEST: 5,
  GET_RESPONSE_START: 6,
  GET_DATA: 7,
  GET_END: 8,
  ERROR: 11,
  AUTH_REQUEST: 12,
  BLOCK_PUT_REQUEST: 13,
  BLOCK_PUT_RESPONSE: 14,
  BLOCK_GET_REQUEST: 15,
  BLOCK_GET_RESPONSE: 16,
  BLOCK_DELETE_REQUEST: 17,
  BLOCK_DELETE_RESPONSE: 18,
  HEALTH_REQUEST: 19,
  HEALTH_RESPONSE: 20,
  PEER_INFO_REQUEST: 21,
  PEER_INFO_RESPONSE: 22,
  PEER_CONNECT: 23,
  PEER_CONNECT_RESULT: 24,
  PEER_LIST_REQUEST: 25,
  PEER_LIST_RESPONSE: 26,
  FRIEND_ADD: 27,
  FRIEND_REMOVE: 28,
  FRIEND_LIST: 29,
  FRIEND_LIST_RESPONSE: 30,
  UPDATE_STATUS_REQUEST: 31,
  UPDATE_STATUS_RESPONSE: 32,
  CONFIG_SHOW_REQUEST: 33,
  CONFIG_SHOW_RESPONSE: 34,
  CONFIG_SET_REQUEST: 35,
  CONFIG_SET_RESPONSE: 36,
  CONFIG_RELOAD_REQUEST: 37,
  CONFIG_RELOAD_RESPONSE: 38
};

export const STATUS = {
  OK: 0,
  BAD_REQUEST: 1,
  NOT_FOUND: 2,
  INTERNAL_ERROR: 3,
  RANGE_NOT_SATISFIABLE: 4,
  UNAUTHORIZED: 5
};

/**
 * @param {Uint8Array} bytes
 * @returns {number}
 */
export function getMessageType(bytes) {
  const arr = decode(bytes);
  return Array.isArray(arr) ? arr[0] : null;
}

// --- Auth ---

/**
 * @param {string} apiKey
 * @returns {Uint8Array}
 */
export function encodeAuthRequest(apiKey) {
  const keyBytes = new TextEncoder().encode(apiKey);
  return encoder.encode([MSG.AUTH_REQUEST, keyBytes]);
}

// --- PUT ---

/**
 * @param {import('./types.js').OffsPutOptions} options
 * @param {Uint8Array|null} data
 * @returns {Uint8Array}
 */
export function encodePutRequest(options, data = null) {
  const recycler = options.recyclerUrls || [];
  const payload = [
    MSG.PUT_REQUEST,
    options.contentType,
    options.fileName,
    options.streamLength,
    options.serverAddress || null,
    data || new Uint8Array(0),
    recycler,
    options.temporary ? 1 : 0
  ];
  if (options.tupleSize !== undefined) {
    payload.push(options.tupleSize);
  }
  return encoder.encode(payload);
}

/**
 * @param {Uint8Array} chunk
 * @returns {Uint8Array}
 */
export function encodePutData(chunk) {
  return encoder.encode([MSG.PUT_DATA, chunk]);
}

/**
 * @returns {Uint8Array}
 */
export function encodePutEnd() {
  return encoder.encode([MSG.PUT_END]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{oriString: string}}
 */
export function decodePutResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.PUT_RESPONSE) throw new Error('Not a put response');
  return { oriString: arr[1] };
}

// --- GET ---

/**
 * @param {string} oriString
 * @param {{start?: number, end?: number}} [range]
 * @returns {Uint8Array}
 */
export function encodeGetRequest(oriString, range) {
  const hasRange = range && (range.start !== undefined || range.end !== undefined);
  const payload = [MSG.GET_REQUEST, oriString, hasRange ? 1 : 0];
  if (hasRange) {
    payload.push(range.start || 0);
    payload.push(range.end || 0);
  }
  return encoder.encode(payload);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{contentType: string, contentLength: number, hasRange: boolean, rangeStart?: number, rangeEnd?: number}}
 */
export function decodeGetResponseStart(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.GET_RESPONSE_START) throw new Error('Not a get response start');
  return {
    contentType: arr[1],
    contentLength: arr[2],
    hasRange: arr[3] === 1,
    rangeStart: arr[3] ? arr[4] : undefined,
    rangeEnd: arr[3] ? arr[5] : undefined
  };
}

/**
 * @param {Uint8Array} bytes
 * @returns {Uint8Array}
 */
export function decodeGetData(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.GET_DATA) throw new Error('Not a get data');
  return arr[1];
}

/**
 * @param {Uint8Array} bytes
 * @returns {boolean}
 */
export function isGetEnd(bytes) {
  const arr = decode(bytes);
  return Array.isArray(arr) && arr[0] === MSG.GET_END;
}

// --- Error ---

/**
 * @param {Uint8Array} bytes
 * @returns {{statusCode: number, message: string}|null}
 */
export function decodeError(bytes) {
  const arr = decode(bytes);
  if (!Array.isArray(arr) || arr[0] !== MSG.ERROR) return null;
  return { statusCode: arr[1], message: arr[2] };
}

// --- Block ---

/**
 * @param {Uint8Array} data
 * @param {number} encoding 0=raw, 1=base58
 * @returns {Uint8Array}
 */
export function encodeBlockPutRequest(data, encoding = 0) {
  return encoder.encode([MSG.BLOCK_PUT_REQUEST, data, encoding]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{status: number, hash: Uint8Array|string}}
 */
export function decodeBlockPutResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.BLOCK_PUT_RESPONSE) throw new Error('Not a block put response');
  return { status: arr[1], hash: arr[2] };
}

/**
 * @param {Uint8Array} hash
 * @returns {Uint8Array}
 */
export function encodeBlockGetRequest(hash) {
  return encoder.encode([MSG.BLOCK_GET_REQUEST, hash]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{status: number, data: Uint8Array}}
 */
export function decodeBlockGetResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.BLOCK_GET_RESPONSE) throw new Error('Not a block get response');
  return { status: arr[1], data: arr[2] };
}

/**
 * @param {Uint8Array} hash
 * @returns {Uint8Array}
 */
export function encodeBlockDeleteRequest(hash) {
  return encoder.encode([MSG.BLOCK_DELETE_REQUEST, hash]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{status: number}}
 */
export function decodeBlockDeleteResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.BLOCK_DELETE_RESPONSE) throw new Error('Not a block delete response');
  return { status: arr[1] };
}

// --- Health ---

/**
 * @returns {Uint8Array}
 */
export function encodeHealthRequest() {
  return encoder.encode([MSG.HEALTH_REQUEST]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{json: string}}
 */
export function decodeHealthResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.HEALTH_RESPONSE) throw new Error('Not a health response');
  return { json: arr[1] };
}

// --- Peer ---

/**
 * @param {number} [format=0] 0=cbor, 1=base58, 2=qrcode (PPM image)
 * @returns {Uint8Array}
 */
export function encodePeerInfoRequest(format = 0) {
  if (format === 0) {
    return encoder.encode([MSG.PEER_INFO_REQUEST]); // 1-element shape, unchanged
  }
  return encoder.encode([MSG.PEER_INFO_REQUEST, format]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{format: number, data: Uint8Array}}
 */
export function decodePeerInfoResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.PEER_INFO_RESPONSE) throw new Error('Not a peer info response');
  return { format: arr[1], data: arr[2] };
}

/**
 * @param {number} format 0=cbor, 1=base58
 * @param {Uint8Array} data
 * @returns {Uint8Array}
 */
export function encodePeerConnect(format, data) {
  return encoder.encode([MSG.PEER_CONNECT, format, data]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{status: number}}
 */
export function decodePeerConnectResult(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.PEER_CONNECT_RESULT) throw new Error('Not a peer connect result');
  return { status: arr[1] };
}

/**
 * @returns {Uint8Array}
 */
export function encodePeerListRequest() {
  return encoder.encode([MSG.PEER_LIST_REQUEST]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {any[]}
 */
export function decodePeerListResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.PEER_LIST_RESPONSE) throw new Error('Not a peer list response');
  return arr[1];
}

// --- Friend ---

/**
 * @param {number} format
 * @param {Uint8Array} data
 * @returns {Uint8Array}
 */
export function encodeFriendAdd(format, data) {
  return encoder.encode([MSG.FRIEND_ADD, format, data]);
}

/**
 * @param {Uint8Array} nodeId
 * @returns {Uint8Array}
 */
export function encodeFriendRemove(nodeId) {
  return encoder.encode([MSG.FRIEND_REMOVE, nodeId]);
}

/**
 * @returns {Uint8Array}
 */
export function encodeFriendListRequest() {
  return encoder.encode([MSG.FRIEND_LIST]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {any[]}
 */
export function decodeFriendListResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.FRIEND_LIST_RESPONSE) throw new Error('Not a friend list response');
  return arr[1];
}

// --- Config ---

/**
 * @returns {Uint8Array}
 */
export function encodeConfigShowRequest() {
  return encoder.encode([MSG.CONFIG_SHOW_REQUEST]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{json: string}}
 */
export function decodeConfigShowResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.CONFIG_SHOW_RESPONSE) throw new Error('Not a config show response');
  return { json: arr[1] };
}

/**
 * @param {string} field
 * @param {string} value
 * @returns {Uint8Array}
 */
export function encodeConfigSetRequest(field, value) {
  return encoder.encode([MSG.CONFIG_SET_REQUEST, field, value]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{status: number, restartRequired: boolean, message: string}}
 */
export function decodeConfigSetResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.CONFIG_SET_RESPONSE) throw new Error('Not a config set response');
  return { status: arr[1], restartRequired: arr[2] === 1, message: arr[3] };
}

/**
 * @returns {Uint8Array}
 */
export function encodeConfigReloadRequest() {
  return encoder.encode([MSG.CONFIG_RELOAD_REQUEST]);
}

/**
 * @param {Uint8Array} bytes
 * @returns {{status: number, message: string}}
 */
export function decodeConfigReloadResponse(bytes) {
  const arr = decode(bytes);
  if (arr[0] !== MSG.CONFIG_RELOAD_RESPONSE) throw new Error('Not a config reload response');
  return { status: arr[1], message: arr[2] };
}
