
/**
 * HTTP REST transport for the OFFS client.
 * Maps wire messages to the HTTP routes in src/ClientAPI/HTTP/.
 */
export class HttpTransport {
  /** @type {string} */
  baseUrl;
  /** @type {string|undefined} */
  apiKey;
  /** @type {AbortController|null} */
  abortController = null;

  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(url, apiKey, _options) {
    this.baseUrl = url.replace(/\/$/, '');
    this.apiKey = apiKey;
  }

  /**
   * @returns {Promise<void>}
   */
  async connect() {
    this.abortController = new AbortController();
  }

  disconnect() {
    if (this.abortController) {
      this.abortController.abort();
      this.abortController = null;
    }
  }

  isConnected() {
    return this.abortController !== null;
  }

  /**
   * @param {string} path
   * @returns {string}
   */
  url(path) {
    return `${this.baseUrl}${path}`;
  }

  /**
   * @returns {Record<string, string>}
   */
  authHeaders() {
    const headers = {};
    if (this.apiKey) {
      headers['Authorization'] = `Bearer ${this.apiKey}`;
    }
    return headers;
  }

  /**
   * @param {(type: number, bytes: Uint8Array) => void} _handler
   */
  setMessageHandler(_handler) {
    // HTTP is request/response; no async messages.
  }

  /**
   * Send raw bytes — not used directly for HTTP; use the typed methods.
   * @param {Uint8Array} _bytes
   */
  send(_bytes) {
    throw new Error('HttpTransport does not support raw send; use OffsClient methods');
  }

