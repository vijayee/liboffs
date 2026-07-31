import { HttpTransport } from './transports/http-transport.js';
import { WsTransport } from './transports/ws-transport.js';
import { WtTransport } from './transports/wt-transport.js';
import * as wire from './wire.js';
import {
  base58Decode,
  base58Encode,
  parseOffUrl,
  offUrlToHttpUrl,
  mimeFromExtension,
  fileToReadableStream,
  normalizeFolderEntries,
  basename
} from './util.js';
import { buildOfdCbor, ofdFile, ofdDirectory } from './ofd.js';

/**
 * @typedef {import('./types.js').OffsClientConfig} OffsClientConfig
 * @typedef {import('./types.js').OffsPutOptions} OffsPutOptions
 * @typedef {import('./types.js').OffsGetCallbacks} OffsGetCallbacks
 */

/**
 * @typedef {Object} PendingRequest
 * @property {number} id
 * @property {number} type
 * @property {(value: any) => void} resolve
 * @property {(reason: any) => void} reject
 * @property {any} [ctx]
 */

/**
 * Default configuration.
 * @returns {OffsClientConfig}
 */
function defaultConfig() {
  return {
    connectTimeoutMs: 5000,
    requestTimeoutMs: 30000
  };
}

/**
 * Create a transport by URL scheme.
 * @param {string} url
 * @param {string} [apiKey]
 * @param {any} [options]
 * @returns {HttpTransport|WsTransport|WtTransport}
 */
function createTransport(url, apiKey, options) {
  if (url.startsWith('ws://') || url.startsWith('wss://')) {
    return new WsTransport(url, apiKey, options);
  }
  if (url.startsWith('wt://') || url.startsWith('wts://')) {
    return new WtTransport(url, apiKey, options);
  }
  return new HttpTransport(url, apiKey, options);
}

/**
 * Browser-only OFFS client supporting HTTP, WebSocket, and WebTransport.
 */
export class OffsClient {
  /** @type {string} */
  url;
  /** @type {string|undefined} */
  apiKey;
  /** @type {OffsClientConfig} */
  config;
  /** @type {HttpTransport|WsTransport|WtTransport} */
  transport;
  /** @type {Map<number, PendingRequest>} */
  pending = new Map();
  /** @type {{type: number, bytes: Uint8Array}[]} */
  inboundQueue = [];
  /** @type {number} */
  nextRequestId = 1;
  /** @type {boolean} */
  streamingPut = false;
  /** @type {OffsPutOptions|null} */
  streamOptions = null;
  /** @type {boolean} */
  connected = false;

  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {OffsClientConfig & {transport?: any}} [config]
   */
  constructor(url, apiKey, config) {
    this.url = url;
    this.apiKey = apiKey;
    this.config = { ...defaultConfig(), ...config };
    this.transport = config?.transport || createTransport(url, apiKey, config);
    this.transport.setMessageHandler(this._onMessage.bind(this));
  }

  /**
   * @returns {Promise<void>}
   */
  async connect() {
    await this.transport.connect();
    this.connected = true;
  }

  disconnect() {
    this.transport.disconnect();
    this.connected = false;
    for (const pending of this.pending.values()) {
      pending.reject(new Error('Client disconnected'));
    }
    this.pending.clear();
  }

  isConnected() {
    return this.transport.isConnected();
  }

  /**
   * @param {number} id
   * @param {number} type
   * @param {number} [timeoutMs]
   * @returns {Promise<any>}
   */
  _request(id, type, timeoutMs) {
    return new Promise((resolve, reject) => {
      const pending = {
        id,
        type,
        resolve,
        reject,
        timer: setTimeout(() => {
          this.pending.delete(id);
          reject(new Error('Request timeout'));
        }, timeoutMs || this.config.requestTimeoutMs)
      };
      this.pending.set(id, pending);
    });
  }

  /**
   * @param {number|number[]} type
   * @param {number} [timeoutMs]
   * @returns {Promise<Uint8Array>}
   */
  _waitForResponse(type, timeoutMs) {
    const id = this.nextRequestId++;
    const promise = this._request(id, type, timeoutMs);
    const queued = this._dequeueMatching(type);
    if (queued !== null) {
      this._resolve(id, queued);
    }
    return promise;
  }

