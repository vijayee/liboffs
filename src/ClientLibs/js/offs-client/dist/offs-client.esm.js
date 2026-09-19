var ur = Object.defineProperty;
var dr = (t, e, r) => e in t ? ur(t, e, { enumerable: !0, configurable: !0, writable: !0, value: r }) : t[e] = r;
var C = (t, e, r) => dr(t, typeof e != "symbol" ? e + "" : e, r);
let He;
try {
  He = new TextDecoder();
} catch {
}
let w, ce, c = 0;
const hr = 105, pr = 57342, Er = 57343, ot = 57337, at = 6, he = {};
let ge = 11281e4, ae = 1681e4, A = {}, M, Ne, Le = 0, me = 0, K, Z, j = [], je = [], z, W, xe, ft = {
  useRecords: !1,
  mapsAsObjects: !0
}, Se = !1, xt = 2;
try {
  new Function("");
} catch {
  xt = 1 / 0;
}
class we {
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
      return Rt(() => (Ge(), this ? this.decode(e, r) : we.prototype.decode.call(ft, e, r)));
    ce = r > -1 ? r : e.length, c = 0, me = 0, Ne = null, K = null, w = e;
    try {
      W = e.dataView || (e.dataView = new DataView(e.buffer, e.byteOffset, e.byteLength));
    } catch (n) {
      throw w = null, e instanceof Uint8Array ? n : new Error("Source must be a Uint8Array or Buffer but was a " + (e && typeof e == "object" ? e.constructor.name : typeof e));
    }
    if (this instanceof we) {
      if (A = this, z = this.sharedValues && (this.pack ? new Array(this.maxPrivatePackedValues || 16).concat(this.sharedValues) : this.sharedValues), this.structures)
        return M = this.structures, Te();
      (!M || M.length > 0) && (M = []);
    } else
      A = ft, (!M || M.length > 0) && (M = []), z = null;
    return Te();
  }
  decodeMultiple(e, r) {
    let n, s = 0;
    try {
      let i = e.length;
      Se = !0;
      let l = this ? this.decode(e, i) : Xe.decode(e, i);
      if (r) {
        if (r(l) === !1)
          return;
        for (; c < i; )
          if (s = c, r(Te()) === !1)
            return;
      } else {
        for (n = [l]; c < i; )
          s = c, n.push(Te());
        return n;
      }
    } catch (i) {
      throw i.lastPosition = s, i.values = n, i;
    } finally {
      Se = !1, Ge();
    }
  }
}
function Te() {
  try {
    let t = b();
    if (K) {
      if (c >= K.postBundlePosition) {
        let e = new Error("Unexpected bundle position");
        throw e.incomplete = !0, e;
      }
      c = K.postBundlePosition, K = null;
    }
    if (c == ce)
      M = null, w = null, Z && (Z = null);
    else if (c > ce) {
      let e = new Error("Unexpected end of CBOR data");
      throw e.incomplete = !0, e;
    } else if (!Se)
      throw new Error("Data read, but end of buffer not reached");
    return t;
  } catch (t) {
    throw Ge(), (t instanceof RangeError || t.message.startsWith("Unexpected end of buffer")) && (t.incomplete = !0), t;
  }
}
function b() {
  let t = w[c++], e = t >> 5;
  if (t = t & 31, t > 23)
    switch (t) {
      case 24:
        t = w[c++];
        break;
      case 25:
        if (e == 7)
          return xr();
        t = W.getUint16(c), c += 2;
        break;
      case 26:
        if (e == 7) {
          let r = W.getFloat32(c);
          if (A.useFloat32 > 2) {
            let n = Ye[(w[c] & 127) << 1 | w[c + 1] >> 7];
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
        } else A.int64AsNumber ? (t = W.getUint32(c) * 4294967296, t += W.getUint32(c + 4)) : t = W.getBigUint64(c);
        c += 8;
        break;
      case 31:
        switch (e) {
          case 2:
          case 3:
            throw new Error("Indefinite length not supported for byte or text strings");
          case 4:
            let r = [], n, s = 0;
            for (; (n = b()) != he; ) {
              if (s >= ge) throw new Error(`Array length exceeds ${ge}`);
              r[s++] = n;
            }
            return e == 4 ? r : e == 3 ? r.join("") : Buffer.concat(r);
          case 5:
            let i;
            if (A.mapsAsObjects) {
              let l = {}, h = 0;
              if (A.keyMap)
                for (; (i = b()) != he; ) {
                  if (h++ >= ae) throw new Error(`Property count exceeds ${ae}`);
                  l[Y(A.decodeKey(i))] = b();
                }
              else
                for (; (i = b()) != he; ) {
                  if (h++ >= ae) throw new Error(`Property count exceeds ${ae}`);
                  l[Y(i)] = b();
                }
              return l;
            } else {
              xe && (A.mapsAsObjects = !0, xe = !1);
              let l = /* @__PURE__ */ new Map();
              if (A.keyMap) {
                let h = 0;
                for (; (i = b()) != he; ) {
                  if (h++ >= ae)
                    throw new Error(`Map size exceeds ${ae}`);
                  l.set(A.decodeKey(i), b());
                }
              } else {
                let h = 0;
                for (; (i = b()) != he; ) {
                  if (h++ >= ae)
                    throw new Error(`Map size exceeds ${ae}`);
                  l.set(i, b());
                }
              }
              return l;
            }
          case 7:
            return he;
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
      return gr(t);
    case 3:
      if (me >= c)
        return Ne.slice(c - Le, (c += t) - Le);
      if (me == 0 && ce < 140 && t < 32) {
        let s = t < 16 ? mt(t) : wr(t);
        if (s != null)
          return s;
      }
      return yr(t);
    case 4:
      if (t >= ge) throw new Error(`Array length exceeds ${ge}`);
      let r = new Array(t);
      for (let s = 0; s < t; s++) r[s] = b();
      return r;
    case 5:
      if (t >= ae) throw new Error(`Map size exceeds ${ge}`);
      if (A.mapsAsObjects) {
        let s = {};
        if (A.keyMap) for (let i = 0; i < t; i++) s[Y(A.decodeKey(b()))] = b();
        else for (let i = 0; i < t; i++) s[Y(b())] = b();
        return s;
      } else {
        xe && (A.mapsAsObjects = !0, xe = !1);
        let s = /* @__PURE__ */ new Map();
        if (A.keyMap) for (let i = 0; i < t; i++) s.set(A.decodeKey(b()), b());
        else for (let i = 0; i < t; i++) s.set(b(), b());
        return s;
      }
    case 6:
      if (t >= ot) {
        let s = M[t & 8191];
        if (s)
          return s.read || (s.read = qe(s)), s.read();
        if (t < 65536) {
          if (t == Er) {
            let i = Ee(), l = b(), h = b();
            Ke(l, h);
            let E = {};
            if (A.keyMap) for (let m = 2; m < i; m++) {
              let x = A.decodeKey(h[m - 2]);
              E[Y(x)] = b();
            }
            else for (let m = 2; m < i; m++) {
              let x = h[m - 2];
              E[Y(x)] = b();
            }
            return E;
          } else if (t == pr) {
            let i = Ee(), l = b();
            for (let h = 2; h < i; h++)
              Ke(l++, b());
            return b();
          } else if (t == ot)
            return Tr();
          if (A.getShared && (Ze(), s = M[t & 8191], s))
            return s.read || (s.read = qe(s)), s.read();
        }
      }
      let n = j[t];
      if (n)
        return n.handlesRead ? n(b) : n(b());
      {
        let s = b();
        for (let i = 0; i < je.length; i++) {
          let l = je[i](t, s);
          if (l !== void 0)
            return l;
        }
        return new ue(s, t);
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
          let s = (z || le())[t];
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
const lt = /^[a-zA-Z_$][a-zA-Z\d_$]*$/;
function qe(t) {
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
        return n(b);
      n = n.next;
    }
    if (this.slowReads++ >= xt) {
      let i = this.length == r ? this : this.slice(0, r);
      return n = A.keyMap ? new Function("r", "return {" + i.map((l) => A.decodeKey(l)).map((l) => lt.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}") : new Function("r", "return {" + i.map((l) => lt.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}"), this.compiledReader && (n.next = this.compiledReader), n.propertyCount = r, this.compiledReader = n, n(b);
    }
    let s = {};
    if (A.keyMap) for (let i = 0; i < r; i++) s[Y(A.decodeKey(this[i]))] = b();
    else for (let i = 0; i < r; i++)
      s[Y(this[i])] = b();
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
let yr = $e;
function $e(t) {
  let e;
  if (t < 16 && (e = mt(t)))
    return e;
  if (t > 64 && He)
    return He.decode(w.subarray(c, c += t));
  const r = c + t, n = [];
  for (e = ""; c < r; ) {
    const s = w[c++];
    if (!(s & 128))
      n.push(s);
    else if ((s & 224) === 192)
      if (s < 194 || c >= r || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const i = w[c++] & 63;
        n.push((s & 31) << 6 | i);
      }
    else if ((s & 240) === 224) {
      const i = c < r ? w[c] : 0;
      if (c >= r || (i & 192) !== 128 || s === 224 && i < 160 || s === 237 && i >= 160)
        n.push(65533);
      else if (c++, c >= r || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = w[c++] & 63;
        n.push((s & 31) << 12 | (i & 63) << 6 | l);
      }
    } else if ((s & 248) === 240) {
      const i = c < r ? w[c] : 0;
      if (s > 244 || c >= r || (i & 192) !== 128 || s === 240 && i < 144 || s === 244 && i >= 144)
        n.push(65533);
      else if (c++, c >= r || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = w[c++] & 63;
        if (c >= r || (w[c] & 192) !== 128)
          n.push(65533);
        else {
          const h = w[c++] & 63;
          let E = (s & 7) << 18 | (i & 63) << 12 | l << 6 | h;
          E -= 65536, n.push(E >>> 10 & 1023 | 55296), n.push(56320 | E & 1023);
        }
      }
    } else
      n.push(65533);
    n.length >= 4096 && (e += G.apply(String, n), n.length = 0);
  }
  return n.length > 0 && (e += G.apply(String, n)), e;
}
let G = String.fromCharCode;
function wr(t) {
  let e = c, r = new Array(t);
  for (let n = 0; n < t; n++) {
    const s = w[c++];
    if ((s & 128) > 0) {
      c = e;
      return;
    }
    r[n] = s;
  }
  return G.apply(String, r);
}
function mt(t) {
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
        return G(e);
      }
    } else {
      let e = w[c++], r = w[c++];
      if ((e & 128) > 0 || (r & 128) > 0) {
        c -= 2;
        return;
      }
      if (t < 3)
        return G(e, r);
      let n = w[c++];
      if ((n & 128) > 0) {
        c -= 3;
        return;
      }
      return G(e, r, n);
    }
  else {
    let e = w[c++], r = w[c++], n = w[c++], s = w[c++];
    if ((e & 128) > 0 || (r & 128) > 0 || (n & 128) > 0 || (s & 128) > 0) {
      c -= 4;
      return;
    }
    if (t < 6) {
      if (t === 4)
        return G(e, r, n, s);
      {
        let i = w[c++];
        if ((i & 128) > 0) {
          c -= 5;
          return;
        }
        return G(e, r, n, s, i);
      }
    } else if (t < 8) {
      let i = w[c++], l = w[c++];
      if ((i & 128) > 0 || (l & 128) > 0) {
        c -= 6;
        return;
      }
      if (t < 7)
        return G(e, r, n, s, i, l);
      let h = w[c++];
      if ((h & 128) > 0) {
        c -= 7;
        return;
      }
      return G(e, r, n, s, i, l, h);
    } else {
      let i = w[c++], l = w[c++], h = w[c++], E = w[c++];
      if ((i & 128) > 0 || (l & 128) > 0 || (h & 128) > 0 || (E & 128) > 0) {
        c -= 8;
        return;
      }
      if (t < 10) {
        if (t === 8)
          return G(e, r, n, s, i, l, h, E);
        {
          let m = w[c++];
          if ((m & 128) > 0) {
            c -= 9;
            return;
          }
          return G(e, r, n, s, i, l, h, E, m);
        }
      } else if (t < 12) {
        let m = w[c++], x = w[c++];
        if ((m & 128) > 0 || (x & 128) > 0) {
          c -= 10;
          return;
        }
        if (t < 11)
          return G(e, r, n, s, i, l, h, E, m, x);
        let U = w[c++];
        if ((U & 128) > 0) {
          c -= 11;
          return;
        }
        return G(e, r, n, s, i, l, h, E, m, x, U);
      } else {
        let m = w[c++], x = w[c++], U = w[c++], L = w[c++];
        if ((m & 128) > 0 || (x & 128) > 0 || (U & 128) > 0 || (L & 128) > 0) {
          c -= 12;
          return;
        }
        if (t < 14) {
          if (t === 12)
            return G(e, r, n, s, i, l, h, E, m, x, U, L);
          {
            let q = w[c++];
            if ((q & 128) > 0) {
              c -= 13;
              return;
            }
            return G(e, r, n, s, i, l, h, E, m, x, U, L, q);
          }
        } else {
          let q = w[c++], v = w[c++];
          if ((q & 128) > 0 || (v & 128) > 0) {
            c -= 14;
            return;
          }
          if (t < 15)
            return G(e, r, n, s, i, l, h, E, m, x, U, L, q, v);
          let B = w[c++];
          if ((B & 128) > 0) {
            c -= 15;
            return;
          }
          return G(e, r, n, s, i, l, h, E, m, x, U, L, q, v, B);
        }
      }
    }
  }
}
function gr(t) {
  return A.copyBuffers ? (
    // specifically use the copying slice (not the node one)
    Uint8Array.prototype.slice.call(w, c, c += t)
  ) : w.subarray(c, c += t);
}
let St = new Float32Array(1), Ae = new Uint8Array(St.buffer, 0, 4);
function xr() {
  let t = w[c++], e = w[c++], r = (t & 127) >> 2;
  if (r === 31)
    return e || t & 3 ? NaN : t & 128 ? -1 / 0 : 1 / 0;
  if (r === 0) {
    let n = ((t & 3) << 8 | e) / 16777216;
    return t & 128 ? -n : n;
  }
  return Ae[3] = t & 128 | // sign bit
  (r >> 1) + 56, Ae[2] = (t & 7) << 5 | // last exponent bit and first two mantissa bits
  e >> 3, Ae[1] = e << 5, Ae[0] = 0, St[0];
}
new Array(4096);
class ue {
  constructor(e, r) {
    this.value = e, this.tag = r;
  }
}
j[0] = (t) => new Date(t);
j[1] = (t) => new Date(Math.round(t * 1e3));
j[2] = (t) => {
  let e = BigInt(0);
  for (let r = 0, n = t.byteLength; r < n; r++)
    e = BigInt(t[r]) + (e << BigInt(8));
  return e;
};
j[3] = (t) => BigInt(-1) - j[2](t);
j[4] = (t) => +(t[1] + "e" + t[0]);
j[5] = (t) => t[1] * Math.exp(t[0] * Math.log(2));
const Ke = (t, e) => {
  t = t - 57344;
  let r = M[t];
  r && r.isShared && ((M.restoreStructures || (M.restoreStructures = []))[t] = r), M[t] = e, e.read = qe(e);
};
j[hr] = (t) => {
  let e = t.length, r = t[1];
  Ke(t[0], r);
  let n = {};
  for (let s = 2; s < e; s++) {
    let i = r[s - 2];
    n[Y(i)] = t[s];
  }
  return n;
};
j[14] = (t) => K ? K[0].slice(K.position0, K.position0 += t) : new ue(t, 14);
j[15] = (t) => K ? K[1].slice(K.position1, K.position1 += t) : new ue(t, 15);
let mr = { Error, RegExp };
j[27] = (t) => (mr[t[0]] || Error)(t[1], t[2]);
const _t = (t) => {
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
_t.handlesRead = !0;
j[51] = _t;
j[at] = (t) => {
  if (!z)
    if (A.getShared)
      Ze();
    else
      return new ue(t, at);
  if (typeof t == "number")
    return z[16 + (t >= 0 ? 2 * t : -2 * t - 1)];
  let e = new Error("No support for non-integer packed references yet");
  throw t === void 0 && (e.incomplete = !0), e;
};
j[28] = (t) => {
  Z || (Z = /* @__PURE__ */ new Map(), Z.id = 0);
  let e = Z.id++, r = c, n = w[c], s;
  n >> 5 == 4 ? s = [] : s = {};
  let i = { target: s };
  Z.set(e, i);
  let l = t();
  return i.used ? (Object.getPrototypeOf(s) !== Object.getPrototypeOf(l) && (c = r, s = l, Z.set(e, { target: s }), l = t()), Object.assign(s, l)) : (i.target = l, l);
};
j[28].handlesRead = !0;
j[29] = (t) => {
  let e = Z.get(t);
  return e.used = !0, e.target;
};
j[258] = (t) => new Set(t);
(j[259] = (t) => (A.mapsAsObjects && (A.mapsAsObjects = !1, xe = !0), t())).handlesRead = !0;
function pe(t, e) {
  return typeof t == "string" ? t + e : t instanceof Array ? t.concat(e) : Object.assign({}, t, e);
}
function le() {
  if (!z)
    if (A.getShared)
      Ze();
    else
      throw new Error("No packed values available");
  return z;
}
const Sr = 1399353956;
je.push((t, e) => {
  if (t >= 225 && t <= 255)
    return pe(le().prefixes[t - 224], e);
  if (t >= 28704 && t <= 32767)
    return pe(le().prefixes[t - 28672], e);
  if (t >= 1879052288 && t <= 2147483647)
    return pe(le().prefixes[t - 1879048192], e);
  if (t >= 216 && t <= 223)
    return pe(e, le().suffixes[t - 216]);
  if (t >= 27647 && t <= 28671)
    return pe(e, le().suffixes[t - 27639]);
  if (t >= 1811940352 && t <= 1879048191)
    return pe(e, le().suffixes[t - 1811939328]);
  if (t == Sr)
    return {
      packedValues: z,
      structures: M.slice(0),
      version: e
    };
  if (t == 55799)
    return e;
});
const _r = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1, ct = [
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
], Rr = [64, 68, 69, 70, 71, 72, 77, 78, 79, 85, 86];
for (let t = 0; t < ct.length; t++)
  Or(ct[t], Rr[t]);
function Or(t, e) {
  let r = "get" + t.name.slice(0, -5), n;
  typeof t == "function" ? n = t.BYTES_PER_ELEMENT : t = null;
  for (let s = 0; s < 2; s++) {
    if (!s && n == 1)
      continue;
    let i = n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : 0;
    j[s ? e : e - 4] = n == 1 || s == _r ? (l) => {
      if (!t)
        throw new Error("Could not find typed array for code " + e);
      return !A.copyBuffers && (n === 1 || n === 2 && !(l.byteOffset & 1) || n === 4 && !(l.byteOffset & 3) || n === 8 && !(l.byteOffset & 7)) ? new t(l.buffer, l.byteOffset, l.byteLength >> i) : new t(Uint8Array.prototype.slice.call(l, 0).buffer);
    } : (l) => {
      if (!t)
        throw new Error("Could not find typed array for code " + e);
      let h = new DataView(l.buffer, l.byteOffset, l.byteLength), E = l.length >> i, m = new t(E), x = h[r];
      for (let U = 0; U < E; U++)
        m[U] = x.call(h, U << i, s);
      return m;
    };
  }
}
function Tr() {
  let t = Ee(), e = c + b();
  for (let n = 2; n < t; n++) {
    let s = Ee();
    c += s;
  }
  let r = c;
  return c = e, K = [$e(Ee()), $e(Ee())], K.position0 = 0, K.position1 = 0, K.postBundlePosition = c, c = r, b();
}
function Ee() {
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
function Ze() {
  if (A.getShared) {
    let t = Rt(() => (w = null, A.getShared())) || {}, e = t.structures || [];
    A.sharedVersion = t.version, z = A.sharedValues = t.packedValues, M === !0 ? A.structures = M = e : M.splice.apply(M, [0, e.length].concat(e));
  }
}
function Rt(t) {
  let e = ce, r = c, n = Le, s = me, i = Ne, l = Z, h = K, E = new Uint8Array(w.slice(0, ce)), m = M, x = A, U = Se, L = t();
  return ce = e, c = r, Le = n, me = s, Ne = i, Z = l, K = h, w = E, Se = U, M = m, A = x, W = new DataView(w.buffer, w.byteOffset, w.byteLength), L;
}
function Ge() {
  w = null, Z = null, M = null;
}
const Ye = new Array(147);
for (let t = 0; t < 256; t++)
  Ye[t] = +("1e" + Math.floor(45.15 - t * 0.30103));
let Xe = new we({ useRecords: !1 });
const k = Xe.decode;
Xe.decodeMultiple;
let Pe;
try {
  Pe = new TextEncoder();
} catch {
}
let ve, Ot;
const Be = typeof globalThis == "object" && globalThis.Buffer, _e = typeof Be < "u", Ie = _e ? Be.allocUnsafeSlow : Uint8Array, ut = _e ? Be : Uint8Array, dt = 256, ht = _e ? 4294967296 : 2144337920;
let Ce, f, I, o = 0, fe, $ = null;
const Ar = 61440, br = /[\u0080-\uFFFF]/, J = Symbol("record-id");
class et extends we {
  constructor(e) {
    super(e), this.offset = 0;
    let r, n, s, i, l;
    e = e || {};
    let h = ut.prototype.utf8Write ? function(a, g) {
      return f.utf8Write(a, g, f.byteLength - g);
    } : Pe && Pe.encodeInto ? function(a, g) {
      return Pe.encodeInto(a, f.subarray(g)).written;
    } : !1, E = this, m = e.structures || e.saveStructures, x = e.maxSharedStructures;
    if (x == null && (x = m ? 128 : 0), x > 8190)
      throw new Error("Maximum maxSharedStructure is 8190");
    let U = e.sequential;
    U && (x = 0), this.structures || (this.structures = []), this.saveStructures && (this.saveShared = this.saveStructures);
    let L, q, v = e.sharedValues, B;
    if (v) {
      B = /* @__PURE__ */ Object.create(null);
      for (let a = 0, g = v.length; a < g; a++)
        B[v[a]] = a;
    }
    let H = [], se = 0, F = 0;
    this.mapEncode = function(a, g) {
      if (this._keyMap && !this._mapped)
        switch (a.constructor.name) {
          case "Array":
            a = a.map((d) => this.encodeKeys(d));
            break;
        }
      return this.encode(a, g);
    }, this.encode = function(a, g) {
      if (f || (f = new Ie(8192), I = new DataView(f.buffer, 0, 8192), o = 0), fe = f.length - 10, fe - o < 2048 ? (f = new Ie(f.length), I = new DataView(f.buffer, 0, f.length), fe = f.length - 10, o = 0) : g === yt && (o = o + 7 & 2147483640), r = o, E.useSelfDescribedHeader && (I.setUint32(o, 3654940416), o += 3), l = E.structuredClone ? /* @__PURE__ */ new Map() : null, E.bundleStrings && typeof a != "string" ? ($ = [], $.size = 1 / 0) : $ = null, n = E.structures, n) {
        if (n.uninitialized) {
          let p = E.getShared() || {};
          E.structures = n = p.structures || [], E.sharedVersion = p.version;
          let u = E.sharedValues = p.packedValues;
          if (u) {
            B = {};
            for (let S = 0, R = u.length; S < R; S++)
              B[u[S]] = S;
          }
        }
        let d = n.length;
        if (d > x && !U && (d = x), !n.transitions) {
          n.transitions = /* @__PURE__ */ Object.create(null);
          for (let p = 0; p < d; p++) {
            let u = n[p];
            if (!u)
              continue;
            let S, R = n.transitions;
            for (let O = 0, T = u.length; O < T; O++) {
              R[J] === void 0 && (R[J] = p);
              let P = u[O];
              S = R[P], S || (S = R[P] = /* @__PURE__ */ Object.create(null)), R = S;
            }
            R[J] = p | 1048576;
          }
        }
        U || (n.nextId = d);
      }
      if (s && (s = !1), i = n || [], q = B, e.pack) {
        let d = /* @__PURE__ */ new Map();
        if (d.values = [], d.encoder = E, d.maxValues = e.maxPrivatePackedValues || (B ? 16 : 1 / 0), d.objectMap = B || !1, d.samplingPackedValues = L, Ue(a, d), d.values.length > 0) {
          f[o++] = 216, f[o++] = 51, re(4);
          let p = d.values;
          _(p), re(0), re(0), q = Object.create(B || null);
          for (let u = 0, S = p.length; u < S; u++)
            q[p[u]] = u;
        }
      }
      Ce = g & Fe;
      try {
        if (Ce)
          return;
        if (_(a), $ && Et(r, _), E.offset = o, l && l.idsToInsert) {
          o += l.idsToInsert.length * 2, o > fe && V(o), E.offset = o;
          let d = Nr(f.subarray(r, o), l.idsToInsert);
          return l = null, d;
        }
        return g & yt ? (f.start = r, f.end = o, f) : f.subarray(r, o);
      } finally {
        if (n) {
          if (F < 10 && F++, n.length > x && (n.length = x), se > 1e4)
            n.transitions = null, F = 0, se = 0, H.length > 0 && (H = []);
          else if (H.length > 0 && !U) {
            for (let d = 0, p = H.length; d < p; d++)
              H[d][J] = void 0;
            H = [];
          }
        }
        if (s && E.saveShared) {
          E.structures.length > x && (E.structures = E.structures.slice(0, x));
          let d = f.subarray(r, o);
          return E.updateSharedData() === !1 ? E.encode(a) : d;
        }
        g & Lr && (o = r);
      }
    }, this.findCommonStringsToPack = () => (L = /* @__PURE__ */ new Map(), B || (B = /* @__PURE__ */ Object.create(null)), (a) => {
      let g = a && a.threshold || 4, d = this.pack ? a.maxPrivatePackedValues || 16 : 0;
      v || (v = this.sharedValues = []);
      for (let [p, u] of L)
        u.count > g && (B[p] = d++, v.push(p), s = !0);
      for (; this.saveShared && this.updateSharedData() === !1; )
        ;
      L = null;
    });
    const _ = (a) => {
      o > fe && (f = V(o));
      var g = typeof a, d;
      if (g === "string") {
        if (q) {
          let R = q[a];
          if (R >= 0) {
            R < 16 ? f[o++] = R + 224 : (f[o++] = 198, R & 1 ? _(15 - R >> 1) : _(R - 16 >> 1));
            return;
          } else if (L && !e.pack) {
            let O = L.get(a);
            O ? O.count++ : L.set(a, {
              count: 1
            });
          }
        }
        let p = a.length;
        if ($ && p >= 4 && p < 1024) {
          if (($.size += p) > Ar) {
            let O, T = ($[0] ? $[0].length * 3 + $[1].length : 0) + 10;
            o + T > fe && (f = V(o + T)), f[o++] = 217, f[o++] = 223, f[o++] = 249, f[o++] = $.position ? 132 : 130, f[o++] = 26, O = o - r, o += 4, $.position && Et(r, _), $ = ["", ""], $.size = 0, $.position = O;
          }
          let R = br.test(a);
          $[R ? 0 : 1] += a, f[o++] = R ? 206 : 207, _(p);
          return;
        }
        let u;
        p < 32 ? u = 1 : p < 256 ? u = 2 : p < 65536 ? u = 3 : u = 5;
        let S = p * 3;
        if (o + S > fe && (f = V(o + S)), p < 64 || !h) {
          let R, O, T, P = o + u;
          for (R = 0; R < p; R++)
            O = a.charCodeAt(R), O < 128 ? f[P++] = O : O < 2048 ? (f[P++] = O >> 6 | 192, f[P++] = O & 63 | 128) : (O & 64512) === 55296 && ((T = a.charCodeAt(R + 1)) & 64512) === 56320 ? (O = 65536 + ((O & 1023) << 10) + (T & 1023), R++, f[P++] = O >> 18 | 240, f[P++] = O >> 12 & 63 | 128, f[P++] = O >> 6 & 63 | 128, f[P++] = O & 63 | 128) : (f[P++] = O >> 12 | 224, f[P++] = O >> 6 & 63 | 128, f[P++] = O & 63 | 128);
          d = P - o - u;
        } else
          d = h(a, o + u, S);
        d < 24 ? f[o++] = 96 | d : d < 256 ? (u < 2 && f.copyWithin(o + 2, o + 1, o + 1 + d), f[o++] = 120, f[o++] = d) : d < 65536 ? (u < 3 && f.copyWithin(o + 3, o + 2, o + 2 + d), f[o++] = 121, f[o++] = d >> 8, f[o++] = d & 255) : (u < 5 && f.copyWithin(o + 5, o + 3, o + 3 + d), f[o++] = 122, I.setUint32(o, d), o += 4), o += d;
      } else if (g === "number")
        if (!this.alwaysUseFloat && a >>> 0 === a)
          a < 24 ? f[o++] = a : a < 256 ? (f[o++] = 24, f[o++] = a) : a < 65536 ? (f[o++] = 25, f[o++] = a >> 8, f[o++] = a & 255) : (f[o++] = 26, I.setUint32(o, a), o += 4);
        else if (!this.alwaysUseFloat && a >> 0 === a)
          a >= -24 ? f[o++] = 31 - a : a >= -256 ? (f[o++] = 56, f[o++] = ~a) : a >= -65536 ? (f[o++] = 57, I.setUint16(o, ~a), o += 2) : (f[o++] = 58, I.setUint32(o, ~a), o += 4);
        else if (!this.alwaysUseFloat && a < 0 && a >= -4294967296 && Math.floor(a) === a)
          f[o++] = 58, I.setUint32(o, -1 - a), o += 4;
        else {
          let p;
          if ((p = this.useFloat32) > 0 && a < 4294967296 && a >= -2147483648) {
            f[o++] = 250, I.setFloat32(o, a);
            let u;
            if (p < 4 || // this checks for rounding of numbers that were encoded in 32-bit float to nearest significant decimal digit that could be preserved
            (u = a * Ye[(f[o] & 127) << 1 | f[o + 1] >> 7]) >> 0 === u) {
              o += 4;
              return;
            } else
              o--;
          }
          f[o++] = 251, I.setFloat64(o, a), o += 8;
        }
      else if (g === "object")
        if (!a)
          f[o++] = 246;
        else {
          if (l) {
            let u = l.get(a);
            if (u) {
              if (f[o++] = 216, f[o++] = 29, f[o++] = 25, !u.references) {
                let S = l.idsToInsert || (l.idsToInsert = []);
                u.references = [], S.push(u);
              }
              u.references.push(o - r), o += 2;
              return;
            } else
              l.set(a, { offset: o - r });
          }
          let p = a.constructor;
          if (p === Object)
            this.skipFunction === !0 && (a = Object.fromEntries([...Object.keys(a).filter((u) => typeof a[u] != "function").map((u) => [u, a[u]])])), X(a);
          else if (p === Array) {
            d = a.length, d < 24 ? f[o++] = 128 | d : re(d);
            for (let u = 0; u < d; u++)
              _(a[u]);
          } else if (p === Map)
            if ((this.mapsAsObjects ? this.useTag259ForMaps !== !1 : this.useTag259ForMaps) && (f[o++] = 217, f[o++] = 1, f[o++] = 3), d = a.size, d < 24 ? f[o++] = 160 | d : d < 256 ? (f[o++] = 184, f[o++] = d) : d < 65536 ? (f[o++] = 185, f[o++] = d >> 8, f[o++] = d & 255) : (f[o++] = 186, I.setUint32(o, d), o += 4), E.keyMap)
              for (let [u, S] of a)
                _(E.encodeKey(u)), _(S);
            else
              for (let [u, S] of a)
                _(u), _(S);
          else {
            for (let u = 0, S = ve.length; u < S; u++) {
              let R = Ot[u];
              if (a instanceof R) {
                let O = ve[u], T = O.tag;
                T == null && (T = O.getTag && O.getTag.call(this, a)), T < 24 ? f[o++] = 192 | T : T < 256 ? (f[o++] = 216, f[o++] = T) : T < 65536 ? (f[o++] = 217, f[o++] = T >> 8, f[o++] = T & 255) : T > -1 && (f[o++] = 218, I.setUint32(o, T), o += 4), O.encode.call(this, a, _, V);
                return;
              }
            }
            if (a[Symbol.iterator]) {
              if (Ce) {
                let u = new Error("Iterable should be serialized as iterator");
                throw u.iteratorNotHandled = !0, u;
              }
              f[o++] = 159;
              for (let u of a)
                _(u);
              f[o++] = 255;
              return;
            }
            if (a[Symbol.asyncIterator] || De(a)) {
              let u = new Error("Iterable/blob should be serialized as iterator");
              throw u.iteratorNotHandled = !0, u;
            }
            if (this.useToJSON && a.toJSON) {
              const u = a.toJSON();
              if (u !== a)
                return _(u);
            }
            X(a);
          }
        }
      else if (g === "boolean")
        f[o++] = a ? 245 : 244;
      else if (g === "bigint") {
        if (a < BigInt(1) << BigInt(64) && a >= 0)
          f[o++] = 27, I.setBigUint64(o, a);
        else if (a > -(BigInt(1) << BigInt(64)) && a < 0)
          f[o++] = 59, I.setBigUint64(o, -a - BigInt(1));
        else if (this.largeBigIntToFloat)
          f[o++] = 251, I.setFloat64(o, Number(a));
        else {
          a >= BigInt(0) ? f[o++] = 194 : (f[o++] = 195, a = BigInt(-1) - a);
          let p = [];
          for (; a; )
            p.push(Number(a & BigInt(255))), a >>= BigInt(8);
          Qe(new Uint8Array(p.reverse()), V);
          return;
        }
        o += 8;
      } else if (g === "undefined")
        f[o++] = 247;
      else
        throw new Error("Unknown type: " + g);
    }, X = this.useRecords === !1 ? this.variableMapSize ? (a) => {
      let g = Object.keys(a), d = Object.values(a), p = g.length;
      if (p < 24 ? f[o++] = 160 | p : p < 256 ? (f[o++] = 184, f[o++] = p) : p < 65536 ? (f[o++] = 185, f[o++] = p >> 8, f[o++] = p & 255) : (f[o++] = 186, I.setUint32(o, p), o += 4), E.keyMap)
        for (let u = 0; u < p; u++)
          _(E.encodeKey(g[u])), _(d[u]);
      else
        for (let u = 0; u < p; u++)
          _(g[u]), _(d[u]);
    } : (a) => {
      f[o++] = 185;
      let g = o - r;
      o += 2;
      let d = 0;
      if (E.keyMap)
        for (let p in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(p)) && (_(E.encodeKey(p)), _(a[p]), d++);
      else
        for (let p in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(p)) && (_(p), _(a[p]), d++);
      f[g++ + r] = d >> 8, f[g + r] = d & 255;
    } : (a, g) => {
      let d, p = i.transitions || (i.transitions = /* @__PURE__ */ Object.create(null)), u = 0, S = 0, R, O;
      if (this.keyMap) {
        O = Object.keys(a).map((P) => this.encodeKey(P)), S = O.length;
        for (let P = 0; P < S; P++) {
          let it = O[P];
          d = p[it], d || (d = p[it] = /* @__PURE__ */ Object.create(null), u++), p = d;
        }
      } else
        for (let P in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(P)) && (d = p[P], d || (p[J] & 1048576 && (R = p[J] & 65535), d = p[P] = /* @__PURE__ */ Object.create(null), u++), p = d, S++);
      let T = p[J];
      if (T !== void 0)
        T &= 65535, f[o++] = 217, f[o++] = T >> 8 | 224, f[o++] = T & 255;
      else if (O || (O = p.__keys__ || (p.__keys__ = Object.keys(a))), R === void 0 ? (T = i.nextId++, T || (T = 0, i.nextId = 1), T >= dt && (i.nextId = (T = x) + 1)) : T = R, i[T] = O, T < x) {
        f[o++] = 217, f[o++] = T >> 8 | 224, f[o++] = T & 255, p = i.transitions;
        for (let P = 0; P < S; P++)
          (p[J] === void 0 || p[J] & 1048576) && (p[J] = T), p = p[O[P]];
        p[J] = T | 1048576, s = !0;
      } else {
        if (p[J] = T, I.setUint32(o, 3655335680), o += 3, u && (se += F * u), H.length >= dt - x && (H.shift()[J] = void 0), H.push(p), re(S + 2), _(57344 + T), _(O), g) return;
        for (let P in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(P)) && _(a[P]);
        return;
      }
      if (S < 24 ? f[o++] = 128 | S : re(S), !g)
        for (let P in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(P)) && _(a[P]);
    }, V = (a) => {
      let g;
      if (a > 16777216) {
        if (a - r > ht)
          throw new Error("Encoded buffer would be larger than maximum buffer size");
        g = Math.min(
          ht,
          Math.round(Math.max((a - r) * (a > 67108864 ? 1.25 : 2), 4194304) / 4096) * 4096
        );
      } else
        g = (Math.max(a - r << 2, f.length - 1) >> 12) + 1 << 12;
      let d = new Ie(g);
      return I = new DataView(d.buffer, 0, g), f.copy ? f.copy(d, 0, r, a) : d.set(f.slice(r, a)), o -= r, r = 0, fe = d.length - 10, f = d;
    };
    let Q = 100, ee = 1e3;
    this.encodeAsIterable = function(a, g) {
      return Re(a, g, ie);
    }, this.encodeAsAsyncIterable = function(a, g) {
      return Re(a, g, Oe);
    };
    function* ie(a, g, d) {
      let p = a.constructor;
      if (p === Object) {
        let u = E.useRecords !== !1;
        u ? X(a, !0) : pt(Object.keys(a).length, 160);
        for (let S in a) {
          let R = a[S];
          u || _(S), R && typeof R == "object" ? g[S] ? yield* ie(R, g[S]) : yield* de(R, g, S) : _(R);
        }
      } else if (p === Array) {
        let u = a.length;
        re(u);
        for (let S = 0; S < u; S++) {
          let R = a[S];
          R && (typeof R == "object" || o - r > Q) ? g.element ? yield* ie(R, g.element) : yield* de(R, g, "element") : _(R);
        }
      } else if (a[Symbol.iterator] && !a.buffer) {
        f[o++] = 159;
        for (let u of a)
          u && (typeof u == "object" || o - r > Q) ? g.element ? yield* ie(u, g.element) : yield* de(u, g, "element") : _(u);
        f[o++] = 255;
      } else De(a) ? (pt(a.size, 64), yield f.subarray(r, o), yield a, oe()) : a[Symbol.asyncIterator] ? (f[o++] = 159, yield f.subarray(r, o), yield a, oe(), f[o++] = 255) : _(a);
      d && o > r ? yield f.subarray(r, o) : o - r > Q && (yield f.subarray(r, o), oe());
    }
    function* de(a, g, d) {
      let p = o - r;
      try {
        _(a), o - r > Q && (yield f.subarray(r, o), oe());
      } catch (u) {
        if (u.iteratorNotHandled)
          g[d] = {}, o = r + p, yield* ie.call(this, a, g[d]);
        else throw u;
      }
    }
    function oe() {
      Q = ee, E.encode(null, Fe);
    }
    function Re(a, g, d) {
      return g && g.chunkThreshold ? Q = ee = g.chunkThreshold : Q = 100, a && typeof a == "object" ? (E.encode(null, Fe), d(a, E.iterateProperties || (E.iterateProperties = {}), !0)) : [E.encode(a)];
    }
    async function* Oe(a, g) {
      for (let d of ie(a, g, !0)) {
        let p = d.constructor;
        if (p === ut || p === Uint8Array)
          yield d;
        else if (De(d)) {
          let u = d.stream().getReader(), S;
          for (; !(S = await u.read()).done; )
            yield S.value;
        } else if (d[Symbol.asyncIterator])
          for await (let u of d)
            oe(), u ? yield* Oe(u, g.async || (g.async = {})) : yield E.encode(u);
        else
          yield d;
      }
    }
  }
  useBuffer(e) {
    f = e, I = new DataView(f.buffer, f.byteOffset, f.byteLength), o = 0;
  }
  clearSharedData() {
    this.structures && (this.structures = []), this.sharedValues && (this.sharedValues = void 0);
  }
  updateSharedData() {
    let e = this.sharedVersion || 0;
    this.sharedVersion = e + 1;
    let r = this.structures.slice(0), n = new Tt(r, this.sharedValues, this.sharedVersion), s = this.saveShared(
      n,
      (i) => (i && i.version || 0) == e
    );
    return s === !1 ? (n = this.getShared() || {}, this.structures = n.structures || [], this.sharedValues = n.packedValues, this.sharedVersion = n.version, this.structures.nextId = this.structures.length) : r.forEach((i, l) => this.structures[l] = i), s;
  }
}
function pt(t, e) {
  t < 24 ? f[o++] = e | t : t < 256 ? (f[o++] = e | 24, f[o++] = t) : t < 65536 ? (f[o++] = e | 25, f[o++] = t >> 8, f[o++] = t & 255) : (f[o++] = e | 26, I.setUint32(o, t), o += 4);
}
class Tt {
  constructor(e, r, n) {
    this.structures = e, this.packedValues = r, this.version = n;
  }
}
function re(t) {
  t < 24 ? f[o++] = 128 | t : t < 256 ? (f[o++] = 152, f[o++] = t) : t < 65536 ? (f[o++] = 153, f[o++] = t >> 8, f[o++] = t & 255) : (f[o++] = 154, I.setUint32(o, t), o += 4);
}
const Pr = typeof Blob > "u" ? function() {
} : Blob;
function De(t) {
  if (t instanceof Pr)
    return !0;
  let e = t[Symbol.toStringTag];
  return e === "Blob" || e === "File";
}
function Ue(t, e) {
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
            Ue(t[n], e);
        else {
          let n = !e.encoder.useRecords;
          for (var r in t)
            t.hasOwnProperty(r) && (n && Ue(r, e), Ue(t[r], e));
        }
      break;
    case "function":
      console.log(t);
  }
}
const Ur = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1;
Ot = [
  Date,
  Set,
  Error,
  RegExp,
  ue,
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
  Tt
];
ve = [
  {
    // Date
    tag: 1,
    encode(t, e) {
      let r = t.getTime() / 1e3;
      (this.useTimestamp32 || t.getMilliseconds() === 0) && r >= 0 && r < 4294967296 ? (f[o++] = 26, I.setUint32(o, r), o += 4) : (f[o++] = 251, I.setFloat64(o, r), o += 8);
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
      Qe(t, r);
    }
  },
  {
    // Uint8Array
    getTag(t) {
      if (t.constructor === Uint8Array && (this.tagUint8Array || _e && this.tagUint8Array !== !1))
        return 64;
    },
    encode(t, e, r) {
      Qe(t, r);
    }
  },
  te(68, 1),
  te(69, 2),
  te(70, 4),
  te(71, 8),
  te(72, 1),
  te(77, 2),
  te(78, 4),
  te(79, 8),
  te(85, 4),
  te(86, 8),
  {
    encode(t, e) {
      let r = t.packedValues || [], n = t.structures || [];
      if (r.values.length > 0) {
        f[o++] = 216, f[o++] = 51, re(4);
        let s = r.values;
        e(s), re(0), re(0), packedObjectMap = Object.create(sharedPackedObjectMap || null);
        for (let i = 0, l = s.length; i < l; i++)
          packedObjectMap[s[i]] = i;
      }
      if (n) {
        I.setUint32(o, 3655335424), o += 3;
        let s = n.slice(0);
        s.unshift(57344), s.push(new ue(t.version, 1399353956)), e(s);
      } else
        e(new ue(t.version, 1399353956));
    }
  }
];
function te(t, e) {
  return !Ur && e > 1 && (t -= 4), {
    tag: t,
    encode: function(n, s) {
      let i = n.byteLength, l = n.byteOffset || 0, h = n.buffer || n;
      s(_e ? Be.from(h, l, i) : new Uint8Array(h, l, i));
    }
  };
}
function Qe(t, e) {
  let r = t.byteLength;
  r < 24 ? f[o++] = 64 + r : r < 256 ? (f[o++] = 88, f[o++] = r) : r < 65536 ? (f[o++] = 89, f[o++] = r >> 8, f[o++] = r & 255) : (f[o++] = 90, I.setUint32(o, r), o += 4), o + r >= f.length && e(o + r), f.set(t.buffer ? t : new Uint8Array(t), o), o += r;
}
function Nr(t, e) {
  let r, n = e.length * 2, s = t.length - n;
  e.sort((i, l) => i.offset > l.offset ? 1 : -1);
  for (let i = 0; i < e.length; i++) {
    let l = e[i];
    l.id = i;
    for (let h of l.references)
      t[h++] = i >> 8, t[h] = i & 255;
  }
  for (; r = e.pop(); ) {
    let i = r.offset;
    t.copyWithin(i + n, i, s), n -= 2;
    let l = i + n;
    t[l++] = 216, t[l++] = 28, s = i;
  }
  return t;
}
function Et(t, e) {
  I.setUint32($.position + t, o - $.position - t + 1);
  let r = $;
  $ = null, e(r[0]), e(r[1]);
}
let tt = new et({ useRecords: !1 });
tt.encode;
tt.encodeAsIterable;
tt.encodeAsAsyncIterable;
const yt = 512, Lr = 1024, Fe = 2048, D = new et({ tagUint8Array: !1 }), y = {
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
  CONFIG_RELOAD_RESPONSE: 38,
  LOAD_REQUEST: 39,
  LOAD_PROGRESS: 40,
  LOAD_END: 41,
  REP_MARK_PERMANENT_REQUEST: 42,
  REP_MARK_PERMANENT_RESPONSE: 43,
  REP_DELETE_EPHEMERAL_REQUEST: 44,
  REP_DELETE_EPHEMERAL_RESPONSE: 45,
  REP_PIN_REQUEST: 46,
  REP_PIN_RESPONSE: 47,
  REP_UNPIN_REQUEST: 48,
  REP_UNPIN_RESPONSE: 49,
  EPHEMERAL_LIST_REQUEST: 50,
  EPHEMERAL_LIST_RESPONSE: 51
}, Br = {
  loaded: 0,
  partial: 1,
  failed: 2
}, ne = { cbor: 0, base58: 1, qrcode: 2 }, We = {
  [ne.cbor]: "application/cbor",
  [ne.base58]: "text/plain",
  [ne.qrcode]: "image/x-portable-pixmap"
}, Ir = {
  OK: 0,
  BAD_REQUEST: 1,
  NOT_FOUND: 2,
  INTERNAL_ERROR: 3,
  RANGE_NOT_SATISFIABLE: 4,
  UNAUTHORIZED: 5,
  CONFLICT: 6
};
function rt(t) {
  const e = k(t);
  return Array.isArray(e) ? e[0] : null;
}
function nt(t) {
  const e = new TextEncoder().encode(t);
  return D.encode([y.AUTH_REQUEST, e]);
}
function Ve(t, e = null) {
  const r = t.recyclerUrls || [], n = [
    y.PUT_REQUEST,
    t.contentType,
    t.fileName,
    t.streamLength,
    t.serverAddress || null,
    e || new Uint8Array(0),
    r,
    t.temporary ? 1 : 0
  ];
  return t.tupleSize !== void 0 && n.push(t.tupleSize), t.recycleEphemeral !== void 0 && t.recycleEphemeral !== 0 && (t.tupleSize === void 0 && n.push(null), n.push(t.recycleEphemeral)), D.encode(n);
}
function At(t) {
  return D.encode([y.PUT_DATA, t]);
}
function bt() {
  return D.encode([y.PUT_END]);
}
function ze(t) {
  const e = k(t);
  if (e[0] !== y.PUT_RESPONSE) throw new Error("Not a put response");
  return { oriString: e[1] };
}
function Pt(t, e) {
  const r = e && (e.start !== void 0 || e.end !== void 0), n = [y.GET_REQUEST, t, r ? 1 : 0];
  return r && (n.push(e.start || 0), n.push(e.end || 0)), D.encode(n);
}
function Ut(t) {
  const e = k(t);
  if (e[0] !== y.GET_RESPONSE_START) throw new Error("Not a get response start");
  return {
    contentType: e[1],
    contentLength: e[2],
    hasRange: e[3] === 1,
    rangeStart: e[3] ? e[4] : void 0,
    rangeEnd: e[3] ? e[5] : void 0
  };
}
function Nt(t) {
  const e = k(t);
  if (e[0] !== y.GET_DATA) throw new Error("Not a get data");
  return e[1];
}
function Lt(t) {
  const e = k(t);
  return Array.isArray(e) && e[0] === y.GET_END;
}
function Bt(t, e) {
  return e && (e.start !== void 0 || e.end !== void 0) ? D.encode([
    y.LOAD_REQUEST,
    t,
    1,
    e.start || 0,
    e.end || 0
  ]) : D.encode([y.LOAD_REQUEST, t]);
}
function It(t) {
  const e = k(t);
  if (!(Array.isArray(e) && e[0] === y.LOAD_PROGRESS))
    throw new Error("Not a load progress");
  return { tuplesLoaded: e[1], tuplesTotal: e[2] };
}
function Ct(t) {
  const e = k(t);
  return Array.isArray(e) && e[0] === y.LOAD_END;
}
function Dt(t) {
  const e = k(t);
  if (!(Array.isArray(e) && e[0] === y.LOAD_END))
    throw new Error("Not a load end");
  return { status: e[1], tuplesLoaded: e[2], tuplesTotal: e[3] };
}
function Ft(t) {
  const e = k(t);
  return !Array.isArray(e) || e[0] !== y.ERROR ? null : { statusCode: e[1], message: e[2] };
}
function Mt(t, e = 0) {
  return D.encode([y.BLOCK_PUT_REQUEST, t, e]);
}
function kt(t) {
  const e = k(t);
  if (e[0] !== y.BLOCK_PUT_RESPONSE) throw new Error("Not a block put response");
  return { status: e[1], hash: e[2] };
}
function Ht(t) {
  return D.encode([y.BLOCK_GET_REQUEST, t]);
}
function jt(t) {
  const e = k(t);
  if (e[0] !== y.BLOCK_GET_RESPONSE) throw new Error("Not a block get response");
  return { status: e[1], data: e[2] };
}
function qt(t, e = 0) {
  return e ? D.encode([y.BLOCK_DELETE_REQUEST, t, 1]) : D.encode([y.BLOCK_DELETE_REQUEST, t]);
}
function $t(t) {
  const e = k(t);
  if (e[0] !== y.BLOCK_DELETE_RESPONSE) throw new Error("Not a block delete response");
  return { status: e[1] };
}
function Kt() {
  return D.encode([y.HEALTH_REQUEST]);
}
function Gt(t) {
  const e = k(t);
  if (e[0] !== y.HEALTH_RESPONSE) throw new Error("Not a health response");
  return { json: e[1] };
}
function vt(t = 0) {
  return t === 0 ? D.encode([y.PEER_INFO_REQUEST]) : D.encode([y.PEER_INFO_REQUEST, t]);
}
function Qt(t) {
  const e = k(t);
  if (e[0] !== y.PEER_INFO_RESPONSE) throw new Error("Not a peer info response");
  return { format: e[1], data: e[2] };
}
function Wt(t, e) {
  return D.encode([y.PEER_CONNECT, t, e]);
}
function Vt(t) {
  const e = k(t);
  if (e[0] !== y.PEER_CONNECT_RESULT) throw new Error("Not a peer connect result");
  return { status: e[1] };
}
function zt() {
  return D.encode([y.PEER_LIST_REQUEST]);
}
function Jt(t) {
  const e = k(t);
  if (e[0] !== y.PEER_LIST_RESPONSE) throw new Error("Not a peer list response");
  return e[1];
}
function Zt(t, e) {
  return D.encode([y.FRIEND_ADD, t, e]);
}
function Yt(t) {
  return D.encode([y.FRIEND_REMOVE, t]);
}
function Xt() {
  return D.encode([y.FRIEND_LIST]);
}
function er(t) {
  const e = k(t);
  if (e[0] !== y.FRIEND_LIST_RESPONSE) throw new Error("Not a friend list response");
  return e[1];
}
function tr() {
  return D.encode([y.CONFIG_SHOW_REQUEST]);
}
function rr(t) {
  const e = k(t);
  if (e[0] !== y.CONFIG_SHOW_RESPONSE) throw new Error("Not a config show response");
  return { json: e[1] };
}
function nr(t, e) {
  return D.encode([y.CONFIG_SET_REQUEST, t, e]);
}
function sr(t) {
  const e = k(t);
  if (e[0] !== y.CONFIG_SET_RESPONSE) throw new Error("Not a config set response");
  return { status: e[1], restartRequired: e[2] === 1, message: e[3] };
}
function ir() {
  return D.encode([y.CONFIG_RELOAD_REQUEST]);
}
function or(t) {
  const e = k(t);
  if (e[0] !== y.CONFIG_RELOAD_RESPONSE) throw new Error("Not a config reload response");
  return { status: e[1], message: e[2] };
}
function ar(t, e) {
  return D.encode([t, e]);
}
function fr(t) {
  const e = k(t);
  return { status: e[1], blocks: e[2] };
}
function lr() {
  return D.encode([y.EPHEMERAL_LIST_REQUEST]);
}
function cr(t) {
  const e = k(t), r = (e[2] || []).map((n) => ({
    hash: Array.from(n[0], (s) => s.toString(16).padStart(2, "0")).join(""),
    claims: n[1],
    pins: n[2]
  }));
  return { status: e[1], entries: r };
}
const Xr = /* @__PURE__ */ Object.freeze(/* @__PURE__ */ Object.defineProperty({
  __proto__: null,
  LOAD_STATUS: Br,
  MSG: y,
  PEER_CONTENT_TYPES: We,
  PEER_FORMATS: ne,
  STATUS: Ir,
  decodeBlockDeleteResponse: $t,
  decodeBlockGetResponse: jt,
  decodeBlockPutResponse: kt,
  decodeConfigReloadResponse: or,
  decodeConfigSetResponse: sr,
  decodeConfigShowResponse: rr,
  decodeEphemeralListResponse: cr,
  decodeError: Ft,
  decodeFriendListResponse: er,
  decodeGetData: Nt,
  decodeGetResponseStart: Ut,
  decodeHealthResponse: Gt,
  decodeLoadEnd: Dt,
  decodeLoadProgress: It,
  decodePeerConnectResult: Vt,
  decodePeerInfoResponse: Qt,
  decodePeerListResponse: Jt,
  decodePutResponse: ze,
  decodeRepResponse: fr,
  encodeAuthRequest: nt,
  encodeBlockDeleteRequest: qt,
  encodeBlockGetRequest: Ht,
  encodeBlockPutRequest: Mt,
  encodeConfigReloadRequest: ir,
  encodeConfigSetRequest: nr,
  encodeConfigShowRequest: tr,
  encodeEphemeralListRequest: lr,
  encodeFriendAdd: Zt,
  encodeFriendListRequest: Xt,
  encodeFriendRemove: Yt,
  encodeGetRequest: Pt,
  encodeHealthRequest: Kt,
  encodeLoadRequest: Bt,
  encodePeerConnect: Wt,
  encodePeerInfoRequest: vt,
  encodePeerListRequest: zt,
  encodePutData: At,
  encodePutEnd: bt,
  encodePutRequest: Ve,
  encodeRepRequest: ar,
  getMessageType: rt,
  isGetEnd: Lt,
  isLoadEnd: Ct
}, Symbol.toStringTag, { value: "Module" }));
class N {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {string} */
    C(this, "baseUrl");
    /** @type {string|undefined} */
    C(this, "apiKey");
    /** @type {AbortController|null} */
    C(this, "abortController", null);
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
    var h, E;
    const n = {
      ...this.authHeaders(),
      type: e.contentType,
      "file-name": e.fileName,
      "stream-length": String(e.streamLength)
    };
    e.serverAddress && (n["server-address"] = e.serverAddress), (h = e.recyclerUrls) != null && h.length && (n.recycler = JSON.stringify(e.recyclerUrls)), e.temporary && (n.temporary = "true"), e.tupleSize !== void 0 && (n["tuple-size"] = String(e.tupleSize)), e.recycleEphemeral === 1 && (n["recycle-ephemeral"] = "commit"), e.recycleEphemeral === 2 && (n["recycle-ephemeral"] = "propagate");
    let s = r;
    r && typeof r.getReader == "function" && (s = await this._readStream(r));
    const i = await fetch(this.url("/offsystem"), {
      method: "PUT",
      headers: n,
      body: s,
      signal: (E = this.abortController) == null ? void 0 : E.signal
    });
    if (!i.ok) {
      const m = await i.text();
      throw new Error(`Upload failed: ${i.status} ${m}`);
    }
    return { oriString: await i.text() };
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
      const { done: h, value: E } = await r.read();
      if (h) break;
      n.push(E), s += E.length;
    }
    const i = new Uint8Array(s);
    let l = 0;
    for (const h of n)
      i.set(h, l), l += h.length;
    return i;
  }
  /**
   * Download from GET /offsystem/v3/...
   * @param {string} offUrl
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   */
  async get(e, r) {
    var U, L, q, v, B, H, se;
    const n = await fetch(e, {
      method: "GET",
      headers: this.authHeaders(),
      signal: (U = this.abortController) == null ? void 0 : U.signal
    });
    if (!n.ok) {
      const F = await n.text();
      (L = r.onError) == null || L.call(r, n.status, F);
      return;
    }
    const s = n.headers.get("content-type") || "application/octet-stream", i = parseInt(n.headers.get("content-length") || "0", 10), l = n.status === 206, h = n.headers.get("content-range");
    let E, m;
    if (h) {
      const F = h.match(/bytes (\d+)-(\d+)\//);
      F && (E = parseInt(F[1], 10), m = parseInt(F[2], 10));
    }
    (q = r.onStart) == null || q.call(r, s, i, l, E, m);
    const x = (v = n.body) == null ? void 0 : v.getReader();
    if (!x) {
      (B = r.onEnd) == null || B.call(r);
      return;
    }
    try {
      for (; ; ) {
        const { done: F, value: _ } = await x.read();
        if (F) break;
        _ && r.onData(_);
      }
      (H = r.onEnd) == null || H.call(r);
    } catch (F) {
      (se = r.onError) == null || se.call(r, 0, String(F));
    }
  }
  /**
   * Cache-only load: GET offUrl + '?load=1'. The daemon pulls the file's
   * blocks into its block cache without serving file data and streams
   * application/x-ndjson progress, one JSON object per line:
   *   {"tuples_loaded":n,"tuples_total":m}          — per resolved tuple
   *   {"status":"loaded|partial|failed",...}         — terminal line
   * The terminal line is also reported through onEnd.
   * @param {string} offUrl
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   * @param {{start?: number, end?: number}} [range]
   * @returns {Promise<void>}
   */
  async load(e, r = {}, n) {
    var m, x, U;
    const s = e.includes("?") ? "&" : "?", i = await fetch(`${e}${s}load=1`, {
      method: "GET",
      headers: n ? { ...this.authHeaders(), Range: `bytes=${n.start || 0}-${n.end || 0}` } : this.authHeaders(),
      signal: (m = this.abortController) == null ? void 0 : m.signal
    });
    if (!i.ok) {
      const L = await i.text();
      throw new Error(`Load failed: ${i.status} ${L}`);
    }
    const l = (x = i.body) == null ? void 0 : x.getReader();
    if (!l) return;
    const h = new TextDecoder();
    let E = "";
    try {
      for (; ; ) {
        const { done: q, value: v } = await l.read();
        if (q) break;
        E += h.decode(v, { stream: !0 });
        let B;
        for (; (B = E.indexOf(`
`)) !== -1; ) {
          const H = E.slice(0, B).trim();
          E = E.slice(B + 1), H && this._handleLoadLine(H, r);
        }
      }
      E += h.decode();
      const L = E.trim();
      L && this._handleLoadLine(L, r);
    } catch (L) {
      (U = r.onError) == null || U.call(r, 0, String(L));
    }
  }
  /**
   * Parse one ndjson progress/status line and dispatch to callbacks.
   * @param {string} line
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   */
  _handleLoadLine(e, r) {
    var s, i;
    let n;
    try {
      n = JSON.parse(e);
    } catch {
      throw new Error(`Bad ndjson line: ${e}`);
    }
    n.status !== void 0 ? (s = r.onEnd) == null || s.call(r, n.status, n.tuples_loaded || 0, n.tuples_total || 0) : (i = r.onProgress) == null || i.call(r, n.tuples_loaded || 0, n.tuples_total || 0);
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
      const h = await s.text();
      throw new Error(`Block put failed: ${s.status} ${h}`);
    }
    const i = await s.arrayBuffer();
    return { status: 0, hash: new Uint8Array(i) };
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
   * @param {number} [force] 1 removes pinned / ephemeral-claimed blocks too
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(e, r = 0) {
    var i;
    const n = r ? "?force=1" : "", s = await fetch(this.url(`/blocks/${e}${n}`), {
      method: "DELETE",
      headers: this.authHeaders(),
      signal: (i = this.abortController) == null ? void 0 : i.signal
    });
    return s.status === 409 ? { status: 6 } : { status: s.ok ? 0 : 2 };
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
    var i;
    const r = ne[e] ?? 0, n = await fetch(this.url(`/peer/info?format=${e}`), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (i = this.abortController) == null ? void 0 : i.signal
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
      headers: { ...this.authHeaders(), "Content-Type": We[r] ?? "application/cbor" },
      body: r === ne.base58 ? new TextDecoder().decode(e) : e,
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
      headers: { ...this.authHeaders(), "Content-Type": We[r] ?? "application/cbor" },
      body: r === ne.base58 ? new TextDecoder().decode(e) : e,
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
  /**
   * Shared plumbing for the representation ephemeral/pin HTTP routes.
   * @private
   * @param {string} path
   * @param {string} url - full OFF URL (request body)
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async _repOp(e, r) {
    var i;
    const n = await fetch(this.url(e), {
      method: "POST",
      headers: { ...this.authHeaders(), "Content-Type": "text/plain" },
      body: r,
      signal: (i = this.abortController) == null ? void 0 : i.signal
    });
    if (!n.ok) throw new Error(`Representation op failed: ${n.status}`);
    const s = await n.json();
    return { status: s.result === "ok" ? 0 : 1, blocks: s.blocks };
  }
  /**
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async markPermanent(e) {
    return this._repOp("/offsystem/ephemeral/commit", e);
  }
  /**
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async deleteEphemeral(e) {
    return this._repOp("/offsystem/ephemeral/delete", e);
  }
  /**
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async pinRepresentation(e) {
    return this._repOp("/offsystem/pin", e);
  }
  /**
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async unpinRepresentation(e) {
    return this._repOp("/offsystem/unpin", e);
  }
  /**
   * @returns {Promise<{status: number, entries: Array<{hash: string, claims: number, pins: number}>}>}
   */
  async listEphemerals() {
    var n;
    const e = await fetch(this.url("/offsystem/ephemeral/list"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    });
    if (!e.ok) throw new Error(`Ephemeral list failed: ${e.status}`);
    return { status: 0, entries: await e.json() };
  }
}
class Cr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {WebSocket|null} */
    C(this, "socket", null);
    /** @type {string|undefined} */
    C(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    C(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    C(this, "openPromise", null);
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
        this.apiKey && this.send(nt(this.apiKey)), e();
      }, n.onerror = (s) => {
        var l;
        const i = s.message || ((l = s.error) == null ? void 0 : l.message) || "unknown";
        r(new Error(`WebSocket error: ${i}`));
      }, n.onclose = () => {
        this.socket = null, this.openPromise = null;
      }, n.onmessage = (s) => {
        var h;
        const i = new Uint8Array(s.data), l = rt(i);
        l !== null && ((h = this.messageHandler) == null || h.call(this, l, i));
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
class Dr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {WebTransport|null} */
    C(this, "transport", null);
    /** @type {WritableStreamWriter|null} */
    C(this, "writer", null);
    /** @type {ReadableStreamReader|null} */
    C(this, "reader", null);
    /** @type {string|undefined} */
    C(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    C(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    C(this, "openPromise", null);
    /** @type {boolean} */
    C(this, "running", !1);
    this.url = e, this.apiKey = r;
  }
  /**
   * @returns {Promise<void>}
   */
  async connect() {
    return this.transport ? this.openPromise || Promise.resolve() : (this.transport = new WebTransport(this.url), this.openPromise = this.transport.ready.then(async () => {
      const e = await this.transport.createBidirectionalStream();
      this.writer = e.writable.getWriter(), this.reader = e.readable.getReader(), this.running = !0, this._readLoop(), this.apiKey && await this.send(nt(this.apiKey));
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
        const i = s instanceof Uint8Array ? s : new Uint8Array(s.buffer, s.byteOffset, s.byteLength);
        for (e = e ? Fr(e, i) : i; e.length >= 4; ) {
          const h = new DataView(e.buffer, e.byteOffset, e.length).getUint32(0, !1);
          if (e.length < 4 + h) break;
          const E = e.subarray(4, 4 + h), m = rt(E);
          m !== null && ((r = this.messageHandler) == null || r.call(this, m, E)), e = e.subarray(4 + h);
        }
      }
    } catch {
    }
  }
}
function Fr(t, e) {
  const r = new Uint8Array(t.length + e.length);
  return r.set(t, 0), r.set(e, t.length), r;
}
const Je = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz", st = new Int8Array(128);
st.fill(-1);
for (let t = 0; t < Je.length; t++)
  st[Je.charCodeAt(t)] = t;
function ye(t) {
  if (t.length === 0) return null;
  let e = 0;
  for (; e < t.length && t[e] === "1"; )
    e++;
  const r = [];
  for (let n = e; n < t.length; n++) {
    const s = t.charCodeAt(n);
    if (s >= 128) return null;
    const i = st[s];
    if (i < 0) return null;
    let l = i;
    for (let h = 0; h < r.length; h++)
      l += r[h] * 58, r[h] = l & 255, l >>= 8;
    for (; l > 0; )
      r.push(l & 255), l >>= 8;
  }
  for (let n = 0; n < e; n++)
    r.push(0);
  return r.reverse(), new Uint8Array(r);
}
function Me(t) {
  if (t.length === 0) return "";
  const e = Array.from(t);
  let r = 0;
  for (; r < e.length && e[r] === 0; )
    r++;
  const n = [];
  for (let i = r; i < e.length; i++) {
    let l = e[i];
    for (let h = 0; h < n.length; h++)
      l += n[h] * 256, n[h] = l % 58, l = Math.floor(l / 58);
    for (; l > 0; )
      n.push(l % 58), l = Math.floor(l / 58);
  }
  return "1".repeat(r) + n.reverse().map((i) => Je[i]).join("");
}
function wt(t) {
  const e = t.indexOf("/offsystem/v3/");
  if (e < 0) return null;
  const n = t.slice(e + 14).split("/");
  if (n.length < 4) return null;
  const s = n[n.length - 4], i = n[n.length - 3], l = n[n.length - 2], h = n.slice(n.length - 1).join("/"), E = parseInt(s, 10);
  return !Number.isFinite(E) || ye(i) === null || ye(l) === null ? null : {
    fileHashB58: i,
    descriptorHashB58: l,
    streamLength: E,
    fileName: decodeURIComponent(h)
  };
}
function Mr(t) {
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
function kr(t) {
  return typeof t.arrayBuffer == "function" ? t.arrayBuffer().then((e) => new Uint8Array(e)) : new Promise((e, r) => {
    const n = new FileReader();
    n.onload = () => e(new Uint8Array(n.result)), n.onerror = () => r(n.error), n.readAsArrayBuffer(t);
  });
}
function be(t) {
  const r = t.replace(/\\\\/g, "/").split("/").filter(Boolean);
  return r.length > 0 ? r[r.length - 1] : "file";
}
function gt(t, e = 65536) {
  let r = 0;
  return new ReadableStream({
    pull(n) {
      if (r >= t.size) {
        n.close();
        return;
      }
      const s = Math.min(r + e, t.size), i = t.slice(r, s);
      return kr(i).then((l) => {
        n.enqueue(l), r = s;
      });
    }
  });
}
function Hr(t) {
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
function jr(t, e = "http://localhost:23402") {
  if (!t || /^https?:\/\//i.test(t)) return t;
  let r = t;
  r.startsWith("offs://") && (r = r.slice(7));
  const n = "/offsystem/v3/", s = r.indexOf(n);
  return s >= 0 && (r = r.slice(s)), r.startsWith(n) ? `${e.replace(/\/$/, "")}${r}` : t;
}
const qr = new et({ tagUint8Array: !1, useRecords: !1 });
new we({ useRecords: !1 });
const $r = 128e3, Kr = 3;
function Gr({
  name: t,
  fileHash: e,
  descriptorHash: r,
  finalByte: n,
  blockType: s = $r,
  tupleSize: i = Kr,
  fileOffset: l = 0
}) {
  return {
    name: t,
    isDirectory: !1,
    fileHash: e,
    descriptorHash: r,
    finalByte: n,
    blockType: s,
    tupleSize: i,
    fileOffset: l
  };
}
function vr({ name: t, dirHash: e, descriptorHash: r, dirSize: n }) {
  return { name: t, isDirectory: !0, dirHash: e, descriptorHash: r, dirSize: n };
}
function Qr(t) {
  const e = t.map((r) => {
    const n = {
      n: r.name,
      t: r.isDirectory ? 1 : 0
    };
    return r.isDirectory ? (n.d = r.dirHash, r.descriptorHash && (n.D = r.descriptorHash), r.dirSize !== void 0 && (n.s = r.dirSize)) : (n.f = r.fileHash, n.D = r.descriptorHash, n.s = r.finalByte, n.B = r.blockType, n.T = r.tupleSize, n.o = r.fileOffset), n;
  });
  return qr.encode({ v: 1, entries: e });
}
function Wr() {
  return {
    connectTimeoutMs: 5e3,
    requestTimeoutMs: 3e4
  };
}
function Vr(t, e, r) {
  return t.startsWith("ws://") || t.startsWith("wss://") ? new Cr(t, e, r) : t.startsWith("wt://") || t.startsWith("wts://") ? new Dr(t, e, r) : new N(t, e, r);
}
class en {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {OffsClientConfig & {transport?: any}} [config]
   */
  constructor(e, r, n) {
    /** @type {string} */
    C(this, "url");
    /** @type {string|undefined} */
    C(this, "apiKey");
    /** @type {OffsClientConfig} */
    C(this, "config");
    /** @type {HttpTransport|WsTransport|WtTransport} */
    C(this, "transport");
    /** @type {Map<number, PendingRequest>} */
    C(this, "pending", /* @__PURE__ */ new Map());
    /** @type {{type: number, bytes: Uint8Array}[]} */
    C(this, "inboundQueue", []);
    /** @type {number} */
    C(this, "nextRequestId", 1);
    /** @type {boolean} */
    C(this, "streamingPut", !1);
    /** @type {OffsPutOptions|null} */
    C(this, "streamOptions", null);
    /** @type {boolean} */
    C(this, "connected", !1);
    this.url = e, this.apiKey = r, this.config = { ...Wr(), ...n }, this.transport = (n == null ? void 0 : n.transport) || Vr(e, r, n), this.transport.setMessageHandler(this._onMessage.bind(this));
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
    return new Promise((s, i) => {
      const l = {
        id: e,
        type: r,
        resolve: s,
        reject: i,
        timer: setTimeout(() => {
          this.pending.delete(e), i(new Error("Request timeout"));
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
    const n = this.nextRequestId++, s = this._request(n, e, r), i = this._dequeueMatching(e);
    return i !== null && this._resolve(n, i), s;
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
    if (e === y.ERROR) {
      const n = Ft(r);
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
    const r = Array.isArray(e) ? e : [e], n = this.inboundQueue.findIndex((i) => r.includes(i.type));
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
    const s = this.nextRequestId++, i = this._request(s, r, n);
    return await this.transport.send(e), i;
  }
  /**
   * @param {string|OffsPutOptions} options
   * @param {Uint8Array|undefined} data
   * @returns {Promise<{oriString: string}>}
   */
  async put(e, r) {
    if (typeof e == "string")
      throw new Error("Use object options (contentType, fileName, streamLength)");
    const n = {
      ...e,
      fileName: be(e.fileName)
    };
    if (this.transport instanceof N) {
      const l = r || new Uint8Array(0);
      return this.transport.put(n, l);
    }
    const s = Ve(n, r), i = await this._sendAndWait(s, y.PUT_RESPONSE);
    return ze(i);
  }
  /**
   * @param {OffsPutOptions} options
   * @returns {Promise<void>}
   */
  async putStreamStart(e) {
    if (this.streamingPut = !0, this.streamOptions = e, this.transport instanceof N)
      return;
    const r = Ve(e);
    await this.transport.send(r);
  }
  /**
   * @param {Uint8Array} chunk
   * @returns {Promise<void>}
   */
  async putStreamData(e) {
    if (this.transport instanceof N)
      throw new Error("HTTP transport does not support putStreamData; use put with ReadableStream");
    await this.transport.send(At(e));
  }
  /**
   * @returns {Promise<{oriString: string}>}
   */
  async putStreamEnd() {
    this.streamingPut = !1;
    const e = this.streamOptions;
    if (this.streamOptions = null, this.transport instanceof N) {
      if (!e) throw new Error("No stream in progress");
      return this.transport.put(e, new Uint8Array(0));
    }
    await this.transport.send(bt());
    const r = await this._request(this.nextRequestId - 1, y.PUT_RESPONSE);
    return ze(r);
  }
  /**
   * @param {string} oriString
   * @param {OffsGetCallbacks} callbacks
   * @param {{start?: number, end?: number}} [range]
   */
  async get(e, r, n) {
    var h, E;
    if (this.transport instanceof N)
      return this.transport.get(e, r);
    const s = Pt(e, n), i = await this._sendAndWait(s, y.GET_RESPONSE_START), l = Ut(i);
    for ((h = r.onStart) == null || h.call(r, l.contentType, l.contentLength, l.hasRange, l.rangeStart, l.rangeEnd); ; ) {
      const m = await this._waitForResponse([y.GET_DATA, y.GET_END]);
      if (Lt(m)) break;
      const x = Nt(m);
      r.onData(x);
    }
    (E = r.onEnd) == null || E.call(r);
  }
  /**
   * Load a file's blocks into the daemon's block cache without downloading
   * the file data. Progress is reported per resolved tuple; the operation
   * ends with a terminal status.
   *
   * HTTP transports stream an application/x-ndjson body whose progress lines
   * are {"tuples_loaded":n,"tuples_total":m} objects and whose terminal line
   * carries a status string ("loaded"|"partial"|"failed"). CBOR transports
   * use LOAD_PROGRESS/LOAD_END frames whose status is numeric
   * (0=loaded, 1=partial, 2=failed) — see wire.LOAD_STATUS.
   *
   * @param {string} oriString
   * @param {Object} [callbacks]
   * @param {(tuplesLoaded: number, tuplesTotal: number) => void} [callbacks.onProgress]
   * @param {(status: string|number, tuplesLoaded: number, tuplesTotal: number) => void} [callbacks.onEnd]
   * @param {(statusCode: number, message: string) => void} [callbacks.onError]
   * @param {{start?: number, end?: number}} [range]
   * @returns {Promise<void>}
   */
  async load(e, r = {}, n) {
    var h, E;
    if (this.transport instanceof N)
      return this.transport.load(e, r, n);
    const s = Bt(e, n);
    await this.transport.send(s);
    let i = null;
    for (; ; ) {
      const m = await this._waitForResponse([y.LOAD_PROGRESS, y.LOAD_END]);
      if (Ct(m)) {
        i = m;
        break;
      }
      const x = It(m);
      (h = r.onProgress) == null || h.call(r, x.tuplesLoaded, x.tuplesTotal);
    }
    const l = Dt(i);
    (E = r.onEnd) == null || E.call(r, l.status, l.tuplesLoaded, l.tuplesTotal);
  }
  /**
   * @param {Uint8Array} data
   * @param {number} [encoding=0]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(e, r = 0) {
    if (this.transport instanceof N)
      return this.transport.blockPut(e, r);
    const n = Mt(e, r), s = await this._sendAndWait(n, y.BLOCK_PUT_RESPONSE);
    return kt(s);
  }
  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number, data: Uint8Array}>}
   */
  async blockGet(e) {
    if (typeof e == "string") return this.transport.blockGet(e);
    if (this.transport instanceof N)
      return this.transport.blockGet(Me(e));
    const r = Ht(e), n = await this._sendAndWait(r, y.BLOCK_GET_RESPONSE);
    return jt(n);
  }
  /**
   * @param {string|Uint8Array} hash
   * @param {{force?: boolean}} [options] force=true removes pinned /
   *   ephemeral-claimed blocks too; otherwise those deletes resolve with
   *   status CONFLICT (wire.STATUS.CONFLICT).
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(e, r = {}) {
    const n = r.force ? 1 : 0;
    if (typeof e == "string") return this.transport.blockDelete(e, n);
    if (this.transport instanceof N)
      return this.transport.blockDelete(Me(e), n);
    const s = qt(e, n), i = await this._sendAndWait(s, y.BLOCK_DELETE_RESPONSE);
    return $t(i);
  }
  /**
   * @returns {Promise<any>}
   */
  async health() {
    if (this.transport instanceof N)
      return this.transport.health();
    const e = Kt(), r = await this._sendAndWait(e, y.HEALTH_RESPONSE), { json: n } = Gt(r);
    return JSON.parse(n);
  }
  /**
   * @param {string} [format='cbor']
   * @returns {Promise<{format: number, data: Uint8Array}>}
   */
  async peerInfo(e = "cbor") {
    if (this.transport instanceof N)
      return this.transport.peerInfo(e);
    const r = vt(ne[e] ?? 0), n = await this._sendAndWait(r, y.PEER_INFO_RESPONSE);
    return Qt(n);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(e, r = 0) {
    if (this.transport instanceof N)
      return this.transport.peerConnect(e, r);
    const n = Wt(r, e), s = await this._sendAndWait(n, y.PEER_CONNECT_RESULT);
    return Vt(s);
  }
  /**
   * Connect to a peer from a QR image (binary P6 PPM bytes).
   * @param {Uint8Array} ppmBytes
   * @returns {Promise<{status: number}>}
   */
  async peerConnectQr(e) {
    return this.peerConnect(e, ne.qrcode);
  }
  /**
   * Add a friend from a QR image (binary P6 PPM bytes).
   * @param {Uint8Array} ppmBytes
   * @returns {Promise<void>}
   */
  async friendAddQr(e) {
    return this.friendAdd(e, ne.qrcode);
  }
  /**
   * Convert an OFF URL/URI string into an HTTP URL usable by a browser.
   * @param {string} oriString
   * @param {string} [baseUrl]
   * @returns {string}
   */
  static offUrlToHttpUrl(e, r) {
    return jr(e, r);
  }
  /**
   * Shared plumbing for the representation ephemeral/pin operations.
   * @private
   * @param {number} requestCode
   * @param {number} responseCode
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async _repOp(e, r, n) {
    const s = ar(e, n), i = await this._sendAndWait(s, r);
    return fr(i);
  }
  /**
   * Commit an ephemeral representation: clear every ephemeral claim its
   * blocks hold, announce the newly-committed blocks to the network.
   * @param {string} url - full OFF URL of the representation
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async markPermanent(e) {
    return this.transport instanceof N ? this.transport.markPermanent(e) : this._repOp(y.REP_MARK_PERMANENT_REQUEST, y.REP_MARK_PERMANENT_RESPONSE, e);
  }
  /**
   * Abort an ephemeral representation: release its claims, deleting the
   * blocks that are exclusively owned by it.
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async deleteEphemeral(e) {
    return this.transport instanceof N ? this.transport.deleteEphemeral(e) : this._repOp(y.REP_DELETE_EPHEMERAL_REQUEST, y.REP_DELETE_EPHEMERAL_RESPONSE, e);
  }
  /**
   * Pin every block of a representation (+1 pin each; pinned permanent blocks
   * resist respiration and explicit deletion).
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async pinRepresentation(e) {
    return this.transport instanceof N ? this.transport.pinRepresentation(e) : this._repOp(y.REP_PIN_REQUEST, y.REP_PIN_RESPONSE, e);
  }
  /**
   * Unpin every block of a representation (clamped at 0).
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async unpinRepresentation(e) {
    return this.transport instanceof N ? this.transport.unpinRepresentation(e) : this._repOp(y.REP_UNPIN_REQUEST, y.REP_UNPIN_RESPONSE, e);
  }
  /**
   * Enumerate every ephemeral block in the cache (maintenance surface).
   * @returns {Promise<{status: number, entries: Array<{hash: string, claims: number, pins: number}>}>}
   */
  async listEphemerals() {
    if (this.transport instanceof N)
      return this.transport.listEphemerals();
    const e = lr(), r = await this._sendAndWait(e, y.EPHEMERAL_LIST_RESPONSE);
    return cr(r);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    if (this.transport instanceof N)
      return this.transport.peerList();
    const e = zt(), r = await this._sendAndWait(e, y.PEER_LIST_RESPONSE);
    return Jt(r);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(e, r = 0) {
    if (this.transport instanceof N)
      return this.transport.friendAdd(e, r);
    const n = Zt(r, e);
    await this.transport.send(n);
  }
  /**
   * @param {string|Uint8Array} nodeId
   * @returns {Promise<void>}
   */
  async friendRemove(e) {
    if (this.transport instanceof N)
      return this.transport.friendRemove(typeof e == "string" ? e : Me(e));
    const r = typeof e == "string" ? new TextEncoder().encode(e) : e, n = Yt(r);
    await this.transport.send(n);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    if (this.transport instanceof N)
      return this.transport.friendList();
    const e = Xt(), r = await this._sendAndWait(e, y.FRIEND_LIST_RESPONSE);
    return er(r);
  }
  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    if (this.transport instanceof N)
      return this.transport.configShow();
    const e = tr(), r = await this._sendAndWait(e, y.CONFIG_SHOW_RESPONSE), { json: n } = rr(r);
    return JSON.parse(n);
  }
  /**
   * @param {string} field
   * @param {string} value
   * @returns {Promise<{status: number, restartRequired: boolean, message: string}>}
   */
  async configSet(e, r) {
    if (this.transport instanceof N)
      return this.transport.configSet(e, r);
    const n = nr(e, r), s = await this._sendAndWait(n, y.CONFIG_SET_RESPONSE);
    return sr(s);
  }
  /**
   * @returns {Promise<{status: number, message: string}>}
   */
  async configReload() {
    if (this.transport instanceof N)
      return this.transport.configReload();
    const e = ir(), r = await this._sendAndWait(e, y.CONFIG_RELOAD_RESPONSE);
    return or(r);
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
    const n = Hr(e);
    if (n.length === 0)
      throw new Error("No files to upload");
    const s = r.recyclerUrls || [], i = n.length;
    let l = 0;
    const h = (x) => {
      var U;
      l++, (U = r.onProgress) == null || U.call(r, x, l, i);
    }, E = zr(n.map((x) => x.path)), m = async (x) => {
      const U = be(x || E || "root"), q = Jr(n, x), v = Zr(n, x), B = [];
      for (const F of v) {
        const X = (await m(F)).oriString, V = wt(X);
        if (!V)
          throw new Error(`Failed to parse subdirectory URL: ${X}`);
        const Q = ye(V.fileHashB58), ee = ye(V.descriptorHashB58);
        if (!Q || !ee)
          throw new Error(`Invalid directory hash in URL: ${X}`);
        B.push(vr({
          name: be(F),
          dirHash: Q,
          descriptorHash: ee,
          dirSize: V.streamLength
        }));
      }
      for (const F of q) {
        const _ = be(F.path), X = Mr(_), V = F.file.size;
        let Q;
        if (this.transport instanceof N) {
          const oe = gt(F.file);
          Q = (await this.put({
            contentType: X,
            fileName: _,
            streamLength: V,
            serverAddress: r.serverAddress,
            recyclerUrls: s,
            temporary: r.temporary
          }, oe)).oriString;
        } else {
          await this.putStreamStart({
            contentType: X,
            fileName: _,
            streamLength: V,
            serverAddress: r.serverAddress,
            recyclerUrls: s,
            temporary: r.temporary
          });
          const oe = gt(F.file).getReader();
          for (; ; ) {
            const { done: Oe, value: a } = await oe.read();
            if (Oe) break;
            await this.putStreamData(a);
          }
          Q = (await this.putStreamEnd()).oriString;
        }
        const ee = wt(Q);
        if (!ee)
          throw new Error(`Failed to parse file URL: ${Q}`);
        const ie = ye(ee.fileHashB58), de = ye(ee.descriptorHashB58);
        if (!ie || !de)
          throw new Error(`Invalid hash in file URL: ${Q}`);
        B.push(Gr({
          name: _,
          fileHash: ie,
          descriptorHash: de,
          finalByte: ee.streamLength
        })), h(_);
      }
      if (B.length === 0)
        throw new Error(`Empty directory: ${x || E}`);
      const H = Qr(B), se = `${U}.ofd`;
      return this.transport instanceof N ? this.put({
        contentType: "offsystem/directory",
        fileName: se,
        streamLength: H.length,
        serverAddress: r.serverAddress,
        recyclerUrls: s,
        temporary: r.temporary
      }, H) : (await this.putStreamStart({
        contentType: "offsystem/directory",
        fileName: se,
        streamLength: H.length,
        serverAddress: r.serverAddress,
        recyclerUrls: s,
        temporary: r.temporary
      }), await this.putStreamData(H), this.putStreamEnd());
    };
    return m(E);
  }
}
function zr(t) {
  if (t.length === 0) return "";
  const e = t.map((i) => i.split("/").filter(Boolean)), r = e[0];
  let n = r.length;
  for (let i = 1; i < e.length; i++) {
    const l = e[i];
    let h = 0;
    for (; h < Math.min(n, l.length) && r[h] === l[h]; )
      h++;
    if (n = h, n === 0) break;
  }
  const s = Math.min(n, r.length - 1);
  return r.slice(0, s).join("/");
}
function Jr(t, e) {
  const r = e ? `${e}/` : "";
  return t.filter((n) => {
    if (!n.path.startsWith(r)) return !1;
    const s = n.path.slice(r.length);
    return s.length > 0 && !s.includes("/");
  });
}
function Zr(t, e) {
  const r = e ? `${e}/` : "", n = /* @__PURE__ */ new Set();
  for (const s of t) {
    if (!s.path.startsWith(r)) continue;
    const i = s.path.slice(r.length);
    if (!i) continue;
    const l = i.indexOf("/");
    l > 0 && n.add(r + i.slice(0, l));
  }
  return Array.from(n);
}
var ke = globalThis.OffsClient;
ke && ke.OffsClient && (globalThis.OffsClient = ke.OffsClient);
export {
  en as OffsClient,
  ye as base58Decode,
  Me as base58Encode,
  Mr as mimeFromExtension,
  jr as offUrlToHttpUrl,
  wt as parseOffUrl,
  Xr as wire
};
//# sourceMappingURL=offs-client.esm.js.map
