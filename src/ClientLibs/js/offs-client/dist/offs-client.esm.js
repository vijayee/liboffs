var er = Object.defineProperty;
var tr = (t, e, r) => e in t ? er(t, e, { enumerable: !0, configurable: !0, writable: !0, value: r }) : t[e] = r;
var B = (t, e, r) => tr(t, typeof e != "symbol" ? e + "" : e, r);
class D {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {string} */
    B(this, "baseUrl");
    /** @type {string|undefined} */
    B(this, "apiKey");
    /** @type {AbortController|null} */
    B(this, "abortController", null);
    this.baseUrl = e.replace(/\/$/, ""), this.apiKey = r;
  }
  /**
   * @returns {Promise<void>}
   */
  async connect() {
    this.abortController = new AbortController();
  }
  disconnect() {
    this.abortController && (this.abortController.abort(), this.abortController = null);
  }
  isConnected() {
    return this.abortController !== null;
  }
  /**
   * @param {string} path
   * @returns {string}
   */
  url(e) {
    return `${this.baseUrl}${e}`;
  }
  /**
   * @returns {Record<string, string>}
   */
  authHeaders() {
    const e = {};
    return this.apiKey && (e.Authorization = `Bearer ${this.apiKey}`), e;
  }
  /**
   * @param {(type: number, bytes: Uint8Array) => void} _handler
   */
  setMessageHandler(e) {
  }
  /**
   * Send raw bytes — not used directly for HTTP; use the typed methods.
   * @param {Uint8Array} _bytes
   */
  send(e) {
    throw new Error("HttpTransport does not support raw send; use OffsClient methods");
  }
  /**
   * Upload a file to PUT /offsystem.
   * @param {import('../types.js').OffsPutOptions} options
   * @param {ReadableStream<Uint8Array>|Uint8Array} body
   * @returns {Promise<{oriString: string}>}
   */
  async put(e, r) {
    var p, y;
    const n = {
      ...this.authHeaders(),
      type: e.contentType,
      "file-name": e.fileName,
      "stream-length": String(e.streamLength)
    };
    e.serverAddress && (n["server-address"] = e.serverAddress), (p = e.recyclerUrls) != null && p.length && (n.recycler = JSON.stringify(e.recyclerUrls)), e.temporary && (n.temporary = "true"), e.tupleSize !== void 0 && (n["tuple-size"] = String(e.tupleSize));
    let s = r;
    r && typeof r.getReader == "function" && (s = await this._readStream(r));
    const o = await fetch(this.url("/offsystem"), {
      method: "PUT",
      headers: n,
      body: s,
      signal: (y = this.abortController) == null ? void 0 : y.signal
    });
    if (!o.ok) {
      const _ = await o.text();
      throw new Error(`Upload failed: ${o.status} ${_}`);
    }
    return { oriString: await o.text() };
  }
  /**
   * Read a ReadableStream into a Uint8Array.
   * The OFFS HTTP server is HTTP/1.1, so request streaming via duplex: 'half'
   * causes ERR_ALPN_NEGOTIATION_FAILED. Buffering the body avoids that.
   * @param {ReadableStream<Uint8Array>} stream
   * @returns {Promise<Uint8Array>}
   */
  async _readStream(e) {
    const r = e.getReader(), n = [];
    let s = 0;
    for (; ; ) {
      const { done: p, value: y } = await r.read();
      if (p) break;
      n.push(y), s += y.length;
    }
    const o = new Uint8Array(s);
    let l = 0;
    for (const p of n)
      o.set(p, l), l += p.length;
    return o;
  }
  /**
   * Download from GET /offsystem/v3/...
   * @param {string} offUrl
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   */
  async get(e, r) {
    var P, L, G, V, I, q, re;
    const n = await fetch(e, {
      method: "GET",
      headers: this.authHeaders(),
      signal: (P = this.abortController) == null ? void 0 : P.signal
    });
    if (!n.ok) {
      const C = await n.text();
      (L = r.onError) == null || L.call(r, n.status, C);
      return;
    }
    const s = n.headers.get("content-type") || "application/octet-stream", o = parseInt(n.headers.get("content-length") || "0", 10), l = n.status === 206, p = n.headers.get("content-range");
    let y, _;
    if (p) {
      const C = p.match(/bytes (\d+)-(\d+)\//);
      C && (y = parseInt(C[1], 10), _ = parseInt(C[2], 10));
    }
    (G = r.onStart) == null || G.call(r, s, o, l, y, _);
    const g = (V = n.body) == null ? void 0 : V.getReader();
    if (!g) {
      (I = r.onEnd) == null || I.call(r);
      return;
    }
    try {
      for (; ; ) {
        const { done: C, value: S } = await g.read();
        if (C) break;
        S && r.onData(S);
      }
      (q = r.onEnd) == null || q.call(r);
    } catch (C) {
      (re = r.onError) == null || re.call(r, 0, String(C));
    }
  }
  /**
   * Delete content.
   * @param {string} offUrl
   * @returns {Promise<void>}
   */
  async delete(e) {
    var n;
    const r = await fetch(e, {
      method: "DELETE",
      headers: this.authHeaders(),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    });
    if (!r.ok) {
      const s = await r.text();
      throw new Error(`Delete failed: ${r.status} ${s}`);
    }
  }
  /**
   * @param {Uint8Array} data
   * @param {number} [encoding]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(e, r = 0) {
    var l;
    const n = r === 1 ? "?encoding=base58" : "", s = await fetch(this.url(`/blocks${n}`), {
      method: "PUT",
      headers: { ...this.authHeaders(), "Content-Type": "application/octet-stream" },
      body: e,
      signal: (l = this.abortController) == null ? void 0 : l.signal
    });
    if (!s.ok) {
      const p = await s.text();
      throw new Error(`Block put failed: ${s.status} ${p}`);
    }
    const o = await s.arrayBuffer();
    return { status: 0, hash: new Uint8Array(o) };
  }
  /**
   * @param {string} base58Hash
   * @returns {Promise<{status: number, data: Uint8Array}>}
   */
  async blockGet(e) {
    var s;
    const r = await fetch(this.url(`/blocks/${e}`), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (s = this.abortController) == null ? void 0 : s.signal
    });
    if (!r.ok)
      return { status: 2, data: new Uint8Array(0) };
    const n = await r.arrayBuffer();
    return { status: 0, data: new Uint8Array(n) };
  }
  /**
   * @param {string} base58Hash
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(e) {
    var n;
    return { status: (await fetch(this.url(`/blocks/${e}`), {
      method: "DELETE",
      headers: this.authHeaders(),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    })).ok ? 0 : 2 };
  }
  /**
   * @returns {Promise<any>}
   */
  async health() {
    var r;
    const e = await fetch(this.url("/health"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (r = this.abortController) == null ? void 0 : r.signal
    });
    if (!e.ok)
      throw new Error(`Health check failed: ${e.status}`);
    return e.json();
  }
  /**
   * @param {string} [format='cbor']
   * @returns {Promise<{format: number, data: Uint8Array}>}
   */
  async peerInfo(e = "cbor") {
    var o;
    const r = e === "base58" ? 1 : 0, n = await fetch(this.url(`/peer/info?format=${e}`), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (o = this.abortController) == null ? void 0 : o.signal
    });
    if (!n.ok) throw new Error(`Peer info failed: ${n.status}`);
    const s = await n.arrayBuffer();
    return { format: r, data: new Uint8Array(s) };
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(e, r = 0) {
    var s;
    const n = await fetch(this.url("/peer/connect"), {
      method: "POST",
      headers: { ...this.authHeaders(), "Content-Type": r === 1 ? "text/plain" : "application/cbor" },
      body: r === 1 ? new TextDecoder().decode(e) : e,
      signal: (s = this.abortController) == null ? void 0 : s.signal
    });
    if (!n.ok) throw new Error(`Peer connect failed: ${n.status}`);
    return { status: 0 };
  }
  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    var r;
    const e = await fetch(this.url("/peers"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (r = this.abortController) == null ? void 0 : r.signal
    });
    if (!e.ok) throw new Error(`Peer list failed: ${e.status}`);
    return e.json();
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(e, r = 0) {
    var s;
    const n = await fetch(this.url("/friends"), {
      method: "POST",
      headers: { ...this.authHeaders(), "Content-Type": r === 1 ? "text/plain" : "application/cbor" },
      body: r === 1 ? new TextDecoder().decode(e) : e,
      signal: (s = this.abortController) == null ? void 0 : s.signal
    });
    if (!n.ok) throw new Error(`Friend add failed: ${n.status}`);
  }
  /**
   * @param {string} nodeId
   * @returns {Promise<void>}
   */
  async friendRemove(e) {
    var n;
    const r = await fetch(this.url(`/friends/${e}`), {
      method: "DELETE",
      headers: this.authHeaders(),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    });
    if (!r.ok) throw new Error(`Friend remove failed: ${r.status}`);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    var r;
    const e = await fetch(this.url("/friends"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (r = this.abortController) == null ? void 0 : r.signal
    });
    if (!e.ok) throw new Error(`Friend list failed: ${e.status}`);
    return e.json();
  }
  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    var r;
    const e = await fetch(this.url("/config"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (r = this.abortController) == null ? void 0 : r.signal
    });
    if (!e.ok) throw new Error(`Config show failed: ${e.status}`);
    return e.json();
  }
  /**
   * @param {string} field
   * @param {string} value
   * @returns {Promise<{staged: any, rejected: any, restart_required: boolean}>}
   */
  async configSet(e, r) {
    var s;
    const n = await fetch(this.url("/config"), {
      method: "PUT",
      headers: { ...this.authHeaders(), "Content-Type": "application/json" },
      body: JSON.stringify({ [e]: r }),
      signal: (s = this.abortController) == null ? void 0 : s.signal
    });
    if (!n.ok) throw new Error(`Config set failed: ${n.status}`);
    return n.json();
  }
  /**
   * @returns {Promise<void>}
   */
  async configReload() {
    var r;
    const e = await fetch(this.url("/config/restart"), {
      method: "POST",
      headers: this.authHeaders(),
      signal: (r = this.abortController) == null ? void 0 : r.signal
    });
    if (!e.ok) throw new Error(`Config reload failed: ${e.status}`);
  }
}
let Le;
try {
  Le = new TextDecoder();
} catch {
}
let w, le, c = 0;
const rr = 105, nr = 57342, sr = 57343, rt = 57337, nt = 6, de = {};
let ye = 11281e4, ie = 1681e4, T = {}, k, Ue, Pe = 0, Ee = 0, H, Z, F = [], Me = [], z, W, we, st = {
  useRecords: !1,
  mapsAsObjects: !0
}, ge = !1, yt = 2;
try {
  new Function("");
} catch {
  yt = 1 / 0;
}
class me {
  constructor(e) {
    if (e && ((e.keyMap || e._keyMap) && !e.useRecords && (e.useRecords = !1, e.mapsAsObjects = !0), e.useRecords === !1 && e.mapsAsObjects === void 0 && (e.mapsAsObjects = !0), e.getStructures && (e.getShared = e.getStructures), e.getShared && !e.structures && ((e.structures = []).uninitialized = !0), e.keyMap)) {
      this.mapKey = /* @__PURE__ */ new Map();
      for (let [r, n] of Object.entries(e.keyMap)) this.mapKey.set(n, r);
    }
    Object.assign(this, e);
  }
  /*
  decodeKey(key) {
  	return this.keyMap
  		? Object.keys(this.keyMap)[Object.values(this.keyMap).indexOf(key)] || key
  		: key
  }
  */
  decodeKey(e) {
    return this.keyMap && this.mapKey.get(e) || e;
  }
  encodeKey(e) {
    return this.keyMap && this.keyMap.hasOwnProperty(e) ? this.keyMap[e] : e;
  }
  encodeKeys(e) {
    if (!this._keyMap) return e;
    let r = /* @__PURE__ */ new Map();
    for (let [n, s] of Object.entries(e)) r.set(this._keyMap.hasOwnProperty(n) ? this._keyMap[n] : n, s);
    return r;
  }
  decodeKeys(e) {
    if (!this._keyMap || e.constructor.name != "Map") return e;
    if (!this._mapKey) {
      this._mapKey = /* @__PURE__ */ new Map();
      for (let [n, s] of Object.entries(this._keyMap)) this._mapKey.set(s, n);
    }
    let r = {};
    return e.forEach((n, s) => r[Y(this._mapKey.has(s) ? this._mapKey.get(s) : s)] = n), r;
  }
  mapDecode(e, r) {
    let n = this.decode(e);
    if (this._keyMap)
      switch (n.constructor.name) {
        case "Array":
          return n.map((s) => this.decodeKeys(s));
      }
    return n;
  }
  decode(e, r) {
    if (w)
      return gt(() => (Ge(), this ? this.decode(e, r) : me.prototype.decode.call(st, e, r)));
    le = r > -1 ? r : e.length, c = 0, Ee = 0, Ue = null, H = null, w = e;
    try {
      W = e.dataView || (e.dataView = new DataView(e.buffer, e.byteOffset, e.byteLength));
    } catch (n) {
      throw w = null, e instanceof Uint8Array ? n : new Error("Source must be a Uint8Array or Buffer but was a " + (e && typeof e == "object" ? e.constructor.name : typeof e));
    }
    if (this instanceof me) {
      if (T = this, z = this.sharedValues && (this.pack ? new Array(this.maxPrivatePackedValues || 16).concat(this.sharedValues) : this.sharedValues), this.structures)
        return k = this.structures, Re();
      (!k || k.length > 0) && (k = []);
    } else
      T = st, (!k || k.length > 0) && (k = []), z = null;
    return Re();
  }
  decodeMultiple(e, r) {
    let n, s = 0;
    try {
      let o = e.length;
      ge = !0;
      let l = this ? this.decode(e, o) : Je.decode(e, o);
      if (r) {
        if (r(l) === !1)
          return;
        for (; c < o; )
          if (s = c, r(Re()) === !1)
            return;
      } else {
        for (n = [l]; c < o; )
          s = c, n.push(Re());
        return n;
      }
    } catch (o) {
      throw o.lastPosition = s, o.values = n, o;
    } finally {
      ge = !1, Ge();
    }
  }
}
function Re() {
  try {
    let t = A();
    if (H) {
      if (c >= H.postBundlePosition) {
        let e = new Error("Unexpected bundle position");
        throw e.incomplete = !0, e;
      }
      c = H.postBundlePosition, H = null;
    }
    if (c == le)
      k = null, w = null, Z && (Z = null);
    else if (c > le) {
      let e = new Error("Unexpected end of CBOR data");
      throw e.incomplete = !0, e;
    } else if (!ge)
      throw new Error("Data read, but end of buffer not reached");
    return t;
  } catch (t) {
    throw Ge(), (t instanceof RangeError || t.message.startsWith("Unexpected end of buffer")) && (t.incomplete = !0), t;
  }
}
function A() {
  let t = w[c++], e = t >> 5;
  if (t = t & 31, t > 23)
    switch (t) {
      case 24:
        t = w[c++];
        break;
      case 25:
        if (e == 7)
          return fr();
        t = W.getUint16(c), c += 2;
        break;
      case 26:
        if (e == 7) {
          let r = W.getFloat32(c);
          if (T.useFloat32 > 2) {
            let n = Qe[(w[c] & 127) << 1 | w[c + 1] >> 7];
            return c += 4, (n * r + (r > 0 ? 0.5 : -0.5) >> 0) / n;
          }
          return c += 4, r;
        }
        if (t = W.getUint32(c), c += 4, e === 1) return -1 - t;
        break;
      case 27:
        if (e == 7) {
          let r = W.getFloat64(c);
          return c += 8, r;
        }
        if (e > 1) {
          if (W.getUint32(c) > 0)
            throw new Error("JavaScript does not support arrays, maps, or strings with length over 4294967295");
          t = W.getUint32(c + 4);
        } else T.int64AsNumber ? (t = W.getUint32(c) * 4294967296, t += W.getUint32(c + 4)) : t = W.getBigUint64(c);
        c += 8;
        break;
      case 31:
        switch (e) {
          case 2:
          case 3:
            throw new Error("Indefinite length not supported for byte or text strings");
          case 4:
            let r = [], n, s = 0;
            for (; (n = A()) != de; ) {
              if (s >= ye) throw new Error(`Array length exceeds ${ye}`);
              r[s++] = n;
            }
            return e == 4 ? r : e == 3 ? r.join("") : Buffer.concat(r);
          case 5:
            let o;
            if (T.mapsAsObjects) {
              let l = {}, p = 0;
              if (T.keyMap)
                for (; (o = A()) != de; ) {
                  if (p++ >= ie) throw new Error(`Property count exceeds ${ie}`);
                  l[Y(T.decodeKey(o))] = A();
                }
              else
                for (; (o = A()) != de; ) {
                  if (p++ >= ie) throw new Error(`Property count exceeds ${ie}`);
                  l[Y(o)] = A();
                }
              return l;
            } else {
              we && (T.mapsAsObjects = !0, we = !1);
              let l = /* @__PURE__ */ new Map();
              if (T.keyMap) {
                let p = 0;
                for (; (o = A()) != de; ) {
                  if (p++ >= ie)
                    throw new Error(`Map size exceeds ${ie}`);
                  l.set(T.decodeKey(o), A());
                }
              } else {
                let p = 0;
                for (; (o = A()) != de; ) {
                  if (p++ >= ie)
                    throw new Error(`Map size exceeds ${ie}`);
                  l.set(o, A());
                }
              }
              return l;
            }
          case 7:
            return de;
          default:
            throw new Error("Invalid major type for indefinite length " + e);
        }
      default:
        throw new Error("Unknown token " + t);
    }
  switch (e) {
    case 0:
      return t;
    case 1:
      return ~t;
    case 2:
      return ar(t);
    case 3:
      if (Ee >= c)
        return Ue.slice(c - Pe, (c += t) - Pe);
      if (Ee == 0 && le < 140 && t < 32) {
        let s = t < 16 ? wt(t) : or(t);
        if (s != null)
          return s;
      }
      return ir(t);
    case 4:
      if (t >= ye) throw new Error(`Array length exceeds ${ye}`);
      let r = new Array(t);
      for (let s = 0; s < t; s++) r[s] = A();
      return r;
    case 5:
      if (t >= ie) throw new Error(`Map size exceeds ${ye}`);
      if (T.mapsAsObjects) {
        let s = {};
        if (T.keyMap) for (let o = 0; o < t; o++) s[Y(T.decodeKey(A()))] = A();
        else for (let o = 0; o < t; o++) s[Y(A())] = A();
        return s;
      } else {
        we && (T.mapsAsObjects = !0, we = !1);
        let s = /* @__PURE__ */ new Map();
        if (T.keyMap) for (let o = 0; o < t; o++) s.set(T.decodeKey(A()), A());
        else for (let o = 0; o < t; o++) s.set(A(), A());
        return s;
      }
    case 6:
      if (t >= rt) {
        let s = k[t & 8191];
        if (s)
          return s.read || (s.read = He(s)), s.read();
        if (t < 65536) {
          if (t == sr) {
            let o = pe(), l = A(), p = A();
            Ke(l, p);
            let y = {};
            if (T.keyMap) for (let _ = 2; _ < o; _++) {
              let g = T.decodeKey(p[_ - 2]);
              y[Y(g)] = A();
            }
            else for (let _ = 2; _ < o; _++) {
              let g = p[_ - 2];
              y[Y(g)] = A();
            }
            return y;
          } else if (t == nr) {
            let o = pe(), l = A();
            for (let p = 2; p < o; p++)
              Ke(l++, A());
            return A();
          } else if (t == rt)
            return pr();
          if (T.getShared && (ze(), s = k[t & 8191], s))
            return s.read || (s.read = He(s)), s.read();
        }
      }
      let n = F[t];
      if (n)
        return n.handlesRead ? n(A) : n(A());
      {
        let s = A();
        for (let o = 0; o < Me.length; o++) {
          let l = Me[o](t, s);
          if (l !== void 0)
            return l;
        }
        return new ce(s, t);
      }
    case 7:
      switch (t) {
        case 20:
          return !1;
        case 21:
          return !0;
        case 22:
          return null;
        case 23:
          return;
        case 31:
        default:
          let s = (z || fe())[t];
          if (s !== void 0)
            return s;
          throw new Error("Unknown token " + t);
      }
    default:
      if (isNaN(t)) {
        let s = new Error("Unexpected end of CBOR data");
        throw s.incomplete = !0, s;
      }
      throw new Error("Unknown CBOR token " + t);
  }
}
const it = /^[a-zA-Z_$][a-zA-Z\d_$]*$/;
function He(t) {
  if (!t) throw new Error("Structure is required in record definition");
  function e() {
    let r = w[c++];
    if (r = r & 31, r > 23)
      switch (r) {
        case 24:
          r = w[c++];
          break;
        case 25:
          r = W.getUint16(c), c += 2;
          break;
        case 26:
          r = W.getUint32(c), c += 4;
          break;
        default:
          throw new Error("Expected array header, but got " + w[c - 1]);
      }
    let n = this.compiledReader;
    for (; n; ) {
      if (n.propertyCount === r)
        return n(A);
      n = n.next;
    }
    if (this.slowReads++ >= yt) {
      let o = this.length == r ? this : this.slice(0, r);
      return n = T.keyMap ? new Function("r", "return {" + o.map((l) => T.decodeKey(l)).map((l) => it.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}") : new Function("r", "return {" + o.map((l) => it.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}"), this.compiledReader && (n.next = this.compiledReader), n.propertyCount = r, this.compiledReader = n, n(A);
    }
    let s = {};
    if (T.keyMap) for (let o = 0; o < r; o++) s[Y(T.decodeKey(this[o]))] = A();
    else for (let o = 0; o < r; o++)
      s[Y(this[o])] = A();
    return s;
  }
  return t.slowReads = 0, e;
}
function Y(t) {
  if (typeof t == "string") return t === "__proto__" ? "__proto_" : t;
  if (typeof t == "number" || typeof t == "boolean" || typeof t == "bigint") return t.toString();
  if (t == null) return t + "";
  throw new Error("Invalid property name type " + typeof t);
}
let ir = je;
function je(t) {
  let e;
  if (t < 16 && (e = wt(t)))
    return e;
  if (t > 64 && Le)
    return Le.decode(w.subarray(c, c += t));
  const r = c + t, n = [];
  for (e = ""; c < r; ) {
    const s = w[c++];
    if (!(s & 128))
      n.push(s);
    else if ((s & 224) === 192)
      if (s < 194 || c >= r || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const o = w[c++] & 63;
        n.push((s & 31) << 6 | o);
      }
    else if ((s & 240) === 224) {
      const o = c < r ? w[c] : 0;
      if (c >= r || (o & 192) !== 128 || s === 224 && o < 160 || s === 237 && o >= 160)
        n.push(65533);
      else if (c++, c >= r || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = w[c++] & 63;
        n.push((s & 31) << 12 | (o & 63) << 6 | l);
      }
    } else if ((s & 248) === 240) {
      const o = c < r ? w[c] : 0;
      if (s > 244 || c >= r || (o & 192) !== 128 || s === 240 && o < 144 || s === 244 && o >= 144)
        n.push(65533);
      else if (c++, c >= r || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = w[c++] & 63;
        if (c >= r || (w[c] & 192) !== 128)
          n.push(65533);
        else {
          const p = w[c++] & 63;
          let y = (s & 7) << 18 | (o & 63) << 12 | l << 6 | p;
          y -= 65536, n.push(y >>> 10 & 1023 | 55296), n.push(56320 | y & 1023);
        }
      }
    } else
      n.push(65533);
    n.length >= 4096 && (e += j.apply(String, n), n.length = 0);
  }
  return n.length > 0 && (e += j.apply(String, n)), e;
}
let j = String.fromCharCode;
function or(t) {
  let e = c, r = new Array(t);
  for (let n = 0; n < t; n++) {
    const s = w[c++];
    if ((s & 128) > 0) {
      c = e;
      return;
    }
    r[n] = s;
  }
  return j.apply(String, r);
}
function wt(t) {
  if (t < 4)
    if (t < 2) {
      if (t === 0)
        return "";
      {
        let e = w[c++];
        if ((e & 128) > 1) {
          c -= 1;
          return;
        }
        return j(e);
      }
    } else {
      let e = w[c++], r = w[c++];
      if ((e & 128) > 0 || (r & 128) > 0) {
        c -= 2;
        return;
      }
      if (t < 3)
        return j(e, r);
      let n = w[c++];
      if ((n & 128) > 0) {
        c -= 3;
        return;
      }
      return j(e, r, n);
    }
  else {
    let e = w[c++], r = w[c++], n = w[c++], s = w[c++];
    if ((e & 128) > 0 || (r & 128) > 0 || (n & 128) > 0 || (s & 128) > 0) {
      c -= 4;
      return;
    }
    if (t < 6) {
      if (t === 4)
        return j(e, r, n, s);
      {
        let o = w[c++];
        if ((o & 128) > 0) {
          c -= 5;
          return;
        }
        return j(e, r, n, s, o);
      }
    } else if (t < 8) {
      let o = w[c++], l = w[c++];
      if ((o & 128) > 0 || (l & 128) > 0) {
        c -= 6;
        return;
      }
      if (t < 7)
        return j(e, r, n, s, o, l);
      let p = w[c++];
      if ((p & 128) > 0) {
        c -= 7;
        return;
      }
      return j(e, r, n, s, o, l, p);
    } else {
      let o = w[c++], l = w[c++], p = w[c++], y = w[c++];
      if ((o & 128) > 0 || (l & 128) > 0 || (p & 128) > 0 || (y & 128) > 0) {
        c -= 8;
        return;
      }
      if (t < 10) {
        if (t === 8)
          return j(e, r, n, s, o, l, p, y);
        {
          let _ = w[c++];
          if ((_ & 128) > 0) {
            c -= 9;
            return;
          }
          return j(e, r, n, s, o, l, p, y, _);
        }
      } else if (t < 12) {
        let _ = w[c++], g = w[c++];
        if ((_ & 128) > 0 || (g & 128) > 0) {
          c -= 10;
          return;
        }
        if (t < 11)
          return j(e, r, n, s, o, l, p, y, _, g);
        let P = w[c++];
        if ((P & 128) > 0) {
          c -= 11;
          return;
        }
        return j(e, r, n, s, o, l, p, y, _, g, P);
      } else {
        let _ = w[c++], g = w[c++], P = w[c++], L = w[c++];
        if ((_ & 128) > 0 || (g & 128) > 0 || (P & 128) > 0 || (L & 128) > 0) {
          c -= 12;
          return;
        }
        if (t < 14) {
          if (t === 12)
            return j(e, r, n, s, o, l, p, y, _, g, P, L);
          {
            let G = w[c++];
            if ((G & 128) > 0) {
              c -= 13;
              return;
            }
            return j(e, r, n, s, o, l, p, y, _, g, P, L, G);
          }
        } else {
          let G = w[c++], V = w[c++];
          if ((G & 128) > 0 || (V & 128) > 0) {
            c -= 14;
            return;
          }
          if (t < 15)
            return j(e, r, n, s, o, l, p, y, _, g, P, L, G, V);
          let I = w[c++];
          if ((I & 128) > 0) {
            c -= 15;
            return;
          }
          return j(e, r, n, s, o, l, p, y, _, g, P, L, G, V, I);
        }
      }
    }
  }
}
function ar(t) {
  return T.copyBuffers ? (
    // specifically use the copying slice (not the node one)
    Uint8Array.prototype.slice.call(w, c, c += t)
  ) : w.subarray(c, c += t);
}
let xt = new Float32Array(1), Oe = new Uint8Array(xt.buffer, 0, 4);
function fr() {
  let t = w[c++], e = w[c++], r = (t & 127) >> 2;
  if (r === 31)
    return e || t & 3 ? NaN : t & 128 ? -1 / 0 : 1 / 0;
  if (r === 0) {
    let n = ((t & 3) << 8 | e) / 16777216;
    return t & 128 ? -n : n;
  }
  return Oe[3] = t & 128 | // sign bit
  (r >> 1) + 56, Oe[2] = (t & 7) << 5 | // last exponent bit and first two mantissa bits
  e >> 3, Oe[1] = e << 5, Oe[0] = 0, xt[0];
}
new Array(4096);
class ce {
  constructor(e, r) {
    this.value = e, this.tag = r;
  }
}
F[0] = (t) => new Date(t);
F[1] = (t) => new Date(Math.round(t * 1e3));
F[2] = (t) => {
  let e = BigInt(0);
  for (let r = 0, n = t.byteLength; r < n; r++)
    e = BigInt(t[r]) + (e << BigInt(8));
  return e;
};
F[3] = (t) => BigInt(-1) - F[2](t);
F[4] = (t) => +(t[1] + "e" + t[0]);
F[5] = (t) => t[1] * Math.exp(t[0] * Math.log(2));
const Ke = (t, e) => {
  t = t - 57344;
  let r = k[t];
  r && r.isShared && ((k.restoreStructures || (k.restoreStructures = []))[t] = r), k[t] = e, e.read = He(e);
};
F[rr] = (t) => {
  let e = t.length, r = t[1];
  Ke(t[0], r);
  let n = {};
  for (let s = 2; s < e; s++) {
    let o = r[s - 2];
    n[Y(o)] = t[s];
  }
  return n;
};
F[14] = (t) => H ? H[0].slice(H.position0, H.position0 += t) : new ce(t, 14);
F[15] = (t) => H ? H[1].slice(H.position1, H.position1 += t) : new ce(t, 15);
let lr = { Error, RegExp };
F[27] = (t) => (lr[t[0]] || Error)(t[1], t[2]);
const Et = (t) => {
  if (w[c++] != 132) {
    let r = new Error("Packed values structure must be followed by a 4 element array");
    throw w.length < c && (r.incomplete = !0), r;
  }
  let e = t();
  if (!e || !e.length) {
    let r = new Error("Packed values structure must be followed by a 4 element array");
    throw r.incomplete = !0, r;
  }
  return z = z ? e.concat(z.slice(e.length)) : e, z.prefixes = t(), z.suffixes = t(), t();
};
Et.handlesRead = !0;
F[51] = Et;
F[nt] = (t) => {
  if (!z)
    if (T.getShared)
      ze();
    else
      return new ce(t, nt);
  if (typeof t == "number")
    return z[16 + (t >= 0 ? 2 * t : -2 * t - 1)];
  let e = new Error("No support for non-integer packed references yet");
  throw t === void 0 && (e.incomplete = !0), e;
};
F[28] = (t) => {
  Z || (Z = /* @__PURE__ */ new Map(), Z.id = 0);
  let e = Z.id++, r = c, n = w[c], s;
  n >> 5 == 4 ? s = [] : s = {};
  let o = { target: s };
  Z.set(e, o);
  let l = t();
  return o.used ? (Object.getPrototypeOf(s) !== Object.getPrototypeOf(l) && (c = r, s = l, Z.set(e, { target: s }), l = t()), Object.assign(s, l)) : (o.target = l, l);
};
F[28].handlesRead = !0;
F[29] = (t) => {
  let e = Z.get(t);
  return e.used = !0, e.target;
};
F[258] = (t) => new Set(t);
(F[259] = (t) => (T.mapsAsObjects && (T.mapsAsObjects = !1, we = !0), t())).handlesRead = !0;
function he(t, e) {
  return typeof t == "string" ? t + e : t instanceof Array ? t.concat(e) : Object.assign({}, t, e);
}
function fe() {
  if (!z)
    if (T.getShared)
      ze();
    else
      throw new Error("No packed values available");
  return z;
}
const cr = 1399353956;
Me.push((t, e) => {
  if (t >= 225 && t <= 255)
    return he(fe().prefixes[t - 224], e);
  if (t >= 28704 && t <= 32767)
    return he(fe().prefixes[t - 28672], e);
  if (t >= 1879052288 && t <= 2147483647)
    return he(fe().prefixes[t - 1879048192], e);
  if (t >= 216 && t <= 223)
    return he(e, fe().suffixes[t - 216]);
  if (t >= 27647 && t <= 28671)
    return he(e, fe().suffixes[t - 27639]);
  if (t >= 1811940352 && t <= 1879048191)
    return he(e, fe().suffixes[t - 1811939328]);
  if (t == cr)
    return {
      packedValues: z,
      structures: k.slice(0),
      version: e
    };
  if (t == 55799)
    return e;
});
const ur = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1, ot = [
  Uint8Array,
  Uint8ClampedArray,
  Uint16Array,
  Uint32Array,
  typeof BigUint64Array > "u" ? { name: "BigUint64Array" } : BigUint64Array,
  Int8Array,
  Int16Array,
  Int32Array,
  typeof BigInt64Array > "u" ? { name: "BigInt64Array" } : BigInt64Array,
  Float32Array,
  Float64Array
], dr = [64, 68, 69, 70, 71, 72, 77, 78, 79, 85, 86];
for (let t = 0; t < ot.length; t++)
  hr(ot[t], dr[t]);
function hr(t, e) {
  let r = "get" + t.name.slice(0, -5), n;
  typeof t == "function" ? n = t.BYTES_PER_ELEMENT : t = null;
  for (let s = 0; s < 2; s++) {
    if (!s && n == 1)
      continue;
    let o = n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : 0;
    F[s ? e : e - 4] = n == 1 || s == ur ? (l) => {
      if (!t)
        throw new Error("Could not find typed array for code " + e);
      return !T.copyBuffers && (n === 1 || n === 2 && !(l.byteOffset & 1) || n === 4 && !(l.byteOffset & 3) || n === 8 && !(l.byteOffset & 7)) ? new t(l.buffer, l.byteOffset, l.byteLength >> o) : new t(Uint8Array.prototype.slice.call(l, 0).buffer);
    } : (l) => {
      if (!t)
        throw new Error("Could not find typed array for code " + e);
      let p = new DataView(l.buffer, l.byteOffset, l.byteLength), y = l.length >> o, _ = new t(y), g = p[r];
      for (let P = 0; P < y; P++)
        _[P] = g.call(p, P << o, s);
      return _;
    };
  }
}
function pr() {
  let t = pe(), e = c + A();
  for (let n = 2; n < t; n++) {
    let s = pe();
    c += s;
  }
  let r = c;
  return c = e, H = [je(pe()), je(pe())], H.position0 = 0, H.position1 = 0, H.postBundlePosition = c, c = r, A();
}
function pe() {
  let t = w[c++] & 31;
  if (t > 23)
    switch (t) {
      case 24:
        t = w[c++];
        break;
      case 25:
        t = W.getUint16(c), c += 2;
        break;
      case 26:
        t = W.getUint32(c), c += 4;
        break;
    }
  return t;
}
function ze() {
  if (T.getShared) {
    let t = gt(() => (w = null, T.getShared())) || {}, e = t.structures || [];
    T.sharedVersion = t.version, z = T.sharedValues = t.packedValues, k === !0 ? T.structures = k = e : k.splice.apply(k, [0, e.length].concat(e));
  }
}
function gt(t) {
  let e = le, r = c, n = Pe, s = Ee, o = Ue, l = Z, p = H, y = new Uint8Array(w.slice(0, le)), _ = k, g = T, P = ge, L = t();
  return le = e, c = r, Pe = n, Ee = s, Ue = o, Z = l, H = p, w = y, ge = P, k = _, T = g, W = new DataView(w.buffer, w.byteOffset, w.byteLength), L;
}
function Ge() {
  w = null, Z = null, k = null;
}
const Qe = new Array(147);
for (let t = 0; t < 256; t++)
  Qe[t] = +("1e" + Math.floor(45.15 - t * 0.30103));
let Je = new me({ useRecords: !1 });
const $ = Je.decode;
Je.decodeMultiple;
let Te;
try {
  Te = new TextEncoder();
} catch {
}
let qe, mt;
const Ne = typeof globalThis == "object" && globalThis.Buffer, Se = typeof Ne < "u", Be = Se ? Ne.allocUnsafeSlow : Uint8Array, at = Se ? Ne : Uint8Array, ft = 256, lt = Se ? 4294967296 : 2144337920;
let Ce, f, N, i = 0, oe, M = null;
const yr = 61440, wr = /[\u0080-\uFFFF]/, Q = Symbol("record-id");
class St extends me {
  constructor(e) {
    super(e), this.offset = 0;
    let r, n, s, o, l;
    e = e || {};
    let p = at.prototype.utf8Write ? function(a, x) {
      return f.utf8Write(a, x, f.byteLength - x);
    } : Te && Te.encodeInto ? function(a, x) {
      return Te.encodeInto(a, f.subarray(x)).written;
    } : !1, y = this, _ = e.structures || e.saveStructures, g = e.maxSharedStructures;
    if (g == null && (g = _ ? 128 : 0), g > 8190)
      throw new Error("Maximum maxSharedStructure is 8190");
    let P = e.sequential;
    P && (g = 0), this.structures || (this.structures = []), this.saveStructures && (this.saveShared = this.saveStructures);
    let L, G, V = e.sharedValues, I;
    if (V) {
      I = /* @__PURE__ */ Object.create(null);
      for (let a = 0, x = V.length; a < x; a++)
        I[V[a]] = a;
    }
    let q = [], re = 0, C = 0;
    this.mapEncode = function(a, x) {
      if (this._keyMap && !this._mapped)
        switch (a.constructor.name) {
          case "Array":
            a = a.map((d) => this.encodeKeys(d));
            break;
        }
      return this.encode(a, x);
    }, this.encode = function(a, x) {
      if (f || (f = new Be(8192), N = new DataView(f.buffer, 0, 8192), i = 0), oe = f.length - 10, oe - i < 2048 ? (f = new Be(f.length), N = new DataView(f.buffer, 0, f.length), oe = f.length - 10, i = 0) : x === dt && (i = i + 7 & 2147483640), r = i, y.useSelfDescribedHeader && (N.setUint32(i, 3654940416), i += 3), l = y.structuredClone ? /* @__PURE__ */ new Map() : null, y.bundleStrings && typeof a != "string" ? (M = [], M.size = 1 / 0) : M = null, n = y.structures, n) {
        if (n.uninitialized) {
          let h = y.getShared() || {};
          y.structures = n = h.structures || [], y.sharedVersion = h.version;
          let u = y.sharedValues = h.packedValues;
          if (u) {
            I = {};
            for (let m = 0, b = u.length; m < b; m++)
              I[u[m]] = m;
          }
        }
        let d = n.length;
        if (d > g && !P && (d = g), !n.transitions) {
          n.transitions = /* @__PURE__ */ Object.create(null);
          for (let h = 0; h < d; h++) {
            let u = n[h];
            if (!u)
              continue;
            let m, b = n.transitions;
            for (let R = 0, O = u.length; R < O; R++) {
              b[Q] === void 0 && (b[Q] = h);
              let U = u[R];
              m = b[U], m || (m = b[U] = /* @__PURE__ */ Object.create(null)), b = m;
            }
            b[Q] = h | 1048576;
          }
        }
        P || (n.nextId = d);
      }
      if (s && (s = !1), o = n || [], G = I, e.pack) {
        let d = /* @__PURE__ */ new Map();
        if (d.values = [], d.encoder = y, d.maxValues = e.maxPrivatePackedValues || (I ? 16 : 1 / 0), d.objectMap = I || !1, d.samplingPackedValues = L, Ae(a, d), d.values.length > 0) {
          f[i++] = 216, f[i++] = 51, te(4);
          let h = d.values;
          S(h), te(0), te(0), G = Object.create(I || null);
          for (let u = 0, m = h.length; u < m; u++)
            G[h[u]] = u;
        }
      }
      Ce = x & ke;
      try {
        if (Ce)
          return;
        if (S(a), M && ut(r, S), y.offset = i, l && l.idsToInsert) {
          i += l.idsToInsert.length * 2, i > oe && J(i), y.offset = i;
          let d = gr(f.subarray(r, i), l.idsToInsert);
          return l = null, d;
        }
        return x & dt ? (f.start = r, f.end = i, f) : f.subarray(r, i);
      } finally {
        if (n) {
          if (C < 10 && C++, n.length > g && (n.length = g), re > 1e4)
            n.transitions = null, C = 0, re = 0, q.length > 0 && (q = []);
          else if (q.length > 0 && !P) {
            for (let d = 0, h = q.length; d < h; d++)
              q[d][Q] = void 0;
            q = [];
          }
        }
        if (s && y.saveShared) {
          y.structures.length > g && (y.structures = y.structures.slice(0, g));
          let d = f.subarray(r, i);
          return y.updateSharedData() === !1 ? y.encode(a) : d;
        }
        x & Sr && (i = r);
      }
    }, this.findCommonStringsToPack = () => (L = /* @__PURE__ */ new Map(), I || (I = /* @__PURE__ */ Object.create(null)), (a) => {
      let x = a && a.threshold || 4, d = this.pack ? a.maxPrivatePackedValues || 16 : 0;
      V || (V = this.sharedValues = []);
      for (let [h, u] of L)
        u.count > x && (I[h] = d++, V.push(h), s = !0);
      for (; this.saveShared && this.updateSharedData() === !1; )
        ;
      L = null;
    });
    const S = (a) => {
      i > oe && (f = J(i));
      var x = typeof a, d;
      if (x === "string") {
        if (G) {
          let b = G[a];
          if (b >= 0) {
            b < 16 ? f[i++] = b + 224 : (f[i++] = 198, b & 1 ? S(15 - b >> 1) : S(b - 16 >> 1));
            return;
          } else if (L && !e.pack) {
            let R = L.get(a);
            R ? R.count++ : L.set(a, {
              count: 1
            });
          }
        }
        let h = a.length;
        if (M && h >= 4 && h < 1024) {
          if ((M.size += h) > yr) {
            let R, O = (M[0] ? M[0].length * 3 + M[1].length : 0) + 10;
            i + O > oe && (f = J(i + O)), f[i++] = 217, f[i++] = 223, f[i++] = 249, f[i++] = M.position ? 132 : 130, f[i++] = 26, R = i - r, i += 4, M.position && ut(r, S), M = ["", ""], M.size = 0, M.position = R;
          }
          let b = wr.test(a);
          M[b ? 0 : 1] += a, f[i++] = b ? 206 : 207, S(h);
          return;
        }
        let u;
        h < 32 ? u = 1 : h < 256 ? u = 2 : h < 65536 ? u = 3 : u = 5;
        let m = h * 3;
        if (i + m > oe && (f = J(i + m)), h < 64 || !p) {
          let b, R, O, U = i + u;
          for (b = 0; b < h; b++)
            R = a.charCodeAt(b), R < 128 ? f[U++] = R : R < 2048 ? (f[U++] = R >> 6 | 192, f[U++] = R & 63 | 128) : (R & 64512) === 55296 && ((O = a.charCodeAt(b + 1)) & 64512) === 56320 ? (R = 65536 + ((R & 1023) << 10) + (O & 1023), b++, f[U++] = R >> 18 | 240, f[U++] = R >> 12 & 63 | 128, f[U++] = R >> 6 & 63 | 128, f[U++] = R & 63 | 128) : (f[U++] = R >> 12 | 224, f[U++] = R >> 6 & 63 | 128, f[U++] = R & 63 | 128);
          d = U - i - u;
        } else
          d = p(a, i + u, m);
        d < 24 ? f[i++] = 96 | d : d < 256 ? (u < 2 && f.copyWithin(i + 2, i + 1, i + 1 + d), f[i++] = 120, f[i++] = d) : d < 65536 ? (u < 3 && f.copyWithin(i + 3, i + 2, i + 2 + d), f[i++] = 121, f[i++] = d >> 8, f[i++] = d & 255) : (u < 5 && f.copyWithin(i + 5, i + 3, i + 3 + d), f[i++] = 122, N.setUint32(i, d), i += 4), i += d;
      } else if (x === "number")
        if (!this.alwaysUseFloat && a >>> 0 === a)
          a < 24 ? f[i++] = a : a < 256 ? (f[i++] = 24, f[i++] = a) : a < 65536 ? (f[i++] = 25, f[i++] = a >> 8, f[i++] = a & 255) : (f[i++] = 26, N.setUint32(i, a), i += 4);
        else if (!this.alwaysUseFloat && a >> 0 === a)
          a >= -24 ? f[i++] = 31 - a : a >= -256 ? (f[i++] = 56, f[i++] = ~a) : a >= -65536 ? (f[i++] = 57, N.setUint16(i, ~a), i += 2) : (f[i++] = 58, N.setUint32(i, ~a), i += 4);
        else if (!this.alwaysUseFloat && a < 0 && a >= -4294967296 && Math.floor(a) === a)
          f[i++] = 58, N.setUint32(i, -1 - a), i += 4;
        else {
          let h;
          if ((h = this.useFloat32) > 0 && a < 4294967296 && a >= -2147483648) {
            f[i++] = 250, N.setFloat32(i, a);
            let u;
            if (h < 4 || // this checks for rounding of numbers that were encoded in 32-bit float to nearest significant decimal digit that could be preserved
            (u = a * Qe[(f[i] & 127) << 1 | f[i + 1] >> 7]) >> 0 === u) {
              i += 4;
              return;
            } else
              i--;
          }
          f[i++] = 251, N.setFloat64(i, a), i += 8;
        }
      else if (x === "object")
        if (!a)
          f[i++] = 246;
        else {
          if (l) {
            let u = l.get(a);
            if (u) {
              if (f[i++] = 216, f[i++] = 29, f[i++] = 25, !u.references) {
                let m = l.idsToInsert || (l.idsToInsert = []);
                u.references = [], m.push(u);
              }
              u.references.push(i - r), i += 2;
              return;
            } else
              l.set(a, { offset: i - r });
          }
          let h = a.constructor;
          if (h === Object)
            this.skipFunction === !0 && (a = Object.fromEntries([...Object.keys(a).filter((u) => typeof a[u] != "function").map((u) => [u, a[u]])])), X(a);
          else if (h === Array) {
            d = a.length, d < 24 ? f[i++] = 128 | d : te(d);
            for (let u = 0; u < d; u++)
              S(a[u]);
          } else if (h === Map)
            if ((this.mapsAsObjects ? this.useTag259ForMaps !== !1 : this.useTag259ForMaps) && (f[i++] = 217, f[i++] = 1, f[i++] = 3), d = a.size, d < 24 ? f[i++] = 160 | d : d < 256 ? (f[i++] = 184, f[i++] = d) : d < 65536 ? (f[i++] = 185, f[i++] = d >> 8, f[i++] = d & 255) : (f[i++] = 186, N.setUint32(i, d), i += 4), y.keyMap)
              for (let [u, m] of a)
                S(y.encodeKey(u)), S(m);
            else
              for (let [u, m] of a)
                S(u), S(m);
          else {
            for (let u = 0, m = qe.length; u < m; u++) {
              let b = mt[u];
              if (a instanceof b) {
                let R = qe[u], O = R.tag;
                O == null && (O = R.getTag && R.getTag.call(this, a)), O < 24 ? f[i++] = 192 | O : O < 256 ? (f[i++] = 216, f[i++] = O) : O < 65536 ? (f[i++] = 217, f[i++] = O >> 8, f[i++] = O & 255) : O > -1 && (f[i++] = 218, N.setUint32(i, O), i += 4), R.encode.call(this, a, S, J);
                return;
              }
            }
            if (a[Symbol.iterator]) {
              if (Ce) {
                let u = new Error("Iterable should be serialized as iterator");
                throw u.iteratorNotHandled = !0, u;
              }
              f[i++] = 159;
              for (let u of a)
                S(u);
              f[i++] = 255;
              return;
            }
            if (a[Symbol.asyncIterator] || Ie(a)) {
              let u = new Error("Iterable/blob should be serialized as iterator");
              throw u.iteratorNotHandled = !0, u;
            }
            if (this.useToJSON && a.toJSON) {
              const u = a.toJSON();
              if (u !== a)
                return S(u);
            }
            X(a);
          }
        }
      else if (x === "boolean")
        f[i++] = a ? 245 : 244;
      else if (x === "bigint") {
        if (a < BigInt(1) << BigInt(64) && a >= 0)
          f[i++] = 27, N.setBigUint64(i, a);
        else if (a > -(BigInt(1) << BigInt(64)) && a < 0)
          f[i++] = 59, N.setBigUint64(i, -a - BigInt(1));
        else if (this.largeBigIntToFloat)
          f[i++] = 251, N.setFloat64(i, Number(a));
        else {
          a >= BigInt(0) ? f[i++] = 194 : (f[i++] = 195, a = BigInt(-1) - a);
          let h = [];
          for (; a; )
            h.push(Number(a & BigInt(255))), a >>= BigInt(8);
          $e(new Uint8Array(h.reverse()), J);
          return;
        }
        i += 8;
      } else if (x === "undefined")
        f[i++] = 247;
      else
        throw new Error("Unknown type: " + x);
    }, X = this.useRecords === !1 ? this.variableMapSize ? (a) => {
      let x = Object.keys(a), d = Object.values(a), h = x.length;
      if (h < 24 ? f[i++] = 160 | h : h < 256 ? (f[i++] = 184, f[i++] = h) : h < 65536 ? (f[i++] = 185, f[i++] = h >> 8, f[i++] = h & 255) : (f[i++] = 186, N.setUint32(i, h), i += 4), y.keyMap)
        for (let u = 0; u < h; u++)
          S(y.encodeKey(x[u])), S(d[u]);
      else
        for (let u = 0; u < h; u++)
          S(x[u]), S(d[u]);
    } : (a) => {
      f[i++] = 185;
      let x = i - r;
      i += 2;
      let d = 0;
      if (y.keyMap)
        for (let h in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(h)) && (S(y.encodeKey(h)), S(a[h]), d++);
      else
        for (let h in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(h)) && (S(h), S(a[h]), d++);
      f[x++ + r] = d >> 8, f[x + r] = d & 255;
    } : (a, x) => {
      let d, h = o.transitions || (o.transitions = /* @__PURE__ */ Object.create(null)), u = 0, m = 0, b, R;
      if (this.keyMap) {
        R = Object.keys(a).map((U) => this.encodeKey(U)), m = R.length;
        for (let U = 0; U < m; U++) {
          let tt = R[U];
          d = h[tt], d || (d = h[tt] = /* @__PURE__ */ Object.create(null), u++), h = d;
        }
      } else
        for (let U in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(U)) && (d = h[U], d || (h[Q] & 1048576 && (b = h[Q] & 65535), d = h[U] = /* @__PURE__ */ Object.create(null), u++), h = d, m++);
      let O = h[Q];
      if (O !== void 0)
        O &= 65535, f[i++] = 217, f[i++] = O >> 8 | 224, f[i++] = O & 255;
      else if (R || (R = h.__keys__ || (h.__keys__ = Object.keys(a))), b === void 0 ? (O = o.nextId++, O || (O = 0, o.nextId = 1), O >= ft && (o.nextId = (O = g) + 1)) : O = b, o[O] = R, O < g) {
        f[i++] = 217, f[i++] = O >> 8 | 224, f[i++] = O & 255, h = o.transitions;
        for (let U = 0; U < m; U++)
          (h[Q] === void 0 || h[Q] & 1048576) && (h[Q] = O), h = h[R[U]];
        h[Q] = O | 1048576, s = !0;
      } else {
        if (h[Q] = O, N.setUint32(i, 3655335680), i += 3, u && (re += C * u), q.length >= ft - g && (q.shift()[Q] = void 0), q.push(h), te(m + 2), S(57344 + O), S(R), x) return;
        for (let U in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(U)) && S(a[U]);
        return;
      }
      if (m < 24 ? f[i++] = 128 | m : te(m), !x)
        for (let U in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(U)) && S(a[U]);
    }, J = (a) => {
      let x;
      if (a > 16777216) {
        if (a - r > lt)
          throw new Error("Encoded buffer would be larger than maximum buffer size");
        x = Math.min(
          lt,
          Math.round(Math.max((a - r) * (a > 67108864 ? 1.25 : 2), 4194304) / 4096) * 4096
        );
      } else
        x = (Math.max(a - r << 2, f.length - 1) >> 12) + 1 << 12;
      let d = new Be(x);
      return N = new DataView(d.buffer, 0, x), f.copy ? f.copy(d, 0, r, a) : d.set(f.slice(r, a)), i -= r, r = 0, oe = d.length - 10, f = d;
    };
    let v = 100, ae = 1e3;
    this.encodeAsIterable = function(a, x) {
      return be(a, x, ne);
    }, this.encodeAsAsyncIterable = function(a, x) {
      return be(a, x, _e);
    };
    function* ne(a, x, d) {
      let h = a.constructor;
      if (h === Object) {
        let u = y.useRecords !== !1;
        u ? X(a, !0) : ct(Object.keys(a).length, 160);
        for (let m in a) {
          let b = a[m];
          u || S(m), b && typeof b == "object" ? x[m] ? yield* ne(b, x[m]) : yield* ue(b, x, m) : S(b);
        }
      } else if (h === Array) {
        let u = a.length;
        te(u);
        for (let m = 0; m < u; m++) {
          let b = a[m];
          b && (typeof b == "object" || i - r > v) ? x.element ? yield* ne(b, x.element) : yield* ue(b, x, "element") : S(b);
        }
      } else if (a[Symbol.iterator] && !a.buffer) {
        f[i++] = 159;
        for (let u of a)
          u && (typeof u == "object" || i - r > v) ? x.element ? yield* ne(u, x.element) : yield* ue(u, x, "element") : S(u);
        f[i++] = 255;
      } else Ie(a) ? (ct(a.size, 64), yield f.subarray(r, i), yield a, se()) : a[Symbol.asyncIterator] ? (f[i++] = 159, yield f.subarray(r, i), yield a, se(), f[i++] = 255) : S(a);
      d && i > r ? yield f.subarray(r, i) : i - r > v && (yield f.subarray(r, i), se());
    }
    function* ue(a, x, d) {
      let h = i - r;
      try {
        S(a), i - r > v && (yield f.subarray(r, i), se());
      } catch (u) {
        if (u.iteratorNotHandled)
          x[d] = {}, i = r + h, yield* ne.call(this, a, x[d]);
        else throw u;
      }
    }
    function se() {
      v = ae, y.encode(null, ke);
    }
    function be(a, x, d) {
      return x && x.chunkThreshold ? v = ae = x.chunkThreshold : v = 100, a && typeof a == "object" ? (y.encode(null, ke), d(a, y.iterateProperties || (y.iterateProperties = {}), !0)) : [y.encode(a)];
    }
    async function* _e(a, x) {
      for (let d of ne(a, x, !0)) {
        let h = d.constructor;
        if (h === at || h === Uint8Array)
          yield d;
        else if (Ie(d)) {
          let u = d.stream().getReader(), m;
          for (; !(m = await u.read()).done; )
            yield m.value;
        } else if (d[Symbol.asyncIterator])
          for await (let u of d)
            se(), u ? yield* _e(u, x.async || (x.async = {})) : yield y.encode(u);
        else
          yield d;
      }
    }
  }
  useBuffer(e) {
    f = e, N = new DataView(f.buffer, f.byteOffset, f.byteLength), i = 0;
  }
  clearSharedData() {
    this.structures && (this.structures = []), this.sharedValues && (this.sharedValues = void 0);
  }
  updateSharedData() {
    let e = this.sharedVersion || 0;
    this.sharedVersion = e + 1;
    let r = this.structures.slice(0), n = new bt(r, this.sharedValues, this.sharedVersion), s = this.saveShared(
      n,
      (o) => (o && o.version || 0) == e
    );
    return s === !1 ? (n = this.getShared() || {}, this.structures = n.structures || [], this.sharedValues = n.packedValues, this.sharedVersion = n.version, this.structures.nextId = this.structures.length) : r.forEach((o, l) => this.structures[l] = o), s;
  }
}
function ct(t, e) {
  t < 24 ? f[i++] = e | t : t < 256 ? (f[i++] = e | 24, f[i++] = t) : t < 65536 ? (f[i++] = e | 25, f[i++] = t >> 8, f[i++] = t & 255) : (f[i++] = e | 26, N.setUint32(i, t), i += 4);
}
class bt {
  constructor(e, r, n) {
    this.structures = e, this.packedValues = r, this.version = n;
  }
}
function te(t) {
  t < 24 ? f[i++] = 128 | t : t < 256 ? (f[i++] = 152, f[i++] = t) : t < 65536 ? (f[i++] = 153, f[i++] = t >> 8, f[i++] = t & 255) : (f[i++] = 154, N.setUint32(i, t), i += 4);
}
const xr = typeof Blob > "u" ? function() {
} : Blob;
function Ie(t) {
  if (t instanceof xr)
    return !0;
  let e = t[Symbol.toStringTag];
  return e === "Blob" || e === "File";
}
function Ae(t, e) {
  switch (typeof t) {
    case "string":
      if (t.length > 3) {
        if (e.objectMap[t] > -1 || e.values.length >= e.maxValues)
          return;
        let n = e.get(t);
        if (n)
          ++n.count == 2 && e.values.push(t);
        else if (e.set(t, {
          count: 1
        }), e.samplingPackedValues) {
          let s = e.samplingPackedValues.get(t);
          s ? s.count++ : e.samplingPackedValues.set(t, {
            count: 1
          });
        }
      }
      break;
    case "object":
      if (t)
        if (t instanceof Array)
          for (let n = 0, s = t.length; n < s; n++)
            Ae(t[n], e);
        else {
          let n = !e.encoder.useRecords;
          for (var r in t)
            t.hasOwnProperty(r) && (n && Ae(r, e), Ae(t[r], e));
        }
      break;
    case "function":
      console.log(t);
  }
}
const Er = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1;
mt = [
  Date,
  Set,
  Error,
  RegExp,
  ce,
  ArrayBuffer,
  Uint8Array,
  Uint8ClampedArray,
  Uint16Array,
  Uint32Array,
  typeof BigUint64Array > "u" ? function() {
  } : BigUint64Array,
  Int8Array,
  Int16Array,
  Int32Array,
  typeof BigInt64Array > "u" ? function() {
  } : BigInt64Array,
  Float32Array,
  Float64Array,
  bt
];
qe = [
  {
    // Date
    tag: 1,
    encode(t, e) {
      let r = t.getTime() / 1e3;
      (this.useTimestamp32 || t.getMilliseconds() === 0) && r >= 0 && r < 4294967296 ? (f[i++] = 26, N.setUint32(i, r), i += 4) : (f[i++] = 251, N.setFloat64(i, r), i += 8);
    }
  },
  {
    // Set
    tag: 258,
    // https://github.com/input-output-hk/cbor-sets-spec/blob/master/CBOR_SETS.md
    encode(t, e) {
      let r = Array.from(t);
      e(r);
    }
  },
  {
    // Error
    tag: 27,
    // http://cbor.schmorp.de/generic-object
    encode(t, e) {
      e([t.name, t.message]);
    }
  },
  {
    // RegExp
    tag: 27,
    // http://cbor.schmorp.de/generic-object
    encode(t, e) {
      e(["RegExp", t.source, t.flags]);
    }
  },
  {
    // Tag
    getTag(t) {
      return t.tag;
    },
    encode(t, e) {
      e(t.value);
    }
  },
  {
    // ArrayBuffer
    encode(t, e, r) {
      $e(t, r);
    }
  },
  {
    // Uint8Array
    getTag(t) {
      if (t.constructor === Uint8Array && (this.tagUint8Array || Se && this.tagUint8Array !== !1))
        return 64;
    },
    encode(t, e, r) {
      $e(t, r);
    }
  },
  ee(68, 1),
  ee(69, 2),
  ee(70, 4),
  ee(71, 8),
  ee(72, 1),
  ee(77, 2),
  ee(78, 4),
  ee(79, 8),
  ee(85, 4),
  ee(86, 8),
  {
    encode(t, e) {
      let r = t.packedValues || [], n = t.structures || [];
      if (r.values.length > 0) {
        f[i++] = 216, f[i++] = 51, te(4);
        let s = r.values;
        e(s), te(0), te(0), packedObjectMap = Object.create(sharedPackedObjectMap || null);
        for (let o = 0, l = s.length; o < l; o++)
          packedObjectMap[s[o]] = o;
      }
      if (n) {
        N.setUint32(i, 3655335424), i += 3;
        let s = n.slice(0);
        s.unshift(57344), s.push(new ce(t.version, 1399353956)), e(s);
      } else
        e(new ce(t.version, 1399353956));
    }
  }
];
function ee(t, e) {
  return !Er && e > 1 && (t -= 4), {
    tag: t,
    encode: function(n, s) {
      let o = n.byteLength, l = n.byteOffset || 0, p = n.buffer || n;
      s(Se ? Ne.from(p, l, o) : new Uint8Array(p, l, o));
    }
  };
}
function $e(t, e) {
  let r = t.byteLength;
  r < 24 ? f[i++] = 64 + r : r < 256 ? (f[i++] = 88, f[i++] = r) : r < 65536 ? (f[i++] = 89, f[i++] = r >> 8, f[i++] = r & 255) : (f[i++] = 90, N.setUint32(i, r), i += 4), i + r >= f.length && e(i + r), f.set(t.buffer ? t : new Uint8Array(t), i), i += r;
}
function gr(t, e) {
  let r, n = e.length * 2, s = t.length - n;
  e.sort((o, l) => o.offset > l.offset ? 1 : -1);
  for (let o = 0; o < e.length; o++) {
    let l = e[o];
    l.id = o;
    for (let p of l.references)
      t[p++] = o >> 8, t[p] = o & 255;
  }
  for (; r = e.pop(); ) {
    let o = r.offset;
    t.copyWithin(o + n, o, s), n -= 2;
    let l = o + n;
    t[l++] = 216, t[l++] = 28, s = o;
  }
  return t;
}
function ut(t, e) {
  N.setUint32(M.position + t, i - M.position - t + 1);
  let r = M;
  M = null, e(r[0]), e(r[1]);
}
let Ze = new St({ useRecords: !1 });
const mr = Ze.encode;
Ze.encodeAsIterable;
Ze.encodeAsAsyncIterable;
const dt = 512, Sr = 1024, ke = 2048, K = new St({ tagUint8Array: !1 }), E = {
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
}, br = {
  OK: 0,
  BAD_REQUEST: 1,
  NOT_FOUND: 2,
  INTERNAL_ERROR: 3,
  RANGE_NOT_SATISFIABLE: 4,
  UNAUTHORIZED: 5
};
function Ye(t) {
  const e = $(t);
  return Array.isArray(e) ? e[0] : null;
}
function Xe(t) {
  const e = new TextEncoder().encode(t);
  return K.encode([E.AUTH_REQUEST, e]);
}
function ve(t, e = null) {
  const r = t.recyclerUrls || [], n = [
    E.PUT_REQUEST,
    t.contentType,
    t.fileName,
    t.streamLength,
    t.serverAddress || null,
    e || new Uint8Array(0),
    r,
    t.temporary ? 1 : 0
  ];
  return t.tupleSize !== void 0 && n.push(t.tupleSize), K.encode(n);
}
function _t(t) {
  return K.encode([E.PUT_DATA, t]);
}
function Rt() {
  return K.encode([E.PUT_END]);
}
function Ve(t) {
  const e = $(t);
  if (e[0] !== E.PUT_RESPONSE) throw new Error("Not a put response");
  return { oriString: e[1] };
}
function Ot(t, e) {
  const r = e && (e.start !== void 0 || e.end !== void 0), n = [E.GET_REQUEST, t, r ? 1 : 0];
  return r && (n.push(e.start || 0), n.push(e.end || 0)), K.encode(n);
}
function Tt(t) {
  const e = $(t);
  if (e[0] !== E.GET_RESPONSE_START) throw new Error("Not a get response start");
  return {
    contentType: e[1],
    contentLength: e[2],
    hasRange: e[3] === 1,
    rangeStart: e[3] ? e[4] : void 0,
    rangeEnd: e[3] ? e[5] : void 0
  };
}
function At(t) {
  const e = $(t);
  if (e[0] !== E.GET_DATA) throw new Error("Not a get data");
  return e[1];
}
function Ut(t) {
  const e = $(t);
  return Array.isArray(e) && e[0] === E.GET_END;
}
function Pt(t) {
  const e = $(t);
  return !Array.isArray(e) || e[0] !== E.ERROR ? null : { statusCode: e[1], message: e[2] };
}
function Nt(t, e = 0) {
  return K.encode([E.BLOCK_PUT_REQUEST, t, e]);
}
function Bt(t) {
  const e = $(t);
  if (e[0] !== E.BLOCK_PUT_RESPONSE) throw new Error("Not a block put response");
  return { status: e[1], hash: e[2] };
}
function Ct(t) {
  return K.encode([E.BLOCK_GET_REQUEST, t]);
}
function It(t) {
  const e = $(t);
  if (e[0] !== E.BLOCK_GET_RESPONSE) throw new Error("Not a block get response");
  return { status: e[1], data: e[2] };
}
function kt(t) {
  return K.encode([E.BLOCK_DELETE_REQUEST, t]);
}
function Dt(t) {
  const e = $(t);
  if (e[0] !== E.BLOCK_DELETE_RESPONSE) throw new Error("Not a block delete response");
  return { status: e[1] };
}
function Ft() {
  return K.encode([E.HEALTH_REQUEST]);
}
function Lt(t) {
  const e = $(t);
  if (e[0] !== E.HEALTH_RESPONSE) throw new Error("Not a health response");
  return { json: e[1] };
}
function Mt() {
  return K.encode([E.PEER_INFO_REQUEST]);
}
function Ht(t) {
  const e = $(t);
  if (e[0] !== E.PEER_INFO_RESPONSE) throw new Error("Not a peer info response");
  return { format: e[1], data: e[2] };
}
function jt(t, e) {
  return K.encode([E.PEER_CONNECT, t, e]);
}
function Kt(t) {
  const e = $(t);
  if (e[0] !== E.PEER_CONNECT_RESULT) throw new Error("Not a peer connect result");
  return { status: e[1] };
}
function Gt() {
  return K.encode([E.PEER_LIST_REQUEST]);
}
function qt(t) {
  const e = $(t);
  if (e[0] !== E.PEER_LIST_RESPONSE) throw new Error("Not a peer list response");
  return e[1];
}
function $t(t, e) {
  return K.encode([E.FRIEND_ADD, t, e]);
}
function vt(t) {
  return K.encode([E.FRIEND_REMOVE, t]);
}
function Vt() {
  return K.encode([E.FRIEND_LIST]);
}
function Wt(t) {
  const e = $(t);
  if (e[0] !== E.FRIEND_LIST_RESPONSE) throw new Error("Not a friend list response");
  return e[1];
}
function zt() {
  return K.encode([E.CONFIG_SHOW_REQUEST]);
}
function Qt(t) {
  const e = $(t);
  if (e[0] !== E.CONFIG_SHOW_RESPONSE) throw new Error("Not a config show response");
  return { json: e[1] };
}
function Jt(t, e) {
  return K.encode([E.CONFIG_SET_REQUEST, t, e]);
}
function Zt(t) {
  const e = $(t);
  if (e[0] !== E.CONFIG_SET_RESPONSE) throw new Error("Not a config set response");
  return { status: e[1], restartRequired: e[2] === 1, message: e[3] };
}
function Yt() {
  return K.encode([E.CONFIG_RELOAD_REQUEST]);
}
function Xt(t) {
  const e = $(t);
  if (e[0] !== E.CONFIG_RELOAD_RESPONSE) throw new Error("Not a config reload response");
  return { status: e[1], message: e[2] };
}
const Kr = /* @__PURE__ */ Object.freeze(/* @__PURE__ */ Object.defineProperty({
  __proto__: null,
  MSG: E,
  STATUS: br,
  decodeBlockDeleteResponse: Dt,
  decodeBlockGetResponse: It,
  decodeBlockPutResponse: Bt,
  decodeConfigReloadResponse: Xt,
  decodeConfigSetResponse: Zt,
  decodeConfigShowResponse: Qt,
  decodeError: Pt,
  decodeFriendListResponse: Wt,
  decodeGetData: At,
  decodeGetResponseStart: Tt,
  decodeHealthResponse: Lt,
  decodePeerConnectResult: Kt,
  decodePeerInfoResponse: Ht,
  decodePeerListResponse: qt,
  decodePutResponse: Ve,
  encodeAuthRequest: Xe,
  encodeBlockDeleteRequest: kt,
  encodeBlockGetRequest: Ct,
  encodeBlockPutRequest: Nt,
  encodeConfigReloadRequest: Yt,
  encodeConfigSetRequest: Jt,
  encodeConfigShowRequest: zt,
  encodeFriendAdd: $t,
  encodeFriendListRequest: Vt,
  encodeFriendRemove: vt,
  encodeGetRequest: Ot,
  encodeHealthRequest: Ft,
  encodePeerConnect: jt,
  encodePeerInfoRequest: Mt,
  encodePeerListRequest: Gt,
  encodePutData: _t,
  encodePutEnd: Rt,
  encodePutRequest: ve,
  getMessageType: Ye,
  isGetEnd: Ut
}, Symbol.toStringTag, { value: "Module" }));
class _r {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {WebSocket|null} */
    B(this, "socket", null);
    /** @type {string|undefined} */
    B(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    B(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    B(this, "openPromise", null);
    this.url = e, this.apiKey = r;
  }
  /**
   * @returns {Promise<void>}
   */
  connect() {
    return this.socket ? this.openPromise || Promise.resolve() : (this.socket = new WebSocket(this.url), this.socket.binaryType = "arraybuffer", this.openPromise = new Promise((e, r) => {
      const n = this.socket;
      if (!n) return r(new Error("Socket not created"));
      n.onopen = () => {
        this.apiKey && this.send(Xe(this.apiKey)), e();
      }, n.onerror = (s) => {
        var l;
        const o = s.message || ((l = s.error) == null ? void 0 : l.message) || "unknown";
        r(new Error(`WebSocket error: ${o}`));
      }, n.onclose = () => {
        this.socket = null, this.openPromise = null;
      }, n.onmessage = (s) => {
        var p;
        const o = new Uint8Array(s.data), l = Ye(o);
        l !== null && ((p = this.messageHandler) == null || p.call(this, l, o));
      };
    }), this.openPromise);
  }
  disconnect() {
    this.socket && (this.socket.close(), this.socket = null), this.openPromise = null;
  }
  isConnected() {
    return this.socket !== null && this.socket.readyState === WebSocket.OPEN;
  }
  /**
   * @param {Uint8Array} bytes
   */
  send(e) {
    if (!this.isConnected())
      throw new Error("WebSocket not connected");
    this.socket.send(e);
  }
  /**
   * @param {(type: number, bytes: Uint8Array) => void} handler
   */
  setMessageHandler(e) {
    this.messageHandler = e;
  }
}
class Rr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {WebTransport|null} */
    B(this, "transport", null);
    /** @type {WritableStreamWriter|null} */
    B(this, "writer", null);
    /** @type {ReadableStreamReader|null} */
    B(this, "reader", null);
    /** @type {string|undefined} */
    B(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    B(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    B(this, "openPromise", null);
    /** @type {boolean} */
    B(this, "running", !1);
    this.url = e, this.apiKey = r;
  }
  /**
   * @returns {Promise<void>}
   */
  async connect() {
    return this.transport ? this.openPromise || Promise.resolve() : (this.transport = new WebTransport(this.url), this.openPromise = this.transport.ready.then(async () => {
      const e = await this.transport.createBidirectionalStream();
      this.writer = e.writable.getWriter(), this.reader = e.readable.getReader(), this.running = !0, this._readLoop(), this.apiKey && await this.send(Xe(this.apiKey));
    }), this.openPromise);
  }
  disconnect() {
    var e, r, n;
    this.running = !1, (e = this.writer) == null || e.releaseLock(), (r = this.reader) == null || r.releaseLock(), (n = this.transport) == null || n.close(), this.writer = null, this.reader = null, this.transport = null, this.openPromise = null;
  }
  isConnected() {
    return this.transport !== null && this.transport.state === "connected";
  }
  /**
   * @param {Uint8Array} bytes
   */
  async send(e) {
    if (!this.writer) throw new Error("WebTransport not connected");
    const r = new Uint8Array(4);
    new DataView(r.buffer).setUint32(0, e.length, !1), await this.writer.write(r), await this.writer.write(e);
  }
  /**
   * @param {(type: number, bytes: Uint8Array) => void} handler
   */
  setMessageHandler(e) {
    this.messageHandler = e;
  }
  async _readLoop() {
    var r;
    let e = null;
    try {
      for (; this.running; ) {
        const { done: n, value: s } = await this.reader.read();
        if (n) break;
        const o = s instanceof Uint8Array ? s : new Uint8Array(s.buffer, s.byteOffset, s.byteLength);
        for (e = e ? Or(e, o) : o; e.length >= 4; ) {
          const p = new DataView(e.buffer, e.byteOffset, e.length).getUint32(0, !1);
          if (e.length < 4 + p) break;
          const y = e.subarray(4, 4 + p), _ = Ye(y);
          _ !== null && ((r = this.messageHandler) == null || r.call(this, _, y)), e = e.subarray(4 + p);
        }
      }
    } catch {
    }
  }
}
function Or(t, e) {
  const r = new Uint8Array(t.length + e.length);
  return r.set(t, 0), r.set(e, t.length), r;
}
const We = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz", et = new Int8Array(128);
et.fill(-1);
for (let t = 0; t < We.length; t++)
  et[We.charCodeAt(t)] = t;
function xe(t) {
  if (t.length === 0) return null;
  let e = 0;
  for (; e < t.length && t[e] === "1"; )
    e++;
  const r = [];
  for (let n = e; n < t.length; n++) {
    const s = t.charCodeAt(n);
    if (s >= 128) return null;
    const o = et[s];
    if (o < 0) return null;
    let l = o;
    for (let p = 0; p < r.length; p++)
      l += r[p] * 58, r[p] = l & 255, l >>= 8;
    for (; l > 0; )
      r.push(l & 255), l >>= 8;
  }
  for (let n = 0; n < e; n++)
    r.push(0);
  return r.reverse(), new Uint8Array(r);
}
function De(t) {
  if (t.length === 0) return "";
  const e = Array.from(t);
  let r = 0;
  for (; r < e.length && e[r] === 0; )
    r++;
  const n = [];
  for (let o = r; o < e.length; o++) {
    let l = e[o];
    for (let p = 0; p < n.length; p++)
      l += n[p] * 256, n[p] = l % 58, l = Math.floor(l / 58);
    for (; l > 0; )
      n.push(l % 58), l = Math.floor(l / 58);
  }
  return "1".repeat(r) + n.reverse().map((o) => We[o]).join("");
}
function ht(t) {
  const e = t.indexOf("/offsystem/v3/");
  if (e < 0) return null;
  const n = t.slice(e + 14).split("/");
  if (n.length < 4) return null;
  const s = n[n.length - 4], o = n[n.length - 3], l = n[n.length - 2], p = n.slice(n.length - 1).join("/"), y = parseInt(s, 10);
  return !Number.isFinite(y) || xe(o) === null || xe(l) === null ? null : {
    fileHashB58: o,
    descriptorHashB58: l,
    streamLength: y,
    fileName: decodeURIComponent(p)
  };
}
function Tr(t) {
  const e = {
    html: "text/html",
    htm: "text/html",
    css: "text/css",
    js: "application/javascript",
    json: "application/json",
    png: "image/png",
    jpg: "image/jpeg",
    jpeg: "image/jpeg",
    gif: "image/gif",
    svg: "image/svg+xml",
    ico: "image/x-icon",
    webp: "image/webp",
    bmp: "image/bmp",
    tiff: "image/tiff",
    tif: "image/tiff",
    mp4: "video/mp4",
    webm: "video/webm",
    mkv: "video/x-matroska",
    avi: "video/x-msvideo",
    mov: "video/quicktime",
    wmv: "video/x-msvideo",
    flv: "video/x-flv",
    mp3: "audio/mpeg",
    ogg: "audio/ogg",
    wav: "audio/wav",
    flac: "audio/flac",
    aac: "audio/mp4",
    m4a: "audio/mp4",
    woff: "font/woff",
    woff2: "font/woff2",
    ttf: "font/ttf",
    otf: "font/otf",
    pdf: "application/pdf",
    zip: "application/zip",
    gz: "application/gzip",
    tar: "application/x-tar",
    rar: "application/vnd.rar",
    "7z": "application/x-7z-compressed",
    doc: "application/msword",
    docx: "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    xls: "application/vnd.ms-excel",
    xlsx: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    ppt: "application/vnd.ms-powerpoint",
    pptx: "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    txt: "text/plain",
    csv: "text/csv",
    xml: "application/xml",
    md: "text/markdown",
    ofd: "application/cbor"
  }, r = t.lastIndexOf(".");
  if (r < 0 || r === t.length - 1) return "application/octet-stream";
  const n = t.slice(r + 1).toLowerCase();
  return e[n] || "application/octet-stream";
}
function Ar(t) {
  return typeof t.arrayBuffer == "function" ? t.arrayBuffer().then((e) => new Uint8Array(e)) : new Promise((e, r) => {
    const n = new FileReader();
    n.onload = () => e(new Uint8Array(n.result)), n.onerror = () => r(n.error), n.readAsArrayBuffer(t);
  });
}
function pt(t, e = 65536) {
  let r = 0;
  return new ReadableStream({
    pull(n) {
      if (r >= t.size) {
        n.close();
        return;
      }
      const s = Math.min(r + e, t.size), o = t.slice(r, s);
      return Ar(o).then((l) => {
        n.enqueue(l), r = s;
      });
    }
  });
}
function Ur(t) {
  if (typeof FileList < "u" && t instanceof FileList) {
    const e = [];
    for (let r = 0; r < t.length; r++) {
      const n = t[r];
      let s = n.webkitRelativePath || n.name;
      e.push({ path: s, file: n });
    }
    return e;
  }
  return Array.isArray(t) ? t.map((e) => e instanceof File || e instanceof Blob ? { path: e.webkitRelativePath || e.name, file: e } : { path: e.path, file: e.file }) : Object.entries(t).map(([e, r]) => ({ path: e, file: r }));
}
function Pr(t, e = "http://localhost:23402") {
  if (!t || /^https?:\/\//i.test(t)) return t;
  let r = t;
  r.startsWith("offs://") && (r = r.slice(7));
  const n = "/offsystem/v3/", s = r.indexOf(n);
  return s >= 0 && (r = r.slice(s)), r.startsWith(n) ? `${e.replace(/\/$/, "")}${r}` : t;
}
const Nr = 128e3, Br = 3;
function Cr({
  name: t,
  fileHash: e,
  descriptorHash: r,
  finalByte: n,
  blockType: s = Nr,
  tupleSize: o = Br,
  fileOffset: l = 0
}) {
  return {
    name: t,
    isDirectory: !1,
    fileHash: e,
    descriptorHash: r,
    finalByte: n,
    blockType: s,
    tupleSize: o,
    fileOffset: l
  };
}
function Ir({ name: t, dirHash: e }) {
  return { name: t, isDirectory: !0, dirHash: e };
}
function kr(t) {
  const e = t.map((r) => {
    const n = {
      n: r.name,
      t: r.isDirectory ? 1 : 0
    };
    return r.isDirectory ? n.d = r.dirHash : (n.f = r.fileHash, n.D = r.descriptorHash, n.s = r.finalByte, n.B = r.blockType, n.T = r.tupleSize, n.o = r.fileOffset), n;
  });
  return mr({ v: 1, entries: e });
}
function Dr() {
  return {
    connectTimeoutMs: 5e3,
    requestTimeoutMs: 3e4
  };
}
function Fr(t, e, r) {
  return t.startsWith("ws://") || t.startsWith("wss://") ? new _r(t, e, r) : t.startsWith("wt://") || t.startsWith("wts://") ? new Rr(t, e, r) : new D(t, e, r);
}
class Gr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {OffsClientConfig & {transport?: any}} [config]
   */
  constructor(e, r, n) {
    /** @type {string} */
    B(this, "url");
    /** @type {string|undefined} */
    B(this, "apiKey");
    /** @type {OffsClientConfig} */
    B(this, "config");
    /** @type {HttpTransport|WsTransport|WtTransport} */
    B(this, "transport");
    /** @type {Map<number, PendingRequest>} */
    B(this, "pending", /* @__PURE__ */ new Map());
    /** @type {{type: number, bytes: Uint8Array}[]} */
    B(this, "inboundQueue", []);
    /** @type {number} */
    B(this, "nextRequestId", 1);
    /** @type {boolean} */
    B(this, "streamingPut", !1);
    /** @type {OffsPutOptions|null} */
    B(this, "streamOptions", null);
    /** @type {boolean} */
    B(this, "connected", !1);
    this.url = e, this.apiKey = r, this.config = { ...Dr(), ...n }, this.transport = (n == null ? void 0 : n.transport) || Fr(e, r, n), this.transport.setMessageHandler(this._onMessage.bind(this));
  }
  /**
   * @returns {Promise<void>}
   */
  async connect() {
    await this.transport.connect(), this.connected = !0;
  }
  disconnect() {
    this.transport.disconnect(), this.connected = !1;
    for (const e of this.pending.values())
      e.reject(new Error("Client disconnected"));
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
  _request(e, r, n) {
    return new Promise((s, o) => {
      const l = {
        id: e,
        type: r,
        resolve: s,
        reject: o,
        timer: setTimeout(() => {
          this.pending.delete(e), o(new Error("Request timeout"));
        }, n || this.config.requestTimeoutMs)
      };
      this.pending.set(e, l);
    });
  }
  /**
   * @param {number|number[]} type
   * @param {number} [timeoutMs]
   * @returns {Promise<Uint8Array>}
   */
  _waitForResponse(e, r) {
    const n = this.nextRequestId++, s = this._request(n, e, r), o = this._dequeueMatching(e);
    return o !== null && this._resolve(n, o), s;
  }
  /**
   * @param {number} id
   * @param {any} value
   */
  _resolve(e, r) {
    const n = this.pending.get(e);
    n && (n.timer && clearTimeout(n.timer), this.pending.delete(e), n.resolve(r));
  }
  /**
   * @param {number} id
   * @param {any} reason
   */
  _reject(e, r) {
    const n = this.pending.get(e);
    n && (n.timer && clearTimeout(n.timer), this.pending.delete(e), n.reject(r));
  }
  /**
   * @param {number} type
   * @param {Uint8Array} bytes
   */
  _onMessage(e, r) {
    if (e === E.ERROR) {
      const n = Pt(r);
      if (n)
        for (const s of this.pending.values())
          this._reject(s.id, new Error(`Server error ${n.statusCode}: ${n.message}`));
      return;
    }
    for (const n of this.pending.values())
      if (Array.isArray(n.type) ? n.type.includes(e) : n.type === e) {
        this._resolve(n.id, r);
        return;
      }
    this.inboundQueue.push({ type: e, bytes: r });
  }
  /**
   * @param {number|number[]} type
   * @returns {Uint8Array|null}
   */
  _dequeueMatching(e) {
    const r = Array.isArray(e) ? e : [e], n = this.inboundQueue.findIndex((o) => r.includes(o.type));
    if (n === -1) return null;
    const s = this.inboundQueue[n];
    return this.inboundQueue.splice(n, 1), s.bytes;
  }
  /**
   * Send a CBOR message and wait for a matching response type.
   * @param {Uint8Array} bytes
   * @param {number} responseType
   * @param {number} [timeoutMs]
   * @returns {Promise<Uint8Array>}
   */
  async _sendAndWait(e, r, n) {
    const s = this.nextRequestId++, o = this._request(s, r, n);
    return await this.transport.send(e), o;
  }
  /**
   * @param {string|OffsPutOptions} options
   * @param {Uint8Array|undefined} data
   * @returns {Promise<{oriString: string}>}
   */
  async put(e, r) {
    if (typeof e == "string")
      throw new Error("Use object options (contentType, fileName, streamLength)");
    if (this.transport instanceof D) {
      const o = r || new Uint8Array(0);
      return this.transport.put(e, o);
    }
    const n = ve(e, r), s = await this._sendAndWait(n, E.PUT_RESPONSE);
    return Ve(s);
  }
  /**
   * @param {OffsPutOptions} options
   * @returns {Promise<void>}
   */
  async putStreamStart(e) {
    if (this.streamingPut = !0, this.streamOptions = e, this.transport instanceof D)
      return;
    const r = ve(e);
    await this.transport.send(r);
  }
  /**
   * @param {Uint8Array} chunk
   * @returns {Promise<void>}
   */
  async putStreamData(e) {
    if (this.transport instanceof D)
      throw new Error("HTTP transport does not support putStreamData; use put with ReadableStream");
    await this.transport.send(_t(e));
  }
  /**
   * @returns {Promise<{oriString: string}>}
   */
  async putStreamEnd() {
    this.streamingPut = !1;
    const e = this.streamOptions;
    if (this.streamOptions = null, this.transport instanceof D) {
      if (!e) throw new Error("No stream in progress");
      return this.transport.put(e, new Uint8Array(0));
    }
    await this.transport.send(Rt());
    const r = await this._request(this.nextRequestId - 1, E.PUT_RESPONSE);
    return Ve(r);
  }
  /**
   * @param {string} oriString
   * @param {OffsGetCallbacks} callbacks
   * @param {{start?: number, end?: number}} [range]
   */
  async get(e, r, n) {
    var p, y;
    if (this.transport instanceof D)
      return this.transport.get(e, r);
    const s = Ot(e, n), o = await this._sendAndWait(s, E.GET_RESPONSE_START), l = Tt(o);
    for ((p = r.onStart) == null || p.call(r, l.contentType, l.contentLength, l.hasRange, l.rangeStart, l.rangeEnd); ; ) {
      const _ = await this._waitForResponse([E.GET_DATA, E.GET_END]);
      if (Ut(_)) break;
      const g = At(_);
      r.onData(g);
    }
    (y = r.onEnd) == null || y.call(r);
  }
  /**
   * @param {Uint8Array} data
   * @param {number} [encoding=0]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(e, r = 0) {
    if (this.transport instanceof D)
      return this.transport.blockPut(e, r);
    const n = Nt(e, r), s = await this._sendAndWait(n, E.BLOCK_PUT_RESPONSE);
    return Bt(s);
  }
  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number, data: Uint8Array}>}
   */
  async blockGet(e) {
    if (typeof e == "string") return this.transport.blockGet(e);
    if (this.transport instanceof D)
      return this.transport.blockGet(De(e));
    const r = Ct(e), n = await this._sendAndWait(r, E.BLOCK_GET_RESPONSE);
    return It(n);
  }
  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(e) {
    if (typeof e == "string") return this.transport.blockDelete(e);
    if (this.transport instanceof D)
      return this.transport.blockDelete(De(e));
    const r = kt(e), n = await this._sendAndWait(r, E.BLOCK_DELETE_RESPONSE);
    return Dt(n);
  }
  /**
   * @returns {Promise<any>}
   */
  async health() {
    if (this.transport instanceof D)
      return this.transport.health();
    const e = Ft(), r = await this._sendAndWait(e, E.HEALTH_RESPONSE), { json: n } = Lt(r);
    return JSON.parse(n);
  }
  /**
   * @param {string} [format='cbor']
   * @returns {Promise<{format: number, data: Uint8Array}>}
   */
  async peerInfo(e = "cbor") {
    if (this.transport instanceof D)
      return this.transport.peerInfo(e);
    const r = Mt(), n = await this._sendAndWait(r, E.PEER_INFO_RESPONSE);
    return Ht(n);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(e, r = 0) {
    if (this.transport instanceof D)
      return this.transport.peerConnect(e, r);
    const n = jt(r, e), s = await this._sendAndWait(n, E.PEER_CONNECT_RESULT);
    return Kt(s);
  }
  /**
   * Convert an OFF URL/URI string into an HTTP URL usable by a browser.
   * @param {string} oriString
   * @param {string} [baseUrl]
   * @returns {string}
   */
  static offUrlToHttpUrl(e, r) {
    return Pr(e, r);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    if (this.transport instanceof D)
      return this.transport.peerList();
    const e = Gt(), r = await this._sendAndWait(e, E.PEER_LIST_RESPONSE);
    return qt(r);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(e, r = 0) {
    if (this.transport instanceof D)
      return this.transport.friendAdd(e, r);
    const n = $t(r, e);
    await this.transport.send(n);
  }
  /**
   * @param {string|Uint8Array} nodeId
   * @returns {Promise<void>}
   */
  async friendRemove(e) {
    if (this.transport instanceof D)
      return this.transport.friendRemove(typeof e == "string" ? e : De(e));
    const r = typeof e == "string" ? new TextEncoder().encode(e) : e, n = vt(r);
    await this.transport.send(n);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    if (this.transport instanceof D)
      return this.transport.friendList();
    const e = Vt(), r = await this._sendAndWait(e, E.FRIEND_LIST_RESPONSE);
    return Wt(r);
  }
  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    if (this.transport instanceof D)
      return this.transport.configShow();
    const e = zt(), r = await this._sendAndWait(e, E.CONFIG_SHOW_RESPONSE), { json: n } = Qt(r);
    return JSON.parse(n);
  }
  /**
   * @param {string} field
   * @param {string} value
   * @returns {Promise<{status: number, restartRequired: boolean, message: string}>}
   */
  async configSet(e, r) {
    if (this.transport instanceof D)
      return this.transport.configSet(e, r);
    const n = Jt(e, r), s = await this._sendAndWait(n, E.CONFIG_SET_RESPONSE);
    return Zt(s);
  }
  /**
   * @returns {Promise<{status: number, message: string}>}
   */
  async configReload() {
    if (this.transport instanceof D)
      return this.transport.configReload();
    const e = Yt(), r = await this._sendAndWait(e, E.CONFIG_RELOAD_RESPONSE);
    return Xt(r);
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
  async putFolder(e, r = {}) {
    const n = Ur(e);
    if (n.length === 0)
      throw new Error("No files to upload");
    const s = r.recyclerUrls || [], o = n.length;
    let l = 0;
    const p = (g) => {
      var P;
      l++, (P = r.onProgress) == null || P.call(r, g, l, o);
    }, y = Lr(n.map((g) => g.path)), _ = async (g) => {
      const P = g ? g.split("/").pop() : y.split("/").pop() || "root", G = Mr(n, g), V = Hr(n, g), I = [];
      for (const C of V) {
        const X = (await _(C)).oriString, J = ht(X);
        if (!J)
          throw new Error(`Failed to parse subdirectory URL: ${X}`);
        const v = xe(J.fileHashB58);
        if (!v)
          throw new Error(`Invalid directory hash in URL: ${X}`);
        I.push(Ir({
          name: C.split("/").pop() || C,
          dirHash: v
        }));
      }
      for (const C of G) {
        const S = C.path.split("/").pop() || C.path, X = Tr(S), J = C.file.size;
        let v;
        if (this.transport instanceof D) {
          const se = pt(C.file);
          v = (await this.put({
            contentType: X,
            fileName: S,
            streamLength: J,
            serverAddress: r.serverAddress,
            recyclerUrls: s,
            temporary: r.temporary
          }, se)).oriString;
        } else {
          await this.putStreamStart({
            contentType: X,
            fileName: S,
            streamLength: J,
            serverAddress: r.serverAddress,
            recyclerUrls: s,
            temporary: r.temporary
          });
          const se = pt(C.file).getReader();
          for (; ; ) {
            const { done: _e, value: a } = await se.read();
            if (_e) break;
            await this.putStreamData(a);
          }
          v = (await this.putStreamEnd()).oriString;
        }
        const ae = ht(v);
        if (!ae)
          throw new Error(`Failed to parse file URL: ${v}`);
        const ne = xe(ae.fileHashB58), ue = xe(ae.descriptorHashB58);
        if (!ne || !ue)
          throw new Error(`Invalid hash in file URL: ${v}`);
        I.push(Cr({
          name: S,
          fileHash: ne,
          descriptorHash: ue,
          finalByte: ae.streamLength
        })), p(S);
      }
      if (I.length === 0)
        throw new Error(`Empty directory: ${g || y}`);
      const q = kr(I), re = `${P}.ofd`;
      return this.transport instanceof D ? this.put({
        contentType: "offsystem/directory",
        fileName: re,
        streamLength: q.length,
        serverAddress: r.serverAddress,
        recyclerUrls: s,
        temporary: r.temporary
      }, q) : (await this.putStreamStart({
        contentType: "offsystem/directory",
        fileName: re,
        streamLength: q.length,
        serverAddress: r.serverAddress,
        recyclerUrls: s,
        temporary: r.temporary
      }), await this.putStreamData(q), this.putStreamEnd());
    };
    return _(y);
  }
}
function Lr(t) {
  if (t.length === 0) return "";
  const e = t.map((o) => o.split("/").filter(Boolean)), r = e[0];
  let n = r.length;
  for (let o = 1; o < e.length; o++) {
    const l = e[o];
    let p = 0;
    for (; p < Math.min(n, l.length) && r[p] === l[p]; )
      p++;
    if (n = p, n === 0) break;
  }
  const s = Math.min(n, r.length - 1);
  return r.slice(0, s).join("/");
}
function Mr(t, e) {
  const r = e ? `${e}/` : "";
  return t.filter((n) => {
    if (!n.path.startsWith(r)) return !1;
    const s = n.path.slice(r.length);
    return s.length > 0 && !s.includes("/");
  });
}
function Hr(t, e) {
  const r = e ? `${e}/` : "", n = /* @__PURE__ */ new Set();
  for (const s of t) {
    if (!s.path.startsWith(r)) continue;
    const o = s.path.slice(r.length);
    if (!o) continue;
    const l = o.indexOf("/");
    l > 0 && n.add(r + o.slice(0, l));
  }
  return Array.from(n);
}
var Fe = globalThis.OffsClient;
Fe && Fe.OffsClient && (globalThis.OffsClient = Fe.OffsClient);
export {
  Gr as OffsClient,
  xe as base58Decode,
  De as base58Encode,
  Tr as mimeFromExtension,
  Pr as offUrlToHttpUrl,
  ht as parseOffUrl,
  Kr as wire
};
//# sourceMappingURL=offs-client.esm.js.map