  /**
   * @param {number} id
   * @param {any} value
   */
  _resolve(id, value) {
    const pending = this.pending.get(id);
    if (!pending) return;
    if (pending.timer) clearTimeout(pending.timer);
    this.pending.delete(id);
    pending.resolve(value);
  }

  /**
   * @param {number} id
   * @param {any} reason
   */
  _reject(id, reason) {
    const pending = this.pending.get(id);
    if (!pending) return;
    if (pending.timer) clearTimeout(pending.timer);
    this.pending.delete(id);
    pending.reject(reason);
  }

  /**
   * @param {number} type
   * @param {Uint8Array} bytes
   */
  _onMessage(type, bytes) {
    if (type === wire.MSG.ERROR) {
      const error = wire.decodeError(bytes);
      if (error) {
        for (const pending of this.pending.values()) {
          this._reject(pending.id, new Error(`Server error ${error.statusCode}: ${error.message}`));
        }
      }
      return;
    }

    for (const pending of this.pending.values()) {
      const matches = Array.isArray(pending.type)
        ? pending.type.includes(type)
        : pending.type === type;
      if (matches) {
        this._resolve(pending.id, bytes);
        return;
      }
    }
    this.inboundQueue.push({ type, bytes });
  }

  /**
   * @param {number|number[]} type
   * @returns {Uint8Array|null}
   */
  _dequeueMatching(type) {
    const types = Array.isArray(type) ? type : [type];
    const index = this.inboundQueue.findIndex((item) => types.includes(item.type));
    if (index === -1) return null;
    const item = this.inboundQueue[index];
    this.inboundQueue.splice(index, 1);
    return item.bytes;
  }

  /**
   * Send a CBOR message and wait for a matching response type.
   * @param {Uint8Array} bytes
   * @param {number} responseType
   * @param {number} [timeoutMs]
   * @returns {Promise<Uint8Array>}
   */
  async _sendAndWait(bytes, responseType, timeoutMs) {
    const id = this.nextRequestId++;
    const promise = this._request(id, responseType, timeoutMs);
    await this.transport.send(bytes);
    return promise;
  }

