import { encodeAuthRequest, getMessageType } from '../wire.js';

/**
 * WebSocket transport for the OFFS client.
 * Sends CBOR messages as binary WebSocket frames.
 */
export class WsTransport {
  /** @type {WebSocket|null} */
  socket = null;
  /** @type {string|undefined} */
  apiKey;
  /** @type {((type: number, bytes: Uint8Array) => void)|null} */
  messageHandler = null;
  /** @type {Promise<void>|null} */
  openPromise = null;

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
  connect() {
    if (this.socket) {
      return this.openPromise || Promise.resolve();
    }

    this.socket = new WebSocket(this.url);
    this.socket.binaryType = 'arraybuffer';

    this.openPromise = new Promise((resolve, reject) => {
      const socket = this.socket;
      if (!socket) return reject(new Error('Socket not created'));

      socket.onopen = () => {
        if (this.apiKey) {
          this.send(encodeAuthRequest(this.apiKey));
        }
        resolve();
      };
      socket.onerror = (event) => {
        const message = event.message || event.error?.message || 'unknown';
        reject(new Error(`WebSocket error: ${message}`));
      };
      socket.onclose = () => {
        this.socket = null;
        this.openPromise = null;
      };
      socket.onmessage = (event) => {
        const bytes = new Uint8Array(event.data);
        const type = getMessageType(bytes);
        if (type !== null) {
          this.messageHandler?.(type, bytes);
        }
      };
    });

    return this.openPromise;
  }

  disconnect() {
    if (this.socket) {
      this.socket.close();
      this.socket = null;
    }
    this.openPromise = null;
  }

  isConnected() {
    return this.socket !== null && this.socket.readyState === WebSocket.OPEN;
  }

  /**
   * @param {Uint8Array} bytes
   */
  send(bytes) {
    if (!this.isConnected()) {
      throw new Error('WebSocket not connected');
    }
    this.socket.send(bytes);
  }

  /**
   * @param {(type: number, bytes: Uint8Array) => void} handler
   */
  setMessageHandler(handler) {
    this.messageHandler = handler;
  }
}
