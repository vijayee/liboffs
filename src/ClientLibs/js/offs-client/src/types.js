/**
 * @typedef {Object} OffsClientConfig
 * @property {number} [connectTimeoutMs=5000]
 * @property {number} [requestTimeoutMs=30000]
 */

/**
 * @typedef {Object} OffsPutOptions
 * @property {string} contentType
 * @property {string} fileName
 * @property {number} streamLength
 * @property {string} [serverAddress]
 * @property {string[]} [recyclerUrls]
 * @property {boolean} [temporary=false]
 * @property {number} [tupleSize]
 */

/**
 * @typedef {Object} OffsGetCallbacks
 * @property {(data: Uint8Array) => void} onData
 * @property {() => void} [onEnd]
 * @property {(statusCode: number, message: string) => void} [onError]
 * @property {(contentType: string, contentLength: number, hasRange: boolean, rangeStart?: number, rangeEnd?: number) => void} [onStart]
 */

/**
 * @typedef {Object} OffsBlockPutResult
 * @property {number} status
 * @property {Uint8Array|string} hash
 */

export {};