  /**
   * @param {string|OffsPutOptions} options
   * @param {Uint8Array|undefined} data
   * @returns {Promise<{oriString: string}>}
   */
  async put(options, data) {
    if (typeof options === 'string') {
      throw new Error('Use object options (contentType, fileName, streamLength)');
    }

    const safeOptions = {
      ...options,
      fileName: basename(options.fileName)
    };

    if (this.transport instanceof HttpTransport) {
      const body = data || new Uint8Array(0);
      return this.transport.put(safeOptions, body);
    }

    const requestBytes = wire.encodePutRequest(safeOptions, data);

    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.PUT_RESPONSE);
    return wire.decodePutResponse(responseBytes);
  }

  /**
   * @param {OffsPutOptions} options
   * @returns {Promise<void>}
   */
  async putStreamStart(options) {
    this.streamingPut = true;
    this.streamOptions = options;

    if (this.transport instanceof HttpTransport) {
      return;
    }

    const requestBytes = wire.encodePutRequest(options);
    await this.transport.send(requestBytes);
  }

  /**
   * @param {Uint8Array} chunk
   * @returns {Promise<void>}
   */
  async putStreamData(chunk) {
    if (this.transport instanceof HttpTransport) {
      throw new Error('HTTP transport does not support putStreamData; use put with ReadableStream');
    }
    await this.transport.send(wire.encodePutData(chunk));
  }

  /**
   * @returns {Promise<{oriString: string}>}
   */
  async putStreamEnd() {
    this.streamingPut = false;
    const options = this.streamOptions;
    this.streamOptions = null;

    if (this.transport instanceof HttpTransport) {
      if (!options) throw new Error('No stream in progress');
      return this.transport.put(options, new Uint8Array(0));
    }

    await this.transport.send(wire.encodePutEnd());
    const responseBytes = await this._request(this.nextRequestId - 1, wire.MSG.PUT_RESPONSE);
    return wire.decodePutResponse(responseBytes);
  }

  /**
   * @param {string} oriString
   * @param {OffsGetCallbacks} callbacks
   * @param {{start?: number, end?: number}} [range]
   */
  async get(oriString, callbacks, range) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.get(oriString, callbacks);
    }

    const requestBytes = wire.encodeGetRequest(oriString, range);

    const startBytes = await this._sendAndWait(requestBytes, wire.MSG.GET_RESPONSE_START);
    const start = wire.decodeGetResponseStart(startBytes);
    callbacks.onStart?.(start.contentType, start.contentLength, start.hasRange, start.rangeStart, start.rangeEnd);

    while (true) {
      const dataBytes = await this._waitForResponse([wire.MSG.GET_DATA, wire.MSG.GET_END]);
      if (wire.isGetEnd(dataBytes)) break;
      const chunk = wire.decodeGetData(dataBytes);
      callbacks.onData(chunk);
    }

    callbacks.onEnd?.();
  }

  /**
   * @param {Uint8Array} data
   * @param {number} [encoding=0]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(data, encoding = 0) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.blockPut(data, encoding);
    }

    const requestBytes = wire.encodeBlockPutRequest(data, encoding);
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.BLOCK_PUT_RESPONSE);
    return wire.decodeBlockPutResponse(responseBytes);
  }

  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number, data: Uint8Array}>}
   */
  async blockGet(hash) {
    if (typeof hash === 'string') return this.transport.blockGet(hash);

    if (this.transport instanceof HttpTransport) {
      return this.transport.blockGet(base58Encode(hash));
    }

    const requestBytes = wire.encodeBlockGetRequest(hash);
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.BLOCK_GET_RESPONSE);
    return wire.decodeBlockGetResponse(responseBytes);
  }

  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(hash) {
    if (typeof hash === 'string') return this.transport.blockDelete(hash);

    if (this.transport instanceof HttpTransport) {
      return this.transport.blockDelete(base58Encode(hash));
    }

    const requestBytes = wire.encodeBlockDeleteRequest(hash);
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.BLOCK_DELETE_RESPONSE);
    return wire.decodeBlockDeleteResponse(responseBytes);
  }

  /**
   * @returns {Promise<any>}
   */
  async health() {
    if (this.transport instanceof HttpTransport) {
      return this.transport.health();
    }

    const requestBytes = wire.encodeHealthRequest();
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.HEALTH_RESPONSE);
    const { json } = wire.decodeHealthResponse(responseBytes);
    return JSON.parse(json);
  }

  /**
   * @param {string} [format='cbor']
   * @returns {Promise<{format: number, data: Uint8Array}>}
   */
  async peerInfo(format = 'cbor') {
    if (this.transport instanceof HttpTransport) {
      return this.transport.peerInfo(format);
    }

    const requestBytes = wire.encodePeerInfoRequest();
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.PEER_INFO_RESPONSE);
    return wire.decodePeerInfoResponse(responseBytes);
  }

  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(peerInfo, format = 0) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.peerConnect(peerInfo, format);
    }

    const requestBytes = wire.encodePeerConnect(format, peerInfo);
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.PEER_CONNECT_RESULT);
    return wire.decodePeerConnectResult(responseBytes);
  }

  /**
   * Convert an OFF URL/URI string into an HTTP URL usable by a browser.
   * @param {string} oriString
   * @param {string} [baseUrl]
   * @returns {string}
   */
  static offUrlToHttpUrl(oriString, baseUrl) {
    return offUrlToHttpUrl(oriString, baseUrl);
  }

  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    if (this.transport instanceof HttpTransport) {
      return this.transport.peerList();
    }

    const requestBytes = wire.encodePeerListRequest();
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.PEER_LIST_RESPONSE);
    return wire.decodePeerListResponse(responseBytes);
  }

  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(peerInfo, format = 0) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.friendAdd(peerInfo, format);
    }

    const requestBytes = wire.encodeFriendAdd(format, peerInfo);
    await this.transport.send(requestBytes);
  }

  /**
   * @param {string|Uint8Array} nodeId
   * @returns {Promise<void>}
   */
  async friendRemove(nodeId) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.friendRemove(typeof nodeId === 'string' ? nodeId : base58Encode(nodeId));
    }

    const idBytes = typeof nodeId === 'string' ? new TextEncoder().encode(nodeId) : nodeId;
    const requestBytes = wire.encodeFriendRemove(idBytes);
    await this.transport.send(requestBytes);
  }

  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    if (this.transport instanceof HttpTransport) {
      return this.transport.friendList();
    }

    const requestBytes = wire.encodeFriendListRequest();
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.FRIEND_LIST_RESPONSE);
    return wire.decodeFriendListResponse(responseBytes);
  }

  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    if (this.transport instanceof HttpTransport) {
      return this.transport.configShow();
    }

    const requestBytes = wire.encodeConfigShowRequest();
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.CONFIG_SHOW_RESPONSE);
    const { json } = wire.decodeConfigShowResponse(responseBytes);
    return JSON.parse(json);
  }

  /**
   * @param {string} field
   * @param {string} value
   * @returns {Promise<{status: number, restartRequired: boolean, message: string}>}
   */
  async configSet(field, value) {
    if (this.transport instanceof HttpTransport) {
      return this.transport.configSet(field, value);
    }

    const requestBytes = wire.encodeConfigSetRequest(field, value);
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.CONFIG_SET_RESPONSE);
    return wire.decodeConfigSetResponse(responseBytes);
  }

  /**
   * @returns {Promise<{status: number, message: string}>}
   */
  async configReload() {
    if (this.transport instanceof HttpTransport) {
      return this.transport.configReload();
    }

    const requestBytes = wire.encodeConfigReloadRequest();
    const responseBytes = await this._sendAndWait(requestBytes, wire.MSG.CONFIG_RELOAD_RESPONSE);
    return wire.decodeConfigReloadResponse(responseBytes);
  }

  /**
   * Upload a folder recursively and return the root directory's ORI URL.
   * Matches the algorithm used by the Flutter example client in
   * examples/off_client/lib/screens/import_screen.dart.
   *
   * @param {FileList|File[]|import('./util.js').FolderEntry[]|Record<string, File|Blob>} items
   * @param {Object} [options]
   * @param {string[]} [options.recyclerUrls]
   * @param {string} [options.serverAddress]
   * @param {boolean} [options.temporary=false]
   * @param {(name: string, uploaded: number, total: number) => void} [options.onProgress]
   * @returns {Promise<{oriString: string}>}
   */
  async putFolder(items, options = {}) {
    const entries = normalizeFolderEntries(items);
    if (entries.length === 0) {
      throw new Error('No files to upload');
    }

    const recyclerUrls = options.recyclerUrls || [];
    const totalFiles = entries.length;
    let uploadedCount = 0;

    const updateProgress = (name) => {
      uploadedCount++;
      options.onProgress?.(name, uploadedCount, totalFiles);
    };

    const rootDir = _commonDirectory(entries.map((entry) => entry.path));

    /**
     * @param {string} dirPath
     * @returns {Promise<{oriString: string}>}
     */
    const uploadDirectory = async (dirPath) => {
      const dirName = basename(dirPath ? dirPath : rootDir || 'root');
      const childEntries = _children(entries, dirPath);
      const fileEntries = childEntries;
      const subdirs = _childDirectories(entries, dirPath);

      /** @type {import('./ofd.js').OfdEntry[]} */
      const ofdEntries = [];

      // Recursively upload subdirectories first.
      for (const subdir of subdirs) {
        const subResult = await uploadDirectory(subdir);
        const subUrl = subResult.oriString;
        const parsed = parseOffUrl(subUrl);
        if (!parsed) {
          throw new Error(`Failed to parse subdirectory URL: ${subUrl}`);
        }
        const dirHash = base58Decode(parsed.fileHashB58);
        if (!dirHash) {
          throw new Error(`Invalid directory hash in URL: ${subUrl}`);
        }
        ofdEntries.push(ofdDirectory({
          name: basename(subdir),
          dirHash
        }));
      }

      // Upload files in this directory.
      for (const fileEntry of fileEntries) {
        const fileName = basename(fileEntry.path);
        const contentType = mimeFromExtension(fileName);
        const streamLength = fileEntry.file.size;

        let url;
        if (this.transport instanceof HttpTransport) {
          const body = fileToReadableStream(fileEntry.file);
          const result = await this.put({
            contentType,
            fileName,
            streamLength,
            serverAddress: options.serverAddress,
            recyclerUrls,
            temporary: options.temporary
          }, body);
          url = result.oriString;
        } else {
          await this.putStreamStart({
            contentType,
            fileName,
            streamLength,
            serverAddress: options.serverAddress,
            recyclerUrls,
            temporary: options.temporary
          });

          const reader = fileToReadableStream(fileEntry.file).getReader();
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            await this.putStreamData(value);
          }

          const result = await this.putStreamEnd();
          url = result.oriString;
        }

        const parsed = parseOffUrl(url);
        if (!parsed) {
          throw new Error(`Failed to parse file URL: ${url}`);
        }
        const fileHash = base58Decode(parsed.fileHashB58);
        const descriptorHash = base58Decode(parsed.descriptorHashB58);
        if (!fileHash || !descriptorHash) {
          throw new Error(`Invalid hash in file URL: ${url}`);
        }

        ofdEntries.push(ofdFile({
          name: fileName,
          fileHash,
          descriptorHash,
          finalByte: parsed.streamLength
        }));

        updateProgress(fileName);
      }

      if (ofdEntries.length === 0) {
        throw new Error(`Empty directory: ${dirPath || rootDir}`);
      }

      const ofdBytes = buildOfdCbor(ofdEntries);
      const dirFileName = `${dirName}.ofd`;

      if (this.transport instanceof HttpTransport) {
        return this.put({
          contentType: 'offsystem/directory',
          fileName: dirFileName,
          streamLength: ofdBytes.length,
          serverAddress: options.serverAddress,
          recyclerUrls,
          temporary: options.temporary
        }, ofdBytes);
      }

      await this.putStreamStart({
        contentType: 'offsystem/directory',
        fileName: dirFileName,
        streamLength: ofdBytes.length,
        serverAddress: options.serverAddress,
        recyclerUrls,
        temporary: options.temporary
      });
      await this.putStreamData(ofdBytes);
      return this.putStreamEnd();
    };

    return uploadDirectory(rootDir);
  }
}