  /**
   * Upload a file to PUT /offsystem.
   * @param {import('../types.js').OffsPutOptions} options
   * @param {ReadableStream<Uint8Array>|Uint8Array} body
   * @returns {Promise<{oriString: string}>}
   */
  async put(options, body) {
    const headers = {
      ...this.authHeaders(),
      'type': options.contentType,
      'file-name': options.fileName,
      'stream-length': String(options.streamLength),
    };
    if (options.serverAddress) headers['server-address'] = options.serverAddress;
    if (options.recyclerUrls?.length) headers['recycler'] = JSON.stringify(options.recyclerUrls);
    if (options.temporary) headers['temporary'] = 'true';
    if (options.tupleSize !== undefined) headers['tuple-size'] = String(options.tupleSize);

    const isStream = body && typeof body.getReader === 'function';
    const response = await fetch(this.url('/offsystem'), {
      method: 'PUT',
      headers,
      body,
      signal: this.abortController?.signal,
      ...(isStream ? { duplex: 'half' } : {})
    });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`Upload failed: ${response.status} ${text}`);
    }
    const oriString = await response.text();
    return { oriString };
  }

  /**
   * Download from GET /offsystem/v3/...
   * @param {string} offUrl
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   */
  async get(offUrl, callbacks) {
    const response = await fetch(offUrl, {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) {
      const text = await response.text();
      callbacks.onError?.(response.status, text);
      return;
    }

    const contentType = response.headers.get('content-type') || 'application/octet-stream';
    const contentLength = parseInt(response.headers.get('content-length') || '0', 10);
    const hasRange = response.status === 206;
    const rangeHeader = response.headers.get('content-range');
    let rangeStart, rangeEnd;
    if (rangeHeader) {
      const match = rangeHeader.match(/bytes (\d+)-(\d+)\//);
      if (match) {
        rangeStart = parseInt(match[1], 10);
        rangeEnd = parseInt(match[2], 10);
      }
    }
    callbacks.onStart?.(contentType, contentLength, hasRange, rangeStart, rangeEnd);

    const reader = response.body?.getReader();
    if (!reader) {
      callbacks.onEnd?.();
      return;
    }

    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        if (value) callbacks.onData(value);
      }
      callbacks.onEnd?.();
    } catch (err) {
      callbacks.onError?.(0, String(err));
    }
  }

  /**
   * Delete content.
   * @param {string} offUrl
   * @returns {Promise<void>}
   */
  async delete(offUrl) {
    const response = await fetch(offUrl, {
      method: 'DELETE',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`Delete failed: ${response.status} ${text}`);
    }
  }

  /**
   * @param {Uint8Array} data
   * @param {number} [encoding]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(data, encoding = 0) {
    const query = encoding === 1 ? '?encoding=base58' : '';
    const response = await fetch(this.url(`/blocks${query}`), {
      method: 'PUT',
      headers: { ...this.authHeaders(), 'Content-Type': 'application/octet-stream' },
      body: data,
      signal: this.abortController?.signal,
    });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`Block put failed: ${response.status} ${text}`);
    }
    const hash = await response.arrayBuffer();
    return { status: 0, hash: new Uint8Array(hash) };
  }

  /**
   * @param {string} base58Hash
   * @returns {Promise<{status: number, data: Uint8Array}>}
   */
  async blockGet(base58Hash) {
    const response = await fetch(this.url(`/blocks/${base58Hash}`), {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) {
      return { status: 2, data: new Uint8Array(0) }; // NOT_FOUND
    }
    const data = await response.arrayBuffer();
    return { status: 0, data: new Uint8Array(data) };
  }

  /**
   * @param {string} base58Hash
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(base58Hash) {
    const response = await fetch(this.url(`/blocks/${base58Hash}`), {
      method: 'DELETE',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    return { status: response.ok ? 0 : 2 };
  }

  /**
   * @returns {Promise<any>}
   */
  async health() {
    const response = await fetch(this.url('/health'), {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) {
      throw new Error(`Health check failed: ${response.status}`);
    }
    return response.json();
  }

  /**
   * @param {string} [format='cbor']
   * @returns {Promise<{format: number, data: Uint8Array}>}
   */
  async peerInfo(format = 'cbor') {
    const fmt = format === 'base58' ? 1 : 0;
    const response = await fetch(this.url(`/peer/info?format=${format}`), {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Peer info failed: ${response.status}`);
    const data = await response.arrayBuffer();
    return { format: fmt, data: new Uint8Array(data) };
  }

  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(peerInfo, format = 0) {
    const response = await fetch(this.url('/peer/connect'), {
      method: 'POST',
      headers: { ...this.authHeaders(), 'Content-Type': format === 1 ? 'text/plain' : 'application/cbor' },
      body: format === 1 ? new TextDecoder().decode(peerInfo) : peerInfo,
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Peer connect failed: ${response.status}`);
    return { status: 0 };
  }

  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    const response = await fetch(this.url('/peers'), {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Peer list failed: ${response.status}`);
    return response.json();
  }

  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(peerInfo, format = 0) {
    const response = await fetch(this.url('/friends'), {
      method: 'POST',
      headers: { ...this.authHeaders(), 'Content-Type': format === 1 ? 'text/plain' : 'application/cbor' },
      body: format === 1 ? new TextDecoder().decode(peerInfo) : peerInfo,
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Friend add failed: ${response.status}`);
  }

  /**
   * @param {string} nodeId
   * @returns {Promise<void>}
   */
  async friendRemove(nodeId) {
    const response = await fetch(this.url(`/friends/${nodeId}`), {
      method: 'DELETE',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Friend remove failed: ${response.status}`);
  }

  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    const response = await fetch(this.url('/friends'), {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Friend list failed: ${response.status}`);
    return response.json();
  }

  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    const response = await fetch(this.url('/config'), {
      method: 'GET',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Config show failed: ${response.status}`);
    return response.json();
  }

  /**
   * @param {string} field
   * @param {string} value
   * @returns {Promise<{staged: any, rejected: any, restart_required: boolean}>}
   */
  async configSet(field, value) {
    const response = await fetch(this.url('/config'), {
      method: 'PUT',
      headers: { ...this.authHeaders(), 'Content-Type': 'application/json' },
      body: JSON.stringify({ [field]: value }),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Config set failed: ${response.status}`);
    return response.json();
  }

  /**
   * @returns {Promise<void>}
   */
  async configReload() {
    const response = await fetch(this.url('/config/restart'), {
      method: 'POST',
      headers: this.authHeaders(),
      signal: this.abortController?.signal,
    });
    if (!response.ok) throw new Error(`Config reload failed: ${response.status}`);
  }
}
