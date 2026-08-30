var ar = Object.defineProperty;
var fr = (t, e, r) => e in t ? ar(t, e, { enumerable: !0, configurable: !0, writable: !0, value: r }) : t[e] = r;
var L = (t, e, r) => fr(t, typeof e != "symbol" ? e + "" : e, r);
let He;
try {
  He = new TextDecoder();
} catch {
}
let E, ce, c = 0;
const lr = 105, cr = 57342, ur = 57343, ot = 57337, at = 6, he = {};
let we = 11281e4, oe = 1681e4, A = {}, F, Ne, Be = 0, me = 0, G, Z, H = [], je = [], z, W, xe, ft = {
  useRecords: !1,
  mapsAsObjects: !0
}, Se = !1, gt = 2;
try {
  new Function("");
} catch {
  gt = 1 / 0;
}
class Ee {
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
    if (E)
      return Rt(() => (Ke(), this ? this.decode(e, r) : Ee.prototype.decode.call(ft, e, r)));
    ce = r > -1 ? r : e.length, c = 0, me = 0, Ne = null, G = null, E = e;
    try {
      W = e.dataView || (e.dataView = new DataView(e.buffer, e.byteOffset, e.byteLength));
    } catch (n) {
      throw E = null, e instanceof Uint8Array ? n : new Error("Source must be a Uint8Array or Buffer but was a " + (e && typeof e == "object" ? e.constructor.name : typeof e));
    }
    if (this instanceof Ee) {
      if (A = this, z = this.sharedValues && (this.pack ? new Array(this.maxPrivatePackedValues || 16).concat(this.sharedValues) : this.sharedValues), this.structures)
        return F = this.structures, Oe();
      (!F || F.length > 0) && (F = []);
    } else
      A = ft, (!F || F.length > 0) && (F = []), z = null;
    return Oe();
  }
  decodeMultiple(e, r) {
    let n, s = 0;
    try {
      let o = e.length;
      Se = !0;
      let l = this ? this.decode(e, o) : Xe.decode(e, o);
      if (r) {
        if (r(l) === !1)
          return;
        for (; c < o; )
          if (s = c, r(Oe()) === !1)
            return;
      } else {
        for (n = [l]; c < o; )
          s = c, n.push(Oe());
        return n;
      }
    } catch (o) {
      throw o.lastPosition = s, o.values = n, o;
    } finally {
      Se = !1, Ke();
    }
  }
}
function Oe() {
  try {
    let t = T();
    if (G) {
      if (c >= G.postBundlePosition) {
        let e = new Error("Unexpected bundle position");
        throw e.incomplete = !0, e;
      }
      c = G.postBundlePosition, G = null;
    }
    if (c == ce)
      F = null, E = null, Z && (Z = null);
    else if (c > ce) {
      let e = new Error("Unexpected end of CBOR data");
      throw e.incomplete = !0, e;
    } else if (!Se)
      throw new Error("Data read, but end of buffer not reached");
    return t;
  } catch (t) {
    throw Ke(), (t instanceof RangeError || t.message.startsWith("Unexpected end of buffer")) && (t.incomplete = !0), t;
  }
}
function T() {
  let t = E[c++], e = t >> 5;
  if (t = t & 31, t > 23)
    switch (t) {
      case 24:
        t = E[c++];
        break;
      case 25:
        if (e == 7)
          return yr();
        t = W.getUint16(c), c += 2;
        break;
      case 26:
        if (e == 7) {
          let r = W.getFloat32(c);
          if (A.useFloat32 > 2) {
            let n = Ye[(E[c] & 127) << 1 | E[c + 1] >> 7];
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
            for (; (n = T()) != he; ) {
              if (s >= we) throw new Error(`Array length exceeds ${we}`);
              r[s++] = n;
            }
            return e == 4 ? r : e == 3 ? r.join("") : Buffer.concat(r);
          case 5:
            let o;
            if (A.mapsAsObjects) {
              let l = {}, h = 0;
              if (A.keyMap)
                for (; (o = T()) != he; ) {
                  if (h++ >= oe) throw new Error(`Property count exceeds ${oe}`);
                  l[Y(A.decodeKey(o))] = T();
                }
              else
                for (; (o = T()) != he; ) {
                  if (h++ >= oe) throw new Error(`Property count exceeds ${oe}`);
                  l[Y(o)] = T();
                }
              return l;
            } else {
              xe && (A.mapsAsObjects = !0, xe = !1);
              let l = /* @__PURE__ */ new Map();
              if (A.keyMap) {
                let h = 0;
                for (; (o = T()) != he; ) {
                  if (h++ >= oe)
                    throw new Error(`Map size exceeds ${oe}`);
                  l.set(A.decodeKey(o), T());
                }
              } else {
                let h = 0;
                for (; (o = T()) != he; ) {
                  if (h++ >= oe)
                    throw new Error(`Map size exceeds ${oe}`);
                  l.set(o, T());
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
      return pr(t);
    case 3:
      if (me >= c)
        return Ne.slice(c - Be, (c += t) - Be);
      if (me == 0 && ce < 140 && t < 32) {
        let s = t < 16 ? mt(t) : hr(t);
        if (s != null)
          return s;
      }
      return dr(t);
    case 4:
      if (t >= we) throw new Error(`Array length exceeds ${we}`);
      let r = new Array(t);
      for (let s = 0; s < t; s++) r[s] = T();
      return r;
    case 5:
      if (t >= oe) throw new Error(`Map size exceeds ${we}`);
      if (A.mapsAsObjects) {
        let s = {};
        if (A.keyMap) for (let o = 0; o < t; o++) s[Y(A.decodeKey(T()))] = T();
        else for (let o = 0; o < t; o++) s[Y(T())] = T();
        return s;
      } else {
        xe && (A.mapsAsObjects = !0, xe = !1);
        let s = /* @__PURE__ */ new Map();
        if (A.keyMap) for (let o = 0; o < t; o++) s.set(A.decodeKey(T()), T());
        else for (let o = 0; o < t; o++) s.set(T(), T());
        return s;
      }
    case 6:
      if (t >= ot) {
        let s = F[t & 8191];
        if (s)
          return s.read || (s.read = qe(s)), s.read();
        if (t < 65536) {
          if (t == ur) {
            let o = ye(), l = T(), h = T();
            Ge(l, h);
            let y = {};
            if (A.keyMap) for (let m = 2; m < o; m++) {
              let g = A.decodeKey(h[m - 2]);
              y[Y(g)] = T();
            }
            else for (let m = 2; m < o; m++) {
              let g = h[m - 2];
              y[Y(g)] = T();
            }
            return y;
          } else if (t == cr) {
            let o = ye(), l = T();
            for (let h = 2; h < o; h++)
              Ge(l++, T());
            return T();
          } else if (t == ot)
            return Sr();
          if (A.getShared && (Ze(), s = F[t & 8191], s))
            return s.read || (s.read = qe(s)), s.read();
        }
      }
      let n = H[t];
      if (n)
        return n.handlesRead ? n(T) : n(T());
      {
        let s = T();
        for (let o = 0; o < je.length; o++) {
          let l = je[o](t, s);
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
    let r = E[c++];
    if (r = r & 31, r > 23)
      switch (r) {
        case 24:
          r = E[c++];
          break;
        case 25:
          r = W.getUint16(c), c += 2;
          break;
        case 26:
          r = W.getUint32(c), c += 4;
          break;
        default:
          throw new Error("Expected array header, but got " + E[c - 1]);
      }
    let n = this.compiledReader;
    for (; n; ) {
      if (n.propertyCount === r)
        return n(T);
      n = n.next;
    }
    if (this.slowReads++ >= gt) {
      let o = this.length == r ? this : this.slice(0, r);
      return n = A.keyMap ? new Function("r", "return {" + o.map((l) => A.decodeKey(l)).map((l) => lt.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}") : new Function("r", "return {" + o.map((l) => lt.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}"), this.compiledReader && (n.next = this.compiledReader), n.propertyCount = r, this.compiledReader = n, n(T);
    }
    let s = {};
    if (A.keyMap) for (let o = 0; o < r; o++) s[Y(A.decodeKey(this[o]))] = T();
    else for (let o = 0; o < r; o++)
      s[Y(this[o])] = T();
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
let dr = $e;
function $e(t) {
  let e;
  if (t < 16 && (e = mt(t)))
    return e;
  if (t > 64 && He)
    return He.decode(E.subarray(c, c += t));
  const r = c + t, n = [];
  for (e = ""; c < r; ) {
    const s = E[c++];
    if (!(s & 128))
      n.push(s);
    else if ((s & 224) === 192)
      if (s < 194 || c >= r || (E[c] & 192) !== 128)
        n.push(65533);
      else {
        const o = E[c++] & 63;
        n.push((s & 31) << 6 | o);
      }
    else if ((s & 240) === 224) {
      const o = c < r ? E[c] : 0;
      if (c >= r || (o & 192) !== 128 || s === 224 && o < 160 || s === 237 && o >= 160)
        n.push(65533);
      else if (c++, c >= r || (E[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = E[c++] & 63;
        n.push((s & 31) << 12 | (o & 63) << 6 | l);
      }
    } else if ((s & 248) === 240) {
      const o = c < r ? E[c] : 0;
      if (s > 244 || c >= r || (o & 192) !== 128 || s === 240 && o < 144 || s === 244 && o >= 144)
        n.push(65533);
      else if (c++, c >= r || (E[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = E[c++] & 63;
        if (c >= r || (E[c] & 192) !== 128)
          n.push(65533);
        else {
          const h = E[c++] & 63;
          let y = (s & 7) << 18 | (o & 63) << 12 | l << 6 | h;
          y -= 65536, n.push(y >>> 10 & 1023 | 55296), n.push(56320 | y & 1023);
        }
      }
    } else
      n.push(65533);
    n.length >= 4096 && (e += K.apply(String, n), n.length = 0);
  }
  return n.length > 0 && (e += K.apply(String, n)), e;
}
let K = String.fromCharCode;
function hr(t) {
  let e = c, r = new Array(t);
  for (let n = 0; n < t; n++) {
    const s = E[c++];
    if ((s & 128) > 0) {
      c = e;
      return;
    }
    r[n] = s;
  }
  return K.apply(String, r);
}
function mt(t) {
  if (t < 4)
    if (t < 2) {
      if (t === 0)
        return "";
      {
        let e = E[c++];
        if ((e & 128) > 1) {
          c -= 1;
          return;
        }
        return K(e);
      }
    } else {
      let e = E[c++], r = E[c++];
      if ((e & 128) > 0 || (r & 128) > 0) {
        c -= 2;
        return;
      }
      if (t < 3)
        return K(e, r);
      let n = E[c++];
      if ((n & 128) > 0) {
        c -= 3;
        return;
      }
      return K(e, r, n);
    }
  else {
    let e = E[c++], r = E[c++], n = E[c++], s = E[c++];
    if ((e & 128) > 0 || (r & 128) > 0 || (n & 128) > 0 || (s & 128) > 0) {
      c -= 4;
      return;
    }
    if (t < 6) {
      if (t === 4)
        return K(e, r, n, s);
      {
        let o = E[c++];
        if ((o & 128) > 0) {
          c -= 5;
          return;
        }
        return K(e, r, n, s, o);
      }
    } else if (t < 8) {
      let o = E[c++], l = E[c++];
      if ((o & 128) > 0 || (l & 128) > 0) {
        c -= 6;
        return;
      }
      if (t < 7)
        return K(e, r, n, s, o, l);
      let h = E[c++];
      if ((h & 128) > 0) {
        c -= 7;
        return;
      }
      return K(e, r, n, s, o, l, h);
    } else {
      let o = E[c++], l = E[c++], h = E[c++], y = E[c++];
      if ((o & 128) > 0 || (l & 128) > 0 || (h & 128) > 0 || (y & 128) > 0) {
        c -= 8;
        return;
      }
      if (t < 10) {
        if (t === 8)
          return K(e, r, n, s, o, l, h, y);
        {
          let m = E[c++];
          if ((m & 128) > 0) {
            c -= 9;
            return;
          }
          return K(e, r, n, s, o, l, h, y, m);
        }
      } else if (t < 12) {
        let m = E[c++], g = E[c++];
        if ((m & 128) > 0 || (g & 128) > 0) {
          c -= 10;
          return;
        }
        if (t < 11)
          return K(e, r, n, s, o, l, h, y, m, g);
        let P = E[c++];
        if ((P & 128) > 0) {
          c -= 11;
          return;
        }
        return K(e, r, n, s, o, l, h, y, m, g, P);
      } else {
        let m = E[c++], g = E[c++], P = E[c++], N = E[c++];
        if ((m & 128) > 0 || (g & 128) > 0 || (P & 128) > 0 || (N & 128) > 0) {
          c -= 12;
          return;
        }
        if (t < 14) {
          if (t === 12)
            return K(e, r, n, s, o, l, h, y, m, g, P, N);
          {
            let q = E[c++];
            if ((q & 128) > 0) {
              c -= 13;
              return;
            }
            return K(e, r, n, s, o, l, h, y, m, g, P, N, q);
          }
        } else {
          let q = E[c++], v = E[c++];
          if ((q & 128) > 0 || (v & 128) > 0) {
            c -= 14;
            return;
          }
          if (t < 15)
            return K(e, r, n, s, o, l, h, y, m, g, P, N, q, v);
          let B = E[c++];
          if ((B & 128) > 0) {
            c -= 15;
            return;
          }
          return K(e, r, n, s, o, l, h, y, m, g, P, N, q, v, B);
        }
      }
    }
  }
}
function pr(t) {
  return A.copyBuffers ? (
    // specifically use the copying slice (not the node one)
    Uint8Array.prototype.slice.call(E, c, c += t)
  ) : E.subarray(c, c += t);
}
let St = new Float32Array(1), Ae = new Uint8Array(St.buffer, 0, 4);
function yr() {
  let t = E[c++], e = E[c++], r = (t & 127) >> 2;
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
H[0] = (t) => new Date(t);
H[1] = (t) => new Date(Math.round(t * 1e3));
H[2] = (t) => {
  let e = BigInt(0);
  for (let r = 0, n = t.byteLength; r < n; r++)
    e = BigInt(t[r]) + (e << BigInt(8));
  return e;
};
H[3] = (t) => BigInt(-1) - H[2](t);
H[4] = (t) => +(t[1] + "e" + t[0]);
H[5] = (t) => t[1] * Math.exp(t[0] * Math.log(2));
const Ge = (t, e) => {
  t = t - 57344;
  let r = F[t];
  r && r.isShared && ((F.restoreStructures || (F.restoreStructures = []))[t] = r), F[t] = e, e.read = qe(e);
};
H[lr] = (t) => {
  let e = t.length, r = t[1];
  Ge(t[0], r);
  let n = {};
  for (let s = 2; s < e; s++) {
    let o = r[s - 2];
    n[Y(o)] = t[s];
  }
  return n;
};
H[14] = (t) => G ? G[0].slice(G.position0, G.position0 += t) : new ue(t, 14);
H[15] = (t) => G ? G[1].slice(G.position1, G.position1 += t) : new ue(t, 15);
let Er = { Error, RegExp };
H[27] = (t) => (Er[t[0]] || Error)(t[1], t[2]);
const _t = (t) => {
  if (E[c++] != 132) {
    let r = new Error("Packed values structure must be followed by a 4 element array");
    throw E.length < c && (r.incomplete = !0), r;
  }
  let e = t();
  if (!e || !e.length) {
    let r = new Error("Packed values structure must be followed by a 4 element array");
    throw r.incomplete = !0, r;
  }
  return z = z ? e.concat(z.slice(e.length)) : e, z.prefixes = t(), z.suffixes = t(), t();
};
_t.handlesRead = !0;
H[51] = _t;
H[at] = (t) => {
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
H[28] = (t) => {
  Z || (Z = /* @__PURE__ */ new Map(), Z.id = 0);
  let e = Z.id++, r = c, n = E[c], s;
  n >> 5 == 4 ? s = [] : s = {};
  let o = { target: s };
  Z.set(e, o);
  let l = t();
  return o.used ? (Object.getPrototypeOf(s) !== Object.getPrototypeOf(l) && (c = r, s = l, Z.set(e, { target: s }), l = t()), Object.assign(s, l)) : (o.target = l, l);
};
H[28].handlesRead = !0;
H[29] = (t) => {
  let e = Z.get(t);
  return e.used = !0, e.target;
};
H[258] = (t) => new Set(t);
(H[259] = (t) => (A.mapsAsObjects && (A.mapsAsObjects = !1, xe = !0), t())).handlesRead = !0;
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
const wr = 1399353956;
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
  if (t == wr)
    return {
      packedValues: z,
      structures: F.slice(0),
      version: e
    };
  if (t == 55799)
    return e;
});
const xr = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1, ct = [
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
], gr = [64, 68, 69, 70, 71, 72, 77, 78, 79, 85, 86];
for (let t = 0; t < ct.length; t++)
  mr(ct[t], gr[t]);
function mr(t, e) {
  let r = "get" + t.name.slice(0, -5), n;
  typeof t == "function" ? n = t.BYTES_PER_ELEMENT : t = null;
  for (let s = 0; s < 2; s++) {
    if (!s && n == 1)
      continue;
    let o = n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : 0;
    H[s ? e : e - 4] = n == 1 || s == xr ? (l) => {
      if (!t)
        throw new Error("Could not find typed array for code " + e);
      return !A.copyBuffers && (n === 1 || n === 2 && !(l.byteOffset & 1) || n === 4 && !(l.byteOffset & 3) || n === 8 && !(l.byteOffset & 7)) ? new t(l.buffer, l.byteOffset, l.byteLength >> o) : new t(Uint8Array.prototype.slice.call(l, 0).buffer);
    } : (l) => {
      if (!t)
        throw new Error("Could not find typed array for code " + e);
      let h = new DataView(l.buffer, l.byteOffset, l.byteLength), y = l.length >> o, m = new t(y), g = h[r];
      for (let P = 0; P < y; P++)
        m[P] = g.call(h, P << o, s);
      return m;
    };
  }
}
function Sr() {
  let t = ye(), e = c + T();
  for (let n = 2; n < t; n++) {
    let s = ye();
    c += s;
  }
  let r = c;
  return c = e, G = [$e(ye()), $e(ye())], G.position0 = 0, G.position1 = 0, G.postBundlePosition = c, c = r, T();
}
function ye() {
  let t = E[c++] & 31;
  if (t > 23)
    switch (t) {
      case 24:
        t = E[c++];
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
    let t = Rt(() => (E = null, A.getShared())) || {}, e = t.structures || [];
    A.sharedVersion = t.version, z = A.sharedValues = t.packedValues, F === !0 ? A.structures = F = e : F.splice.apply(F, [0, e.length].concat(e));
  }
}
function Rt(t) {
  let e = ce, r = c, n = Be, s = me, o = Ne, l = Z, h = G, y = new Uint8Array(E.slice(0, ce)), m = F, g = A, P = Se, N = t();
  return ce = e, c = r, Be = n, me = s, Ne = o, Z = l, G = h, E = y, Se = P, F = m, A = g, W = new DataView(E.buffer, E.byteOffset, E.byteLength), N;
}
function Ke() {
  E = null, Z = null, F = null;
}
const Ye = new Array(147);
for (let t = 0; t < 256; t++)
  Ye[t] = +("1e" + Math.floor(45.15 - t * 0.30103));
let Xe = new Ee({ useRecords: !1 });
const j = Xe.decode;
Xe.decodeMultiple;
let Ue;
try {
  Ue = new TextEncoder();
} catch {
}
let ve, bt;
const Ce = typeof globalThis == "object" && globalThis.Buffer, _e = typeof Ce < "u", Le = _e ? Ce.allocUnsafeSlow : Uint8Array, ut = _e ? Ce : Uint8Array, dt = 256, ht = _e ? 4294967296 : 2144337920;
let Ie, f, C, i = 0, ae, $ = null;
const _r = 61440, Rr = /[\u0080-\uFFFF]/, Q = Symbol("record-id");
class et extends Ee {
  constructor(e) {
    super(e), this.offset = 0;
    let r, n, s, o, l;
    e = e || {};
    let h = ut.prototype.utf8Write ? function(a, w) {
      return f.utf8Write(a, w, f.byteLength - w);
    } : Ue && Ue.encodeInto ? function(a, w) {
      return Ue.encodeInto(a, f.subarray(w)).written;
    } : !1, y = this, m = e.structures || e.saveStructures, g = e.maxSharedStructures;
    if (g == null && (g = m ? 128 : 0), g > 8190)
      throw new Error("Maximum maxSharedStructure is 8190");
    let P = e.sequential;
    P && (g = 0), this.structures || (this.structures = []), this.saveStructures && (this.saveShared = this.saveStructures);
    let N, q, v = e.sharedValues, B;
    if (v) {
      B = /* @__PURE__ */ Object.create(null);
      for (let a = 0, w = v.length; a < w; a++)
        B[v[a]] = a;
    }
    let M = [], ne = 0, I = 0;
    this.mapEncode = function(a, w) {
      if (this._keyMap && !this._mapped)
        switch (a.constructor.name) {
          case "Array":
            a = a.map((d) => this.encodeKeys(d));
            break;
        }
      return this.encode(a, w);
    }, this.encode = function(a, w) {
      if (f || (f = new Le(8192), C = new DataView(f.buffer, 0, 8192), i = 0), ae = f.length - 10, ae - i < 2048 ? (f = new Le(f.length), C = new DataView(f.buffer, 0, f.length), ae = f.length - 10, i = 0) : w === Et && (i = i + 7 & 2147483640), r = i, y.useSelfDescribedHeader && (C.setUint32(i, 3654940416), i += 3), l = y.structuredClone ? /* @__PURE__ */ new Map() : null, y.bundleStrings && typeof a != "string" ? ($ = [], $.size = 1 / 0) : $ = null, n = y.structures, n) {
        if (n.uninitialized) {
          let p = y.getShared() || {};
          y.structures = n = p.structures || [], y.sharedVersion = p.version;
          let u = y.sharedValues = p.packedValues;
          if (u) {
            B = {};
            for (let S = 0, R = u.length; S < R; S++)
              B[u[S]] = S;
          }
        }
        let d = n.length;
        if (d > g && !P && (d = g), !n.transitions) {
          n.transitions = /* @__PURE__ */ Object.create(null);
          for (let p = 0; p < d; p++) {
            let u = n[p];
            if (!u)
              continue;
            let S, R = n.transitions;
            for (let b = 0, O = u.length; b < O; b++) {
              R[Q] === void 0 && (R[Q] = p);
              let U = u[b];
              S = R[U], S || (S = R[U] = /* @__PURE__ */ Object.create(null)), R = S;
            }
            R[Q] = p | 1048576;
          }
        }
        P || (n.nextId = d);
      }
      if (s && (s = !1), o = n || [], q = B, e.pack) {
        let d = /* @__PURE__ */ new Map();
        if (d.values = [], d.encoder = y, d.maxValues = e.maxPrivatePackedValues || (B ? 16 : 1 / 0), d.objectMap = B || !1, d.samplingPackedValues = N, Pe(a, d), d.values.length > 0) {
          f[i++] = 216, f[i++] = 51, te(4);
          let p = d.values;
          _(p), te(0), te(0), q = Object.create(B || null);
          for (let u = 0, S = p.length; u < S; u++)
            q[p[u]] = u;
        }
      }
      Ie = w & Fe;
      try {
        if (Ie)
          return;
        if (_(a), $ && yt(r, _), y.offset = i, l && l.idsToInsert) {
          i += l.idsToInsert.length * 2, i > ae && J(i), y.offset = i;
          let d = Ar(f.subarray(r, i), l.idsToInsert);
          return l = null, d;
        }
        return w & Et ? (f.start = r, f.end = i, f) : f.subarray(r, i);
      } finally {
        if (n) {
          if (I < 10 && I++, n.length > g && (n.length = g), ne > 1e4)
            n.transitions = null, I = 0, ne = 0, M.length > 0 && (M = []);
          else if (M.length > 0 && !P) {
            for (let d = 0, p = M.length; d < p; d++)
              M[d][Q] = void 0;
            M = [];
          }
        }
        if (s && y.saveShared) {
          y.structures.length > g && (y.structures = y.structures.slice(0, g));
          let d = f.subarray(r, i);
          return y.updateSharedData() === !1 ? y.encode(a) : d;
        }
        w & Tr && (i = r);
      }
    }, this.findCommonStringsToPack = () => (N = /* @__PURE__ */ new Map(), B || (B = /* @__PURE__ */ Object.create(null)), (a) => {
      let w = a && a.threshold || 4, d = this.pack ? a.maxPrivatePackedValues || 16 : 0;
      v || (v = this.sharedValues = []);
      for (let [p, u] of N)
        u.count > w && (B[p] = d++, v.push(p), s = !0);
      for (; this.saveShared && this.updateSharedData() === !1; )
        ;
      N = null;
    });
    const _ = (a) => {
      i > ae && (f = J(i));
      var w = typeof a, d;
      if (w === "string") {
        if (q) {
          let R = q[a];
          if (R >= 0) {
            R < 16 ? f[i++] = R + 224 : (f[i++] = 198, R & 1 ? _(15 - R >> 1) : _(R - 16 >> 1));
            return;
          } else if (N && !e.pack) {
            let b = N.get(a);
            b ? b.count++ : N.set(a, {
              count: 1
            });
          }
        }
        let p = a.length;
        if ($ && p >= 4 && p < 1024) {
          if (($.size += p) > _r) {
            let b, O = ($[0] ? $[0].length * 3 + $[1].length : 0) + 10;
            i + O > ae && (f = J(i + O)), f[i++] = 217, f[i++] = 223, f[i++] = 249, f[i++] = $.position ? 132 : 130, f[i++] = 26, b = i - r, i += 4, $.position && yt(r, _), $ = ["", ""], $.size = 0, $.position = b;
          }
          let R = Rr.test(a);
          $[R ? 0 : 1] += a, f[i++] = R ? 206 : 207, _(p);
          return;
        }
        let u;
        p < 32 ? u = 1 : p < 256 ? u = 2 : p < 65536 ? u = 3 : u = 5;
        let S = p * 3;
        if (i + S > ae && (f = J(i + S)), p < 64 || !h) {
          let R, b, O, U = i + u;
          for (R = 0; R < p; R++)
            b = a.charCodeAt(R), b < 128 ? f[U++] = b : b < 2048 ? (f[U++] = b >> 6 | 192, f[U++] = b & 63 | 128) : (b & 64512) === 55296 && ((O = a.charCodeAt(R + 1)) & 64512) === 56320 ? (b = 65536 + ((b & 1023) << 10) + (O & 1023), R++, f[U++] = b >> 18 | 240, f[U++] = b >> 12 & 63 | 128, f[U++] = b >> 6 & 63 | 128, f[U++] = b & 63 | 128) : (f[U++] = b >> 12 | 224, f[U++] = b >> 6 & 63 | 128, f[U++] = b & 63 | 128);
          d = U - i - u;
        } else
          d = h(a, i + u, S);
        d < 24 ? f[i++] = 96 | d : d < 256 ? (u < 2 && f.copyWithin(i + 2, i + 1, i + 1 + d), f[i++] = 120, f[i++] = d) : d < 65536 ? (u < 3 && f.copyWithin(i + 3, i + 2, i + 2 + d), f[i++] = 121, f[i++] = d >> 8, f[i++] = d & 255) : (u < 5 && f.copyWithin(i + 5, i + 3, i + 3 + d), f[i++] = 122, C.setUint32(i, d), i += 4), i += d;
      } else if (w === "number")
        if (!this.alwaysUseFloat && a >>> 0 === a)
          a < 24 ? f[i++] = a : a < 256 ? (f[i++] = 24, f[i++] = a) : a < 65536 ? (f[i++] = 25, f[i++] = a >> 8, f[i++] = a & 255) : (f[i++] = 26, C.setUint32(i, a), i += 4);
        else if (!this.alwaysUseFloat && a >> 0 === a)
          a >= -24 ? f[i++] = 31 - a : a >= -256 ? (f[i++] = 56, f[i++] = ~a) : a >= -65536 ? (f[i++] = 57, C.setUint16(i, ~a), i += 2) : (f[i++] = 58, C.setUint32(i, ~a), i += 4);
        else if (!this.alwaysUseFloat && a < 0 && a >= -4294967296 && Math.floor(a) === a)
          f[i++] = 58, C.setUint32(i, -1 - a), i += 4;
        else {
          let p;
          if ((p = this.useFloat32) > 0 && a < 4294967296 && a >= -2147483648) {
            f[i++] = 250, C.setFloat32(i, a);
            let u;
            if (p < 4 || // this checks for rounding of numbers that were encoded in 32-bit float to nearest significant decimal digit that could be preserved
            (u = a * Ye[(f[i] & 127) << 1 | f[i + 1] >> 7]) >> 0 === u) {
              i += 4;
              return;
            } else
              i--;
          }
          f[i++] = 251, C.setFloat64(i, a), i += 8;
        }
      else if (w === "object")
        if (!a)
          f[i++] = 246;
        else {
          if (l) {
            let u = l.get(a);
            if (u) {
              if (f[i++] = 216, f[i++] = 29, f[i++] = 25, !u.references) {
                let S = l.idsToInsert || (l.idsToInsert = []);
                u.references = [], S.push(u);
              }
              u.references.push(i - r), i += 2;
              return;
            } else
              l.set(a, { offset: i - r });
          }
          let p = a.constructor;
          if (p === Object)
            this.skipFunction === !0 && (a = Object.fromEntries([...Object.keys(a).filter((u) => typeof a[u] != "function").map((u) => [u, a[u]])])), X(a);
          else if (p === Array) {
            d = a.length, d < 24 ? f[i++] = 128 | d : te(d);
            for (let u = 0; u < d; u++)
              _(a[u]);
          } else if (p === Map)
            if ((this.mapsAsObjects ? this.useTag259ForMaps !== !1 : this.useTag259ForMaps) && (f[i++] = 217, f[i++] = 1, f[i++] = 3), d = a.size, d < 24 ? f[i++] = 160 | d : d < 256 ? (f[i++] = 184, f[i++] = d) : d < 65536 ? (f[i++] = 185, f[i++] = d >> 8, f[i++] = d & 255) : (f[i++] = 186, C.setUint32(i, d), i += 4), y.keyMap)
              for (let [u, S] of a)
                _(y.encodeKey(u)), _(S);
            else
              for (let [u, S] of a)
                _(u), _(S);
          else {
            for (let u = 0, S = ve.length; u < S; u++) {
              let R = bt[u];
              if (a instanceof R) {
                let b = ve[u], O = b.tag;
                O == null && (O = b.getTag && b.getTag.call(this, a)), O < 24 ? f[i++] = 192 | O : O < 256 ? (f[i++] = 216, f[i++] = O) : O < 65536 ? (f[i++] = 217, f[i++] = O >> 8, f[i++] = O & 255) : O > -1 && (f[i++] = 218, C.setUint32(i, O), i += 4), b.encode.call(this, a, _, J);
                return;
              }
            }
            if (a[Symbol.iterator]) {
              if (Ie) {
                let u = new Error("Iterable should be serialized as iterator");
                throw u.iteratorNotHandled = !0, u;
              }
              f[i++] = 159;
              for (let u of a)
                _(u);
              f[i++] = 255;
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
      else if (w === "boolean")
        f[i++] = a ? 245 : 244;
      else if (w === "bigint") {
        if (a < BigInt(1) << BigInt(64) && a >= 0)
          f[i++] = 27, C.setBigUint64(i, a);
        else if (a > -(BigInt(1) << BigInt(64)) && a < 0)
          f[i++] = 59, C.setBigUint64(i, -a - BigInt(1));
        else if (this.largeBigIntToFloat)
          f[i++] = 251, C.setFloat64(i, Number(a));
        else {
          a >= BigInt(0) ? f[i++] = 194 : (f[i++] = 195, a = BigInt(-1) - a);
          let p = [];
          for (; a; )
            p.push(Number(a & BigInt(255))), a >>= BigInt(8);
          Ve(new Uint8Array(p.reverse()), J);
          return;
        }
        i += 8;
      } else if (w === "undefined")
        f[i++] = 247;
      else
        throw new Error("Unknown type: " + w);
    }, X = this.useRecords === !1 ? this.variableMapSize ? (a) => {
      let w = Object.keys(a), d = Object.values(a), p = w.length;
      if (p < 24 ? f[i++] = 160 | p : p < 256 ? (f[i++] = 184, f[i++] = p) : p < 65536 ? (f[i++] = 185, f[i++] = p >> 8, f[i++] = p & 255) : (f[i++] = 186, C.setUint32(i, p), i += 4), y.keyMap)
        for (let u = 0; u < p; u++)
          _(y.encodeKey(w[u])), _(d[u]);
      else
        for (let u = 0; u < p; u++)
          _(w[u]), _(d[u]);
    } : (a) => {
      f[i++] = 185;
      let w = i - r;
      i += 2;
      let d = 0;
      if (y.keyMap)
        for (let p in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(p)) && (_(y.encodeKey(p)), _(a[p]), d++);
      else
        for (let p in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(p)) && (_(p), _(a[p]), d++);
      f[w++ + r] = d >> 8, f[w + r] = d & 255;
    } : (a, w) => {
      let d, p = o.transitions || (o.transitions = /* @__PURE__ */ Object.create(null)), u = 0, S = 0, R, b;
      if (this.keyMap) {
        b = Object.keys(a).map((U) => this.encodeKey(U)), S = b.length;
        for (let U = 0; U < S; U++) {
          let it = b[U];
          d = p[it], d || (d = p[it] = /* @__PURE__ */ Object.create(null), u++), p = d;
        }
      } else
        for (let U in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(U)) && (d = p[U], d || (p[Q] & 1048576 && (R = p[Q] & 65535), d = p[U] = /* @__PURE__ */ Object.create(null), u++), p = d, S++);
      let O = p[Q];
      if (O !== void 0)
        O &= 65535, f[i++] = 217, f[i++] = O >> 8 | 224, f[i++] = O & 255;
      else if (b || (b = p.__keys__ || (p.__keys__ = Object.keys(a))), R === void 0 ? (O = o.nextId++, O || (O = 0, o.nextId = 1), O >= dt && (o.nextId = (O = g) + 1)) : O = R, o[O] = b, O < g) {
        f[i++] = 217, f[i++] = O >> 8 | 224, f[i++] = O & 255, p = o.transitions;
        for (let U = 0; U < S; U++)
          (p[Q] === void 0 || p[Q] & 1048576) && (p[Q] = O), p = p[b[U]];
        p[Q] = O | 1048576, s = !0;
      } else {
        if (p[Q] = O, C.setUint32(i, 3655335680), i += 3, u && (ne += I * u), M.length >= dt - g && (M.shift()[Q] = void 0), M.push(p), te(S + 2), _(57344 + O), _(b), w) return;
        for (let U in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(U)) && _(a[U]);
        return;
      }
      if (S < 24 ? f[i++] = 128 | S : te(S), !w)
        for (let U in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(U)) && _(a[U]);
    }, J = (a) => {
      let w;
      if (a > 16777216) {
        if (a - r > ht)
          throw new Error("Encoded buffer would be larger than maximum buffer size");
        w = Math.min(
          ht,
          Math.round(Math.max((a - r) * (a > 67108864 ? 1.25 : 2), 4194304) / 4096) * 4096
        );
      } else
        w = (Math.max(a - r << 2, f.length - 1) >> 12) + 1 << 12;
      let d = new Le(w);
      return C = new DataView(d.buffer, 0, w), f.copy ? f.copy(d, 0, r, a) : d.set(f.slice(r, a)), i -= r, r = 0, ae = d.length - 10, f = d;
    };
    let V = 100, fe = 1e3;
    this.encodeAsIterable = function(a, w) {
      return Re(a, w, se);
    }, this.encodeAsAsyncIterable = function(a, w) {
      return Re(a, w, be);
    };
    function* se(a, w, d) {
      let p = a.constructor;
      if (p === Object) {
        let u = y.useRecords !== !1;
        u ? X(a, !0) : pt(Object.keys(a).length, 160);
        for (let S in a) {
          let R = a[S];
          u || _(S), R && typeof R == "object" ? w[S] ? yield* se(R, w[S]) : yield* de(R, w, S) : _(R);
        }
      } else if (p === Array) {
        let u = a.length;
        te(u);
        for (let S = 0; S < u; S++) {
          let R = a[S];
          R && (typeof R == "object" || i - r > V) ? w.element ? yield* se(R, w.element) : yield* de(R, w, "element") : _(R);
        }
      } else if (a[Symbol.iterator] && !a.buffer) {
        f[i++] = 159;
        for (let u of a)
          u && (typeof u == "object" || i - r > V) ? w.element ? yield* se(u, w.element) : yield* de(u, w, "element") : _(u);
        f[i++] = 255;
      } else De(a) ? (pt(a.size, 64), yield f.subarray(r, i), yield a, ie()) : a[Symbol.asyncIterator] ? (f[i++] = 159, yield f.subarray(r, i), yield a, ie(), f[i++] = 255) : _(a);
      d && i > r ? yield f.subarray(r, i) : i - r > V && (yield f.subarray(r, i), ie());
    }
    function* de(a, w, d) {
      let p = i - r;
      try {
        _(a), i - r > V && (yield f.subarray(r, i), ie());
      } catch (u) {
        if (u.iteratorNotHandled)
          w[d] = {}, i = r + p, yield* se.call(this, a, w[d]);
        else throw u;
      }
    }
    function ie() {
      V = fe, y.encode(null, Fe);
    }
    function Re(a, w, d) {
      return w && w.chunkThreshold ? V = fe = w.chunkThreshold : V = 100, a && typeof a == "object" ? (y.encode(null, Fe), d(a, y.iterateProperties || (y.iterateProperties = {}), !0)) : [y.encode(a)];
    }
    async function* be(a, w) {
      for (let d of se(a, w, !0)) {
        let p = d.constructor;
        if (p === ut || p === Uint8Array)
          yield d;
        else if (De(d)) {
          let u = d.stream().getReader(), S;
          for (; !(S = await u.read()).done; )
            yield S.value;
        } else if (d[Symbol.asyncIterator])
          for await (let u of d)
            ie(), u ? yield* be(u, w.async || (w.async = {})) : yield y.encode(u);
        else
          yield d;
      }
    }
  }
  useBuffer(e) {
    f = e, C = new DataView(f.buffer, f.byteOffset, f.byteLength), i = 0;
  }
  clearSharedData() {
    this.structures && (this.structures = []), this.sharedValues && (this.sharedValues = void 0);
  }
  updateSharedData() {
    let e = this.sharedVersion || 0;
    this.sharedVersion = e + 1;
    let r = this.structures.slice(0), n = new Ot(r, this.sharedValues, this.sharedVersion), s = this.saveShared(
      n,
      (o) => (o && o.version || 0) == e
    );
    return s === !1 ? (n = this.getShared() || {}, this.structures = n.structures || [], this.sharedValues = n.packedValues, this.sharedVersion = n.version, this.structures.nextId = this.structures.length) : r.forEach((o, l) => this.structures[l] = o), s;
  }
}
function pt(t, e) {
  t < 24 ? f[i++] = e | t : t < 256 ? (f[i++] = e | 24, f[i++] = t) : t < 65536 ? (f[i++] = e | 25, f[i++] = t >> 8, f[i++] = t & 255) : (f[i++] = e | 26, C.setUint32(i, t), i += 4);
}
class Ot {
  constructor(e, r, n) {
    this.structures = e, this.packedValues = r, this.version = n;
  }
}
function te(t) {
  t < 24 ? f[i++] = 128 | t : t < 256 ? (f[i++] = 152, f[i++] = t) : t < 65536 ? (f[i++] = 153, f[i++] = t >> 8, f[i++] = t & 255) : (f[i++] = 154, C.setUint32(i, t), i += 4);
}
const br = typeof Blob > "u" ? function() {
} : Blob;
function De(t) {
  if (t instanceof br)
    return !0;
  let e = t[Symbol.toStringTag];
  return e === "Blob" || e === "File";
}
function Pe(t, e) {
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
            Pe(t[n], e);
        else {
          let n = !e.encoder.useRecords;
          for (var r in t)
            t.hasOwnProperty(r) && (n && Pe(r, e), Pe(t[r], e));
        }
      break;
    case "function":
      console.log(t);
  }
}
const Or = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1;
bt = [
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
  Ot
];
ve = [
  {
    // Date
    tag: 1,
    encode(t, e) {
      let r = t.getTime() / 1e3;
      (this.useTimestamp32 || t.getMilliseconds() === 0) && r >= 0 && r < 4294967296 ? (f[i++] = 26, C.setUint32(i, r), i += 4) : (f[i++] = 251, C.setFloat64(i, r), i += 8);
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
      Ve(t, r);
    }
  },
  {
    // Uint8Array
    getTag(t) {
      if (t.constructor === Uint8Array && (this.tagUint8Array || _e && this.tagUint8Array !== !1))
        return 64;
    },
    encode(t, e, r) {
      Ve(t, r);
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
        C.setUint32(i, 3655335424), i += 3;
        let s = n.slice(0);
        s.unshift(57344), s.push(new ue(t.version, 1399353956)), e(s);
      } else
        e(new ue(t.version, 1399353956));
    }
  }
];
function ee(t, e) {
  return !Or && e > 1 && (t -= 4), {
    tag: t,
    encode: function(n, s) {
      let o = n.byteLength, l = n.byteOffset || 0, h = n.buffer || n;
      s(_e ? Ce.from(h, l, o) : new Uint8Array(h, l, o));
    }
  };
}
function Ve(t, e) {
  let r = t.byteLength;
  r < 24 ? f[i++] = 64 + r : r < 256 ? (f[i++] = 88, f[i++] = r) : r < 65536 ? (f[i++] = 89, f[i++] = r >> 8, f[i++] = r & 255) : (f[i++] = 90, C.setUint32(i, r), i += 4), i + r >= f.length && e(i + r), f.set(t.buffer ? t : new Uint8Array(t), i), i += r;
}
function Ar(t, e) {
  let r, n = e.length * 2, s = t.length - n;
  e.sort((o, l) => o.offset > l.offset ? 1 : -1);
  for (let o = 0; o < e.length; o++) {
    let l = e[o];
    l.id = o;
    for (let h of l.references)
      t[h++] = o >> 8, t[h] = o & 255;
  }
  for (; r = e.pop(); ) {
    let o = r.offset;
    t.copyWithin(o + n, o, s), n -= 2;
    let l = o + n;
    t[l++] = 216, t[l++] = 28, s = o;
  }
  return t;
}
function yt(t, e) {
  C.setUint32($.position + t, i - $.position - t + 1);
  let r = $;
  $ = null, e(r[0]), e(r[1]);
}
let tt = new et({ useRecords: !1 });
tt.encode;
tt.encodeAsIterable;
tt.encodeAsAsyncIterable;
const Et = 512, Tr = 1024, Fe = 2048, k = new et({ tagUint8Array: !1 }), x = {
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
  LOAD_END: 41
}, Ur = {
  loaded: 0,
  partial: 1,
  failed: 2
}, re = { cbor: 0, base58: 1, qrcode: 2 }, We = {
  [re.cbor]: "application/cbor",
  [re.base58]: "text/plain",
  [re.qrcode]: "image/x-portable-pixmap"
}, Pr = {
  OK: 0,
  BAD_REQUEST: 1,
  NOT_FOUND: 2,
  INTERNAL_ERROR: 3,
  RANGE_NOT_SATISFIABLE: 4,
  UNAUTHORIZED: 5
};
function rt(t) {
  const e = j(t);
  return Array.isArray(e) ? e[0] : null;
}
function nt(t) {
  const e = new TextEncoder().encode(t);
  return k.encode([x.AUTH_REQUEST, e]);
}
function ze(t, e = null) {
  const r = t.recyclerUrls || [], n = [
    x.PUT_REQUEST,
    t.contentType,
    t.fileName,
    t.streamLength,
    t.serverAddress || null,
    e || new Uint8Array(0),
    r,
    t.temporary ? 1 : 0
  ];
  return t.tupleSize !== void 0 && n.push(t.tupleSize), k.encode(n);
}
function At(t) {
  return k.encode([x.PUT_DATA, t]);
}
function Tt() {
  return k.encode([x.PUT_END]);
}
function Qe(t) {
  const e = j(t);
  if (e[0] !== x.PUT_RESPONSE) throw new Error("Not a put response");
  return { oriString: e[1] };
}
function Ut(t, e) {
  const r = e && (e.start !== void 0 || e.end !== void 0), n = [x.GET_REQUEST, t, r ? 1 : 0];
  return r && (n.push(e.start || 0), n.push(e.end || 0)), k.encode(n);
}
function Pt(t) {
  const e = j(t);
  if (e[0] !== x.GET_RESPONSE_START) throw new Error("Not a get response start");
  return {
    contentType: e[1],
    contentLength: e[2],
    hasRange: e[3] === 1,
    rangeStart: e[3] ? e[4] : void 0,
    rangeEnd: e[3] ? e[5] : void 0
  };
}
function Nt(t) {
  const e = j(t);
  if (e[0] !== x.GET_DATA) throw new Error("Not a get data");
  return e[1];
}
function Bt(t) {
  const e = j(t);
  return Array.isArray(e) && e[0] === x.GET_END;
}
function Ct(t, e) {
  return e && (e.start !== void 0 || e.end !== void 0) ? k.encode([
    x.LOAD_REQUEST,
    t,
    1,
    e.start || 0,
    e.end || 0
  ]) : k.encode([x.LOAD_REQUEST, t]);
}
function Lt(t) {
  const e = j(t);
  if (!(Array.isArray(e) && e[0] === x.LOAD_PROGRESS))
    throw new Error("Not a load progress");
  return { tuplesLoaded: e[1], tuplesTotal: e[2] };
}
function It(t) {
  const e = j(t);
  return Array.isArray(e) && e[0] === x.LOAD_END;
}
function Dt(t) {
  const e = j(t);
  if (!(Array.isArray(e) && e[0] === x.LOAD_END))
    throw new Error("Not a load end");
  return { status: e[1], tuplesLoaded: e[2], tuplesTotal: e[3] };
}
function Ft(t) {
  const e = j(t);
  return !Array.isArray(e) || e[0] !== x.ERROR ? null : { statusCode: e[1], message: e[2] };
}
function kt(t, e = 0) {
  return k.encode([x.BLOCK_PUT_REQUEST, t, e]);
}
function Mt(t) {
  const e = j(t);
  if (e[0] !== x.BLOCK_PUT_RESPONSE) throw new Error("Not a block put response");
  return { status: e[1], hash: e[2] };
}
function Ht(t) {
  return k.encode([x.BLOCK_GET_REQUEST, t]);
}
function jt(t) {
  const e = j(t);
  if (e[0] !== x.BLOCK_GET_RESPONSE) throw new Error("Not a block get response");
  return { status: e[1], data: e[2] };
}
function qt(t) {
  return k.encode([x.BLOCK_DELETE_REQUEST, t]);
}
function $t(t) {
  const e = j(t);
  if (e[0] !== x.BLOCK_DELETE_RESPONSE) throw new Error("Not a block delete response");
  return { status: e[1] };
}
function Gt() {
  return k.encode([x.HEALTH_REQUEST]);
}
function Kt(t) {
  const e = j(t);
  if (e[0] !== x.HEALTH_RESPONSE) throw new Error("Not a health response");
  return { json: e[1] };
}
function vt(t = 0) {
  return t === 0 ? k.encode([x.PEER_INFO_REQUEST]) : k.encode([x.PEER_INFO_REQUEST, t]);
}
function Vt(t) {
  const e = j(t);
  if (e[0] !== x.PEER_INFO_RESPONSE) throw new Error("Not a peer info response");
  return { format: e[1], data: e[2] };
}
function Wt(t, e) {
  return k.encode([x.PEER_CONNECT, t, e]);
}
function zt(t) {
  const e = j(t);
  if (e[0] !== x.PEER_CONNECT_RESULT) throw new Error("Not a peer connect result");
  return { status: e[1] };
}
function Qt() {
  return k.encode([x.PEER_LIST_REQUEST]);
}
function Jt(t) {
  const e = j(t);
  if (e[0] !== x.PEER_LIST_RESPONSE) throw new Error("Not a peer list response");
  return e[1];
}
function Zt(t, e) {
  return k.encode([x.FRIEND_ADD, t, e]);
}
function Yt(t) {
  return k.encode([x.FRIEND_REMOVE, t]);
}
function Xt() {
  return k.encode([x.FRIEND_LIST]);
}
function er(t) {
  const e = j(t);
  if (e[0] !== x.FRIEND_LIST_RESPONSE) throw new Error("Not a friend list response");
  return e[1];
}
function tr() {
  return k.encode([x.CONFIG_SHOW_REQUEST]);
}
function rr(t) {
  const e = j(t);
  if (e[0] !== x.CONFIG_SHOW_RESPONSE) throw new Error("Not a config show response");
  return { json: e[1] };
}
function nr(t, e) {
  return k.encode([x.CONFIG_SET_REQUEST, t, e]);
}
function sr(t) {
  const e = j(t);
  if (e[0] !== x.CONFIG_SET_RESPONSE) throw new Error("Not a config set response");
  return { status: e[1], restartRequired: e[2] === 1, message: e[3] };
}
function ir() {
  return k.encode([x.CONFIG_RELOAD_REQUEST]);
}
function or(t) {
  const e = j(t);
  if (e[0] !== x.CONFIG_RELOAD_RESPONSE) throw new Error("Not a config reload response");
  return { status: e[1], message: e[2] };
}
const Qr = /* @__PURE__ */ Object.freeze(/* @__PURE__ */ Object.defineProperty({
  __proto__: null,
  LOAD_STATUS: Ur,
  MSG: x,
  PEER_CONTENT_TYPES: We,
  PEER_FORMATS: re,
  STATUS: Pr,
  decodeBlockDeleteResponse: $t,
  decodeBlockGetResponse: jt,
  decodeBlockPutResponse: Mt,
  decodeConfigReloadResponse: or,
  decodeConfigSetResponse: sr,
  decodeConfigShowResponse: rr,
  decodeError: Ft,
  decodeFriendListResponse: er,
  decodeGetData: Nt,
  decodeGetResponseStart: Pt,
  decodeHealthResponse: Kt,
  decodeLoadEnd: Dt,
  decodeLoadProgress: Lt,
  decodePeerConnectResult: zt,
  decodePeerInfoResponse: Vt,
  decodePeerListResponse: Jt,
  decodePutResponse: Qe,
  encodeAuthRequest: nt,
  encodeBlockDeleteRequest: qt,
  encodeBlockGetRequest: Ht,
  encodeBlockPutRequest: kt,
  encodeConfigReloadRequest: ir,
  encodeConfigSetRequest: nr,
  encodeConfigShowRequest: tr,
  encodeFriendAdd: Zt,
  encodeFriendListRequest: Xt,
  encodeFriendRemove: Yt,
  encodeGetRequest: Ut,
  encodeHealthRequest: Gt,
  encodeLoadRequest: Ct,
  encodePeerConnect: Wt,
  encodePeerInfoRequest: vt,
  encodePeerListRequest: Qt,
  encodePutData: At,
  encodePutEnd: Tt,
  encodePutRequest: ze,
  getMessageType: rt,
  isGetEnd: Bt,
  isLoadEnd: It
}, Symbol.toStringTag, { value: "Module" }));
class D {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {string} */
    L(this, "baseUrl");
    /** @type {string|undefined} */
    L(this, "apiKey");
    /** @type {AbortController|null} */
    L(this, "abortController", null);
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
    var h, y;
    const n = {
      ...this.authHeaders(),
      type: e.contentType,
      "file-name": e.fileName,
      "stream-length": String(e.streamLength)
    };
    e.serverAddress && (n["server-address"] = e.serverAddress), (h = e.recyclerUrls) != null && h.length && (n.recycler = JSON.stringify(e.recyclerUrls)), e.temporary && (n.temporary = "true"), e.tupleSize !== void 0 && (n["tuple-size"] = String(e.tupleSize));
    let s = r;
    r && typeof r.getReader == "function" && (s = await this._readStream(r));
    const o = await fetch(this.url("/offsystem"), {
      method: "PUT",
      headers: n,
      body: s,
      signal: (y = this.abortController) == null ? void 0 : y.signal
    });
    if (!o.ok) {
      const m = await o.text();
      throw new Error(`Upload failed: ${o.status} ${m}`);
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
      const { done: h, value: y } = await r.read();
      if (h) break;
      n.push(y), s += y.length;
    }
    const o = new Uint8Array(s);
    let l = 0;
    for (const h of n)
      o.set(h, l), l += h.length;
    return o;
  }
  /**
   * Download from GET /offsystem/v3/...
   * @param {string} offUrl
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   */
  async get(e, r) {
    var P, N, q, v, B, M, ne;
    const n = await fetch(e, {
      method: "GET",
      headers: this.authHeaders(),
      signal: (P = this.abortController) == null ? void 0 : P.signal
    });
    if (!n.ok) {
      const I = await n.text();
      (N = r.onError) == null || N.call(r, n.status, I);
      return;
    }
    const s = n.headers.get("content-type") || "application/octet-stream", o = parseInt(n.headers.get("content-length") || "0", 10), l = n.status === 206, h = n.headers.get("content-range");
    let y, m;
    if (h) {
      const I = h.match(/bytes (\d+)-(\d+)\//);
      I && (y = parseInt(I[1], 10), m = parseInt(I[2], 10));
    }
    (q = r.onStart) == null || q.call(r, s, o, l, y, m);
    const g = (v = n.body) == null ? void 0 : v.getReader();
    if (!g) {
      (B = r.onEnd) == null || B.call(r);
      return;
    }
    try {
      for (; ; ) {
        const { done: I, value: _ } = await g.read();
        if (I) break;
        _ && r.onData(_);
      }
      (M = r.onEnd) == null || M.call(r);
    } catch (I) {
      (ne = r.onError) == null || ne.call(r, 0, String(I));
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
    var m, g, P;
    const s = e.includes("?") ? "&" : "?", o = await fetch(`${e}${s}load=1`, {
      method: "GET",
      headers: n ? { ...this.authHeaders(), Range: `bytes=${n.start || 0}-${n.end || 0}` } : this.authHeaders(),
      signal: (m = this.abortController) == null ? void 0 : m.signal
    });
    if (!o.ok) {
      const N = await o.text();
      throw new Error(`Load failed: ${o.status} ${N}`);
    }
    const l = (g = o.body) == null ? void 0 : g.getReader();
    if (!l) return;
    const h = new TextDecoder();
    let y = "";
    try {
      for (; ; ) {
        const { done: q, value: v } = await l.read();
        if (q) break;
        y += h.decode(v, { stream: !0 });
        let B;
        for (; (B = y.indexOf(`
`)) !== -1; ) {
          const M = y.slice(0, B).trim();
          y = y.slice(B + 1), M && this._handleLoadLine(M, r);
        }
      }
      y += h.decode();
      const N = y.trim();
      N && this._handleLoadLine(N, r);
    } catch (N) {
      (P = r.onError) == null || P.call(r, 0, String(N));
    }
  }
  /**
   * Parse one ndjson progress/status line and dispatch to callbacks.
   * @param {string} line
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   */
  _handleLoadLine(e, r) {
    var s, o;
    let n;
    try {
      n = JSON.parse(e);
    } catch {
      throw new Error(`Bad ndjson line: ${e}`);
    }
    n.status !== void 0 ? (s = r.onEnd) == null || s.call(r, n.status, n.tuples_loaded || 0, n.tuples_total || 0) : (o = r.onProgress) == null || o.call(r, n.tuples_loaded || 0, n.tuples_total || 0);
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
    const r = re[e] ?? 0, n = await fetch(this.url(`/peer/info?format=${e}`), {
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
      headers: { ...this.authHeaders(), "Content-Type": We[r] ?? "application/cbor" },
      body: r === re.base58 ? new TextDecoder().decode(e) : e,
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
      body: r === re.base58 ? new TextDecoder().decode(e) : e,
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
class Nr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {WebSocket|null} */
    L(this, "socket", null);
    /** @type {string|undefined} */
    L(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    L(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    L(this, "openPromise", null);
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
        const o = s.message || ((l = s.error) == null ? void 0 : l.message) || "unknown";
        r(new Error(`WebSocket error: ${o}`));
      }, n.onclose = () => {
        this.socket = null, this.openPromise = null;
      }, n.onmessage = (s) => {
        var h;
        const o = new Uint8Array(s.data), l = rt(o);
        l !== null && ((h = this.messageHandler) == null || h.call(this, l, o));
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
class Br {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, r, n) {
    /** @type {WebTransport|null} */
    L(this, "transport", null);
    /** @type {WritableStreamWriter|null} */
    L(this, "writer", null);
    /** @type {ReadableStreamReader|null} */
    L(this, "reader", null);
    /** @type {string|undefined} */
    L(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    L(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    L(this, "openPromise", null);
    /** @type {boolean} */
    L(this, "running", !1);
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
        const o = s instanceof Uint8Array ? s : new Uint8Array(s.buffer, s.byteOffset, s.byteLength);
        for (e = e ? Cr(e, o) : o; e.length >= 4; ) {
          const h = new DataView(e.buffer, e.byteOffset, e.length).getUint32(0, !1);
          if (e.length < 4 + h) break;
          const y = e.subarray(4, 4 + h), m = rt(y);
          m !== null && ((r = this.messageHandler) == null || r.call(this, m, y)), e = e.subarray(4 + h);
        }
      }
    } catch {
    }
  }
}
function Cr(t, e) {
  const r = new Uint8Array(t.length + e.length);
  return r.set(t, 0), r.set(e, t.length), r;
}
const Je = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz", st = new Int8Array(128);
st.fill(-1);
for (let t = 0; t < Je.length; t++)
  st[Je.charCodeAt(t)] = t;
function ge(t) {
  if (t.length === 0) return null;
  let e = 0;
  for (; e < t.length && t[e] === "1"; )
    e++;
  const r = [];
  for (let n = e; n < t.length; n++) {
    const s = t.charCodeAt(n);
    if (s >= 128) return null;
    const o = st[s];
    if (o < 0) return null;
    let l = o;
    for (let h = 0; h < r.length; h++)
      l += r[h] * 58, r[h] = l & 255, l >>= 8;
    for (; l > 0; )
      r.push(l & 255), l >>= 8;
  }
  for (let n = 0; n < e; n++)
    r.push(0);
  return r.reverse(), new Uint8Array(r);
}
function ke(t) {
  if (t.length === 0) return "";
  const e = Array.from(t);
  let r = 0;
  for (; r < e.length && e[r] === 0; )
    r++;
  const n = [];
  for (let o = r; o < e.length; o++) {
    let l = e[o];
    for (let h = 0; h < n.length; h++)
      l += n[h] * 256, n[h] = l % 58, l = Math.floor(l / 58);
    for (; l > 0; )
      n.push(l % 58), l = Math.floor(l / 58);
  }
  return "1".repeat(r) + n.reverse().map((o) => Je[o]).join("");
}
function wt(t) {
  const e = t.indexOf("/offsystem/v3/");
  if (e < 0) return null;
  const n = t.slice(e + 14).split("/");
  if (n.length < 4) return null;
  const s = n[n.length - 4], o = n[n.length - 3], l = n[n.length - 2], h = n.slice(n.length - 1).join("/"), y = parseInt(s, 10);
  return !Number.isFinite(y) || ge(o) === null || ge(l) === null ? null : {
    fileHashB58: o,
    descriptorHashB58: l,
    streamLength: y,
    fileName: decodeURIComponent(h)
  };
}
function Lr(t) {
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
function Ir(t) {
  return typeof t.arrayBuffer == "function" ? t.arrayBuffer().then((e) => new Uint8Array(e)) : new Promise((e, r) => {
    const n = new FileReader();
    n.onload = () => e(new Uint8Array(n.result)), n.onerror = () => r(n.error), n.readAsArrayBuffer(t);
  });
}
function Te(t) {
  const r = t.replace(/\\\\/g, "/").split("/").filter(Boolean);
  return r.length > 0 ? r[r.length - 1] : "file";
}
function xt(t, e = 65536) {
  let r = 0;
  return new ReadableStream({
    pull(n) {
      if (r >= t.size) {
        n.close();
        return;
      }
      const s = Math.min(r + e, t.size), o = t.slice(r, s);
      return Ir(o).then((l) => {
        n.enqueue(l), r = s;
      });
    }
  });
}
function Dr(t) {
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
function Fr(t, e = "http://localhost:23402") {
  if (!t || /^https?:\/\//i.test(t)) return t;
  let r = t;
  r.startsWith("offs://") && (r = r.slice(7));
  const n = "/offsystem/v3/", s = r.indexOf(n);
  return s >= 0 && (r = r.slice(s)), r.startsWith(n) ? `${e.replace(/\/$/, "")}${r}` : t;
}
const kr = new et({ tagUint8Array: !1, useRecords: !1 });
new Ee({ useRecords: !1 });
const Mr = 128e3, Hr = 3;
function jr({
  name: t,
  fileHash: e,
  descriptorHash: r,
  finalByte: n,
  blockType: s = Mr,
  tupleSize: o = Hr,
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
function qr({ name: t, dirHash: e }) {
  return { name: t, isDirectory: !0, dirHash: e };
}
function $r(t) {
  const e = t.map((r) => {
    const n = {
      n: r.name,
      t: r.isDirectory ? 1 : 0
    };
    return r.isDirectory ? n.d = r.dirHash : (n.f = r.fileHash, n.D = r.descriptorHash, n.s = r.finalByte, n.B = r.blockType, n.T = r.tupleSize, n.o = r.fileOffset), n;
  });
  return kr.encode({ v: 1, entries: e });
}
function Gr() {
  return {
    connectTimeoutMs: 5e3,
    requestTimeoutMs: 3e4
  };
}
function Kr(t, e, r) {
  return t.startsWith("ws://") || t.startsWith("wss://") ? new Nr(t, e, r) : t.startsWith("wt://") || t.startsWith("wts://") ? new Br(t, e, r) : new D(t, e, r);
}
class Jr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {OffsClientConfig & {transport?: any}} [config]
   */
  constructor(e, r, n) {
    /** @type {string} */
    L(this, "url");
    /** @type {string|undefined} */
    L(this, "apiKey");
    /** @type {OffsClientConfig} */
    L(this, "config");
    /** @type {HttpTransport|WsTransport|WtTransport} */
    L(this, "transport");
    /** @type {Map<number, PendingRequest>} */
    L(this, "pending", /* @__PURE__ */ new Map());
    /** @type {{type: number, bytes: Uint8Array}[]} */
    L(this, "inboundQueue", []);
    /** @type {number} */
    L(this, "nextRequestId", 1);
    /** @type {boolean} */
    L(this, "streamingPut", !1);
    /** @type {OffsPutOptions|null} */
    L(this, "streamOptions", null);
    /** @type {boolean} */
    L(this, "connected", !1);
    this.url = e, this.apiKey = r, this.config = { ...Gr(), ...n }, this.transport = (n == null ? void 0 : n.transport) || Kr(e, r, n), this.transport.setMessageHandler(this._onMessage.bind(this));
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
    if (e === x.ERROR) {
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
    const n = {
      ...e,
      fileName: Te(e.fileName)
    };
    if (this.transport instanceof D) {
      const l = r || new Uint8Array(0);
      return this.transport.put(n, l);
    }
    const s = ze(n, r), o = await this._sendAndWait(s, x.PUT_RESPONSE);
    return Qe(o);
  }
  /**
   * @param {OffsPutOptions} options
   * @returns {Promise<void>}
   */
  async putStreamStart(e) {
    if (this.streamingPut = !0, this.streamOptions = e, this.transport instanceof D)
      return;
    const r = ze(e);
    await this.transport.send(r);
  }
  /**
   * @param {Uint8Array} chunk
   * @returns {Promise<void>}
   */
  async putStreamData(e) {
    if (this.transport instanceof D)
      throw new Error("HTTP transport does not support putStreamData; use put with ReadableStream");
    await this.transport.send(At(e));
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
    await this.transport.send(Tt());
    const r = await this._request(this.nextRequestId - 1, x.PUT_RESPONSE);
    return Qe(r);
  }
  /**
   * @param {string} oriString
   * @param {OffsGetCallbacks} callbacks
   * @param {{start?: number, end?: number}} [range]
   */
  async get(e, r, n) {
    var h, y;
    if (this.transport instanceof D)
      return this.transport.get(e, r);
    const s = Ut(e, n), o = await this._sendAndWait(s, x.GET_RESPONSE_START), l = Pt(o);
    for ((h = r.onStart) == null || h.call(r, l.contentType, l.contentLength, l.hasRange, l.rangeStart, l.rangeEnd); ; ) {
      const m = await this._waitForResponse([x.GET_DATA, x.GET_END]);
      if (Bt(m)) break;
      const g = Nt(m);
      r.onData(g);
    }
    (y = r.onEnd) == null || y.call(r);
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
    var h, y;
    if (this.transport instanceof D)
      return this.transport.load(e, r, n);
    const s = Ct(e, n);
    await this.transport.send(s);
    let o = null;
    for (; ; ) {
      const m = await this._waitForResponse([x.LOAD_PROGRESS, x.LOAD_END]);
      if (It(m)) {
        o = m;
        break;
      }
      const g = Lt(m);
      (h = r.onProgress) == null || h.call(r, g.tuplesLoaded, g.tuplesTotal);
    }
    const l = Dt(o);
    (y = r.onEnd) == null || y.call(r, l.status, l.tuplesLoaded, l.tuplesTotal);
  }
  /**
   * @param {Uint8Array} data
   * @param {number} [encoding=0]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(e, r = 0) {
    if (this.transport instanceof D)
      return this.transport.blockPut(e, r);
    const n = kt(e, r), s = await this._sendAndWait(n, x.BLOCK_PUT_RESPONSE);
    return Mt(s);
  }
  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number, data: Uint8Array}>}
   */
  async blockGet(e) {
    if (typeof e == "string") return this.transport.blockGet(e);
    if (this.transport instanceof D)
      return this.transport.blockGet(ke(e));
    const r = Ht(e), n = await this._sendAndWait(r, x.BLOCK_GET_RESPONSE);
    return jt(n);
  }
  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(e) {
    if (typeof e == "string") return this.transport.blockDelete(e);
    if (this.transport instanceof D)
      return this.transport.blockDelete(ke(e));
    const r = qt(e), n = await this._sendAndWait(r, x.BLOCK_DELETE_RESPONSE);
    return $t(n);
  }
  /**
   * @returns {Promise<any>}
   */
  async health() {
    if (this.transport instanceof D)
      return this.transport.health();
    const e = Gt(), r = await this._sendAndWait(e, x.HEALTH_RESPONSE), { json: n } = Kt(r);
    return JSON.parse(n);
  }
  /**
   * @param {string} [format='cbor']
   * @returns {Promise<{format: number, data: Uint8Array}>}
   */
  async peerInfo(e = "cbor") {
    if (this.transport instanceof D)
      return this.transport.peerInfo(e);
    const r = vt(re[e] ?? 0), n = await this._sendAndWait(r, x.PEER_INFO_RESPONSE);
    return Vt(n);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(e, r = 0) {
    if (this.transport instanceof D)
      return this.transport.peerConnect(e, r);
    const n = Wt(r, e), s = await this._sendAndWait(n, x.PEER_CONNECT_RESULT);
    return zt(s);
  }
  /**
   * Connect to a peer from a QR image (binary P6 PPM bytes).
   * @param {Uint8Array} ppmBytes
   * @returns {Promise<{status: number}>}
   */
  async peerConnectQr(e) {
    return this.peerConnect(e, re.qrcode);
  }
  /**
   * Add a friend from a QR image (binary P6 PPM bytes).
   * @param {Uint8Array} ppmBytes
   * @returns {Promise<void>}
   */
  async friendAddQr(e) {
    return this.friendAdd(e, re.qrcode);
  }
  /**
   * Convert an OFF URL/URI string into an HTTP URL usable by a browser.
   * @param {string} oriString
   * @param {string} [baseUrl]
   * @returns {string}
   */
  static offUrlToHttpUrl(e, r) {
    return Fr(e, r);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    if (this.transport instanceof D)
      return this.transport.peerList();
    const e = Qt(), r = await this._sendAndWait(e, x.PEER_LIST_RESPONSE);
    return Jt(r);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(e, r = 0) {
    if (this.transport instanceof D)
      return this.transport.friendAdd(e, r);
    const n = Zt(r, e);
    await this.transport.send(n);
  }
  /**
   * @param {string|Uint8Array} nodeId
   * @returns {Promise<void>}
   */
  async friendRemove(e) {
    if (this.transport instanceof D)
      return this.transport.friendRemove(typeof e == "string" ? e : ke(e));
    const r = typeof e == "string" ? new TextEncoder().encode(e) : e, n = Yt(r);
    await this.transport.send(n);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    if (this.transport instanceof D)
      return this.transport.friendList();
    const e = Xt(), r = await this._sendAndWait(e, x.FRIEND_LIST_RESPONSE);
    return er(r);
  }
  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    if (this.transport instanceof D)
      return this.transport.configShow();
    const e = tr(), r = await this._sendAndWait(e, x.CONFIG_SHOW_RESPONSE), { json: n } = rr(r);
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
    const n = nr(e, r), s = await this._sendAndWait(n, x.CONFIG_SET_RESPONSE);
    return sr(s);
  }
  /**
   * @returns {Promise<{status: number, message: string}>}
   */
  async configReload() {
    if (this.transport instanceof D)
      return this.transport.configReload();
    const e = ir(), r = await this._sendAndWait(e, x.CONFIG_RELOAD_RESPONSE);
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
    const n = Dr(e);
    if (n.length === 0)
      throw new Error("No files to upload");
    const s = r.recyclerUrls || [], o = n.length;
    let l = 0;
    const h = (g) => {
      var P;
      l++, (P = r.onProgress) == null || P.call(r, g, l, o);
    }, y = vr(n.map((g) => g.path)), m = async (g) => {
      const P = Te(g || y || "root"), q = Vr(n, g), v = Wr(n, g), B = [];
      for (const I of v) {
        const X = (await m(I)).oriString, J = wt(X);
        if (!J)
          throw new Error(`Failed to parse subdirectory URL: ${X}`);
        const V = ge(J.fileHashB58);
        if (!V)
          throw new Error(`Invalid directory hash in URL: ${X}`);
        B.push(qr({
          name: Te(I),
          dirHash: V
        }));
      }
      for (const I of q) {
        const _ = Te(I.path), X = Lr(_), J = I.file.size;
        let V;
        if (this.transport instanceof D) {
          const ie = xt(I.file);
          V = (await this.put({
            contentType: X,
            fileName: _,
            streamLength: J,
            serverAddress: r.serverAddress,
            recyclerUrls: s,
            temporary: r.temporary
          }, ie)).oriString;
        } else {
          await this.putStreamStart({
            contentType: X,
            fileName: _,
            streamLength: J,
            serverAddress: r.serverAddress,
            recyclerUrls: s,
            temporary: r.temporary
          });
          const ie = xt(I.file).getReader();
          for (; ; ) {
            const { done: be, value: a } = await ie.read();
            if (be) break;
            await this.putStreamData(a);
          }
          V = (await this.putStreamEnd()).oriString;
        }
        const fe = wt(V);
        if (!fe)
          throw new Error(`Failed to parse file URL: ${V}`);
        const se = ge(fe.fileHashB58), de = ge(fe.descriptorHashB58);
        if (!se || !de)
          throw new Error(`Invalid hash in file URL: ${V}`);
        B.push(jr({
          name: _,
          fileHash: se,
          descriptorHash: de,
          finalByte: fe.streamLength
        })), h(_);
      }
      if (B.length === 0)
        throw new Error(`Empty directory: ${g || y}`);
      const M = $r(B), ne = `${P}.ofd`;
      return this.transport instanceof D ? this.put({
        contentType: "offsystem/directory",
        fileName: ne,
        streamLength: M.length,
        serverAddress: r.serverAddress,
        recyclerUrls: s,
        temporary: r.temporary
      }, M) : (await this.putStreamStart({
        contentType: "offsystem/directory",
        fileName: ne,
        streamLength: M.length,
        serverAddress: r.serverAddress,
        recyclerUrls: s,
        temporary: r.temporary
      }), await this.putStreamData(M), this.putStreamEnd());
    };
    return m(y);
  }
}
function vr(t) {
  if (t.length === 0) return "";
  const e = t.map((o) => o.split("/").filter(Boolean)), r = e[0];
  let n = r.length;
  for (let o = 1; o < e.length; o++) {
    const l = e[o];
    let h = 0;
    for (; h < Math.min(n, l.length) && r[h] === l[h]; )
      h++;
    if (n = h, n === 0) break;
  }
  const s = Math.min(n, r.length - 1);
  return r.slice(0, s).join("/");
}
function Vr(t, e) {
  const r = e ? `${e}/` : "";
  return t.filter((n) => {
    if (!n.path.startsWith(r)) return !1;
    const s = n.path.slice(r.length);
    return s.length > 0 && !s.includes("/");
  });
}
function Wr(t, e) {
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
var Me = globalThis.OffsClient;
Me && Me.OffsClient && (globalThis.OffsClient = Me.OffsClient);
export {
  Jr as OffsClient,
  ge as base58Decode,
  ke as base58Encode,
  Lr as mimeFromExtension,
  Fr as offUrlToHttpUrl,
  wt as parseOffUrl,
  Qr as wire
};
//# sourceMappingURL=offs-client.esm.js.map
