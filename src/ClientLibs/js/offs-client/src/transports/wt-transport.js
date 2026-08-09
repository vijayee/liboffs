import { encodeAuthRequest, getMessageType } from '../wire.js';

/**
 * WebTransport transport for the OFFS client.
 * Sends length-prefixed CBOR frames over an HTTP/3 bidirectional stream.
 */
export class WtTransport {
  /** @type {WebTransport|null} */
  transport = null;
  /** @type {WritableStreamWriter|null} */
  writer = null;
  /** @type {ReadableStreamReader|null} */
  reader = null;
  /** @type {string|undefined} */
  apiKey;
  /** @type {((type: number, bytes: Uint8Array) => void)|null} */
  messageHandler = null;
  /** @type {Promise<void>|null} */
  openPromise = null;
  /** @type {boolean} */
  running = false;

  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(url, apiKey, _options) {
    this.url = url;
    this.apiKey = apiKey;
  }

  /**
   * @returns {Promise<void>}
   */
  async connect() {
    if (this.transport) return this.openPromise || Promise.resolve();

    this.transport = new WebTransport(this.url);
    this.openPromise = this.transport.ready.then(async () => {
      const stream = await this.transport.createBidirectionalStream();
      this.writer = stream.writable.getWriter();
      this.reader = stream.readable.getReader();
      this.running = true;
      this._readLoop();
      if (this.apiKey) {
        await this.send(encodeAuthRequest(this.apiKey));
      }
    });

    return this.openPromise;
  }

  disconnect() {
    this.running = false;
    this.writer?.releaseLock();
    this.reader?.releaseLock();
    this.transport?.close();
    this.writer = null;
    this.reader = null;
    this.transport = null;
    this.openPromise = null;
  }

  isConnected() {
    return this.transport !== null && this.transport.state === 'connected';
  }

  /**
   * @param {Uint8Array} bytes
   */
  async send(bytes) {
    if (!this.writer) throw new Error('WebTransport not connected');
    const length = new Uint8Array(4);
    const view = new DataView(length.buffer);
    view.setUint32(0, bytes.length, false); // big-endian
    await this.writer.write(length);
    await this.writer.write(bytes);
  }

  /**
   * @param {(type: number, bytes: Uint8Array) => void} handler
   */
  setMessageHandler(handler) {
    this.messageHandler = handler;
  }

  async _readLoop() {
    /** @type {Uint8Array|null} */
    let pending = null;
    try {
      while (this.running) {
        const { done, value } = await this.reader.read();
        if (done) break;
        const chunk = value instanceof Uint8Array ? value : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        pending = pending ? _concat(pending, chunk) : chunk;
        while (pending.length >= 4) {
          const view = new DataView(pending.buffer, pending.byteOffset, pending.length);
          const msgLen = view.getUint32(0, false);
          if (pending.length < 4 + msgLen) break;
          const msgBytes = pending.subarray(4, 4 + msgLen);
          const type = getMessageType(msgBytes);
          if (type !== null) {
            this.messageHandler?.(type, msgBytes);
          }
          pending = pending.subarray(4 + msgLen);
        }
      }
    } catch (_err) {
      // ignore errors after disconnect
    }
  }
}

/**
 * @param {Uint8Array} a
 * @param {Uint8Array} b
 * @returns {Uint8Array}
 */
function _concat(a, b) {
  const result = new Uint8Array(a.length + b.length);
  result.set(a, 0);
  result.set(b, a.length);
  return result;
}