/**
 * Find the common root directory for a set of file paths.
 * @param {string[]} paths
 * @returns {string}
 */
function _commonDirectory(paths) {
  if (paths.length === 0) return '';
  const segments = paths.map((path) => path.split('/').filter(Boolean));
  const first = segments[0];
  let commonLength = first.length;
  for (let index = 1; index < segments.length; index++) {
    const other = segments[index];
    let match = 0;
    while (match < Math.min(commonLength, other.length) && first[match] === other[match]) {
      match++;
    }
    commonLength = match;
    if (commonLength === 0) break;
  }
  // The common prefix must end at a directory boundary, not inside a filename.
  const prefixLength = Math.min(commonLength, first.length - 1);
  return first.slice(0, prefixLength).join('/');
}

/**
 * Get direct child files of a directory path.
 * @param {import('./util.js').FolderEntry[]} entries
 * @param {string} dirPath
 * @returns {import('./util.js').FolderEntry[]}
 */
function _children(entries, dirPath) {
  const prefix = dirPath ? `${dirPath}/` : '';
  return entries.filter((entry) => {
    if (!entry.path.startsWith(prefix)) return false;
    const rest = entry.path.slice(prefix.length);
    return rest.length > 0 && !rest.includes('/');
  });
}

/**
 * Get direct child directory paths of a directory path.
 * @param {import('./util.js').FolderEntry[]} entries
 * @param {string} dirPath
 * @returns {string[]}
 */
function _childDirectories(entries, dirPath) {
  const prefix = dirPath ? `${dirPath}/` : '';
  const seen = new Set();
  for (const entry of entries) {
    if (!entry.path.startsWith(prefix)) continue;
    const rest = entry.path.slice(prefix.length);
    if (!rest) continue;
    const slashIndex = rest.indexOf('/');
    if (slashIndex > 0) {
      seen.add(prefix + rest.slice(0, slashIndex));
    }
  }
  return Array.from(seen);
}

export { wire, base58Decode, base58Encode, parseOffUrl, offUrlToHttpUrl, mimeFromExtension };
