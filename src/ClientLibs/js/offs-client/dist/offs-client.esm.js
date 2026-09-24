var Er = Object.defineProperty;
var yr = (r, e, t) => e in r ? Er(r, e, { enumerable: !0, configurable: !0, writable: !0, value: t }) : r[e] = t;
var D = (r, e, t) => yr(r, typeof e != "symbol" ? e + "" : e, t);
let He;
try {
  He = new TextDecoder();
} catch {
}
let w, ce, c = 0;
const wr = 105, gr = 57342, xr = 57343, ot = 57337, at = 6, he = {};
let ge = 11281e4, ae = 1681e4, A = {}, F, Ne, Be = 0, Se = 0, K, Z, j = [], je = [], z, V, xe, ft = {
  useRecords: !1,
  mapsAsObjects: !0
}, me = !1, xt = 2;
try {
  new Function("");
} catch {
  xt = 1 / 0;
}
class we {
  constructor(e) {
    if (e && ((e.keyMap || e._keyMap) && !e.useRecords && (e.useRecords = !1, e.mapsAsObjects = !0), e.useRecords === !1 && e.mapsAsObjects === void 0 && (e.mapsAsObjects = !0), e.getStructures && (e.getShared = e.getStructures), e.getShared && !e.structures && ((e.structures = []).uninitialized = !0), e.keyMap)) {
      this.mapKey = /* @__PURE__ */ new Map();
      for (let [t, n] of Object.entries(e.keyMap)) this.mapKey.set(n, t);
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
    let t = /* @__PURE__ */ new Map();
    for (let [n, s] of Object.entries(e)) t.set(this._keyMap.hasOwnProperty(n) ? this._keyMap[n] : n, s);
    return t;
  }
  decodeKeys(e) {
    if (!this._keyMap || e.constructor.name != "Map") return e;
    if (!this._mapKey) {
      this._mapKey = /* @__PURE__ */ new Map();
      for (let [n, s] of Object.entries(this._keyMap)) this._mapKey.set(s, n);
    }
    let t = {};
    return e.forEach((n, s) => t[Y(this._mapKey.has(s) ? this._mapKey.get(s) : s)] = n), t;
  }
  mapDecode(e, t) {
    let n = this.decode(e);
    if (this._keyMap)
      switch (n.constructor.name) {
        case "Array":
          return n.map((s) => this.decodeKeys(s));
      }
    return n;
  }
  decode(e, t) {
    if (w)
      return _t(() => (Ge(), this ? this.decode(e, t) : we.prototype.decode.call(ft, e, t)));
    ce = t > -1 ? t : e.length, c = 0, Se = 0, Ne = null, K = null, w = e;
    try {
      V = e.dataView || (e.dataView = new DataView(e.buffer, e.byteOffset, e.byteLength));
    } catch (n) {
      throw w = null, e instanceof Uint8Array ? n : new Error("Source must be a Uint8Array or Buffer but was a " + (e && typeof e == "object" ? e.constructor.name : typeof e));
    }
    if (this instanceof we) {
      if (A = this, z = this.sharedValues && (this.pack ? new Array(this.maxPrivatePackedValues || 16).concat(this.sharedValues) : this.sharedValues), this.structures)
        return F = this.structures, Te();
      (!F || F.length > 0) && (F = []);
    } else
      A = ft, (!F || F.length > 0) && (F = []), z = null;
    return Te();
  }
  decodeMultiple(e, t) {
    let n, s = 0;
    try {
      let i = e.length;
      me = !0;
      let l = this ? this.decode(e, i) : Xe.decode(e, i);
      if (t) {
        if (t(l) === !1)
          return;
        for (; c < i; )
          if (s = c, t(Te()) === !1)
            return;
      } else {
        for (n = [l]; c < i; )
          s = c, n.push(Te());
        return n;
      }
    } catch (i) {
      throw i.lastPosition = s, i.values = n, i;
    } finally {
      me = !1, Ge();
    }
  }
}
function Te() {
  try {
    let r = b();
    if (K) {
      if (c >= K.postBundlePosition) {
        let e = new Error("Unexpected bundle position");
        throw e.incomplete = !0, e;
      }
      c = K.postBundlePosition, K = null;
    }
    if (c == ce)
      F = null, w = null, Z && (Z = null);
    else if (c > ce) {
      let e = new Error("Unexpected end of CBOR data");
      throw e.incomplete = !0, e;
    } else if (!me)
      throw new Error("Data read, but end of buffer not reached");
    return r;
  } catch (r) {
    throw Ge(), (r instanceof RangeError || r.message.startsWith("Unexpected end of buffer")) && (r.incomplete = !0), r;
  }
}
function b() {
  let r = w[c++], e = r >> 5;
  if (r = r & 31, r > 23)
    switch (r) {
      case 24:
        r = w[c++];
        break;
      case 25:
        if (e == 7)
          return _r();
        r = V.getUint16(c), c += 2;
        break;
      case 26:
        if (e == 7) {
          let t = V.getFloat32(c);
          if (A.useFloat32 > 2) {
            let n = Ye[(w[c] & 127) << 1 | w[c + 1] >> 7];
            return c += 4, (n * t + (t > 0 ? 0.5 : -0.5) >> 0) / n;
          }
          return c += 4, t;
        }
        if (r = V.getUint32(c), c += 4, e === 1) return -1 - r;
        break;
      case 27:
        if (e == 7) {
          let t = V.getFloat64(c);
          return c += 8, t;
        }
        if (e > 1) {
          if (V.getUint32(c) > 0)
            throw new Error("JavaScript does not support arrays, maps, or strings with length over 4294967295");
          r = V.getUint32(c + 4);
        } else A.int64AsNumber ? (r = V.getUint32(c) * 4294967296, r += V.getUint32(c + 4)) : r = V.getBigUint64(c);
        c += 8;
        break;
      case 31:
        switch (e) {
          case 2:
          case 3:
            throw new Error("Indefinite length not supported for byte or text strings");
          case 4:
            let t = [], n, s = 0;
            for (; (n = b()) != he; ) {
              if (s >= ge) throw new Error(`Array length exceeds ${ge}`);
              t[s++] = n;
            }
            return e == 4 ? t : e == 3 ? t.join("") : Buffer.concat(t);
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
        throw new Error("Unknown token " + r);
    }
  switch (e) {
    case 0:
      return r;
    case 1:
      return ~r;
    case 2:
      return Rr(r);
    case 3:
      if (Se >= c)
        return Ne.slice(c - Be, (c += r) - Be);
      if (Se == 0 && ce < 140 && r < 32) {
        let s = r < 16 ? St(r) : mr(r);
        if (s != null)
          return s;
      }
      return Sr(r);
    case 4:
      if (r >= ge) throw new Error(`Array length exceeds ${ge}`);
      let t = new Array(r);
      for (let s = 0; s < r; s++) t[s] = b();
      return t;
    case 5:
      if (r >= ae) throw new Error(`Map size exceeds ${ge}`);
      if (A.mapsAsObjects) {
        let s = {};
        if (A.keyMap) for (let i = 0; i < r; i++) s[Y(A.decodeKey(b()))] = b();
        else for (let i = 0; i < r; i++) s[Y(b())] = b();
        return s;
      } else {
        xe && (A.mapsAsObjects = !0, xe = !1);
        let s = /* @__PURE__ */ new Map();
        if (A.keyMap) for (let i = 0; i < r; i++) s.set(A.decodeKey(b()), b());
        else for (let i = 0; i < r; i++) s.set(b(), b());
        return s;
      }
    case 6:
      if (r >= ot) {
        let s = F[r & 8191];
        if (s)
          return s.read || (s.read = qe(s)), s.read();
        if (r < 65536) {
          if (r == xr) {
            let i = Ee(), l = b(), h = b();
            Ke(l, h);
            let E = {};
            if (A.keyMap) for (let S = 2; S < i; S++) {
              let x = A.decodeKey(h[S - 2]);
              E[Y(x)] = b();
            }
            else for (let S = 2; S < i; S++) {
              let x = h[S - 2];
              E[Y(x)] = b();
            }
            return E;
          } else if (r == gr) {
            let i = Ee(), l = b();
            for (let h = 2; h < i; h++)
              Ke(l++, b());
            return b();
          } else if (r == ot)
            return Ur();
          if (A.getShared && (Ze(), s = F[r & 8191], s))
            return s.read || (s.read = qe(s)), s.read();
        }
      }
      let n = j[r];
      if (n)
        return n.handlesRead ? n(b) : n(b());
      {
        let s = b();
        for (let i = 0; i < je.length; i++) {
          let l = je[i](r, s);
          if (l !== void 0)
            return l;
        }
        return new ue(s, r);
      }
    case 7:
      switch (r) {
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
          let s = (z || le())[r];
          if (s !== void 0)
            return s;
          throw new Error("Unknown token " + r);
      }
    default:
      if (isNaN(r)) {
        let s = new Error("Unexpected end of CBOR data");
        throw s.incomplete = !0, s;
      }
      throw new Error("Unknown CBOR token " + r);
  }
}
const lt = /^[a-zA-Z_$][a-zA-Z\d_$]*$/;
function qe(r) {
  if (!r) throw new Error("Structure is required in record definition");
  function e() {
    let t = w[c++];
    if (t = t & 31, t > 23)
      switch (t) {
        case 24:
          t = w[c++];
          break;
        case 25:
          t = V.getUint16(c), c += 2;
          break;
        case 26:
          t = V.getUint32(c), c += 4;
          break;
        default:
          throw new Error("Expected array header, but got " + w[c - 1]);
      }
    let n = this.compiledReader;
    for (; n; ) {
      if (n.propertyCount === t)
        return n(b);
      n = n.next;
    }
    if (this.slowReads++ >= xt) {
      let i = this.length == t ? this : this.slice(0, t);
      return n = A.keyMap ? new Function("r", "return {" + i.map((l) => A.decodeKey(l)).map((l) => lt.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}") : new Function("r", "return {" + i.map((l) => lt.test(l) ? Y(l) + ":r()" : "[" + JSON.stringify(l) + "]:r()").join(",") + "}"), this.compiledReader && (n.next = this.compiledReader), n.propertyCount = t, this.compiledReader = n, n(b);
    }
    let s = {};
    if (A.keyMap) for (let i = 0; i < t; i++) s[Y(A.decodeKey(this[i]))] = b();
    else for (let i = 0; i < t; i++)
      s[Y(this[i])] = b();
    return s;
  }
  return r.slowReads = 0, e;
}
function Y(r) {
  if (typeof r == "string") return r === "__proto__" ? "__proto_" : r;
  if (typeof r == "number" || typeof r == "boolean" || typeof r == "bigint") return r.toString();
  if (r == null) return r + "";
  throw new Error("Invalid property name type " + typeof r);
}
let Sr = $e;
function $e(r) {
  let e;
  if (r < 16 && (e = St(r)))
    return e;
  if (r > 64 && He)
    return He.decode(w.subarray(c, c += r));
  const t = c + r, n = [];
  for (e = ""; c < t; ) {
    const s = w[c++];
    if (!(s & 128))
      n.push(s);
    else if ((s & 224) === 192)
      if (s < 194 || c >= t || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const i = w[c++] & 63;
        n.push((s & 31) << 6 | i);
      }
    else if ((s & 240) === 224) {
      const i = c < t ? w[c] : 0;
      if (c >= t || (i & 192) !== 128 || s === 224 && i < 160 || s === 237 && i >= 160)
        n.push(65533);
      else if (c++, c >= t || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = w[c++] & 63;
        n.push((s & 31) << 12 | (i & 63) << 6 | l);
      }
    } else if ((s & 248) === 240) {
      const i = c < t ? w[c] : 0;
      if (s > 244 || c >= t || (i & 192) !== 128 || s === 240 && i < 144 || s === 244 && i >= 144)
        n.push(65533);
      else if (c++, c >= t || (w[c] & 192) !== 128)
        n.push(65533);
      else {
        const l = w[c++] & 63;
        if (c >= t || (w[c] & 192) !== 128)
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
function mr(r) {
  let e = c, t = new Array(r);
  for (let n = 0; n < r; n++) {
    const s = w[c++];
    if ((s & 128) > 0) {
      c = e;
      return;
    }
    t[n] = s;
  }
  return G.apply(String, t);
}
function St(r) {
  if (r < 4)
    if (r < 2) {
      if (r === 0)
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
      let e = w[c++], t = w[c++];
      if ((e & 128) > 0 || (t & 128) > 0) {
        c -= 2;
        return;
      }
      if (r < 3)
        return G(e, t);
      let n = w[c++];
      if ((n & 128) > 0) {
        c -= 3;
        return;
      }
      return G(e, t, n);
    }
  else {
    let e = w[c++], t = w[c++], n = w[c++], s = w[c++];
    if ((e & 128) > 0 || (t & 128) > 0 || (n & 128) > 0 || (s & 128) > 0) {
      c -= 4;
      return;
    }
    if (r < 6) {
      if (r === 4)
        return G(e, t, n, s);
      {
        let i = w[c++];
        if ((i & 128) > 0) {
          c -= 5;
          return;
        }
        return G(e, t, n, s, i);
      }
    } else if (r < 8) {
      let i = w[c++], l = w[c++];
      if ((i & 128) > 0 || (l & 128) > 0) {
        c -= 6;
        return;
      }
      if (r < 7)
        return G(e, t, n, s, i, l);
      let h = w[c++];
      if ((h & 128) > 0) {
        c -= 7;
        return;
      }
      return G(e, t, n, s, i, l, h);
    } else {
      let i = w[c++], l = w[c++], h = w[c++], E = w[c++];
      if ((i & 128) > 0 || (l & 128) > 0 || (h & 128) > 0 || (E & 128) > 0) {
        c -= 8;
        return;
      }
      if (r < 10) {
        if (r === 8)
          return G(e, t, n, s, i, l, h, E);
        {
          let S = w[c++];
          if ((S & 128) > 0) {
            c -= 9;
            return;
          }
          return G(e, t, n, s, i, l, h, E, S);
        }
      } else if (r < 12) {
        let S = w[c++], x = w[c++];
        if ((S & 128) > 0 || (x & 128) > 0) {
          c -= 10;
          return;
        }
        if (r < 11)
          return G(e, t, n, s, i, l, h, E, S, x);
        let U = w[c++];
        if ((U & 128) > 0) {
          c -= 11;
          return;
        }
        return G(e, t, n, s, i, l, h, E, S, x, U);
      } else {
        let S = w[c++], x = w[c++], U = w[c++], L = w[c++];
        if ((S & 128) > 0 || (x & 128) > 0 || (U & 128) > 0 || (L & 128) > 0) {
          c -= 12;
          return;
        }
        if (r < 14) {
          if (r === 12)
            return G(e, t, n, s, i, l, h, E, S, x, U, L);
          {
            let q = w[c++];
            if ((q & 128) > 0) {
              c -= 13;
              return;
            }
            return G(e, t, n, s, i, l, h, E, S, x, U, L, q);
          }
        } else {
          let q = w[c++], v = w[c++];
          if ((q & 128) > 0 || (v & 128) > 0) {
            c -= 14;
            return;
          }
          if (r < 15)
            return G(e, t, n, s, i, l, h, E, S, x, U, L, q, v);
          let I = w[c++];
          if ((I & 128) > 0) {
            c -= 15;
            return;
          }
          return G(e, t, n, s, i, l, h, E, S, x, U, L, q, v, I);
        }
      }
    }
  }
}
function Rr(r) {
  return A.copyBuffers ? (
    // specifically use the copying slice (not the node one)
    Uint8Array.prototype.slice.call(w, c, c += r)
  ) : w.subarray(c, c += r);
}
let mt = new Float32Array(1), Ae = new Uint8Array(mt.buffer, 0, 4);
function _r() {
  let r = w[c++], e = w[c++], t = (r & 127) >> 2;
  if (t === 31)
    return e || r & 3 ? NaN : r & 128 ? -1 / 0 : 1 / 0;
  if (t === 0) {
    let n = ((r & 3) << 8 | e) / 16777216;
    return r & 128 ? -n : n;
  }
  return Ae[3] = r & 128 | // sign bit
  (t >> 1) + 56, Ae[2] = (r & 7) << 5 | // last exponent bit and first two mantissa bits
  e >> 3, Ae[1] = e << 5, Ae[0] = 0, mt[0];
}
new Array(4096);
class ue {
  constructor(e, t) {
    this.value = e, this.tag = t;
  }
}
j[0] = (r) => new Date(r);
j[1] = (r) => new Date(Math.round(r * 1e3));
j[2] = (r) => {
  let e = BigInt(0);
  for (let t = 0, n = r.byteLength; t < n; t++)
    e = BigInt(r[t]) + (e << BigInt(8));
  return e;
};
j[3] = (r) => BigInt(-1) - j[2](r);
j[4] = (r) => +(r[1] + "e" + r[0]);
j[5] = (r) => r[1] * Math.exp(r[0] * Math.log(2));
const Ke = (r, e) => {
  r = r - 57344;
  let t = F[r];
  t && t.isShared && ((F.restoreStructures || (F.restoreStructures = []))[r] = t), F[r] = e, e.read = qe(e);
};
j[wr] = (r) => {
  let e = r.length, t = r[1];
  Ke(r[0], t);
  let n = {};
  for (let s = 2; s < e; s++) {
    let i = t[s - 2];
    n[Y(i)] = r[s];
  }
  return n;
};
j[14] = (r) => K ? K[0].slice(K.position0, K.position0 += r) : new ue(r, 14);
j[15] = (r) => K ? K[1].slice(K.position1, K.position1 += r) : new ue(r, 15);
let Or = { Error, RegExp };
j[27] = (r) => (Or[r[0]] || Error)(r[1], r[2]);
const Rt = (r) => {
  if (w[c++] != 132) {
    let t = new Error("Packed values structure must be followed by a 4 element array");
    throw w.length < c && (t.incomplete = !0), t;
  }
  let e = r();
  if (!e || !e.length) {
    let t = new Error("Packed values structure must be followed by a 4 element array");
    throw t.incomplete = !0, t;
  }
  return z = z ? e.concat(z.slice(e.length)) : e, z.prefixes = r(), z.suffixes = r(), r();
};
Rt.handlesRead = !0;
j[51] = Rt;
j[at] = (r) => {
  if (!z)
    if (A.getShared)
      Ze();
    else
      return new ue(r, at);
  if (typeof r == "number")
    return z[16 + (r >= 0 ? 2 * r : -2 * r - 1)];
  let e = new Error("No support for non-integer packed references yet");
  throw r === void 0 && (e.incomplete = !0), e;
};
j[28] = (r) => {
  Z || (Z = /* @__PURE__ */ new Map(), Z.id = 0);
  let e = Z.id++, t = c, n = w[c], s;
  n >> 5 == 4 ? s = [] : s = {};
  let i = { target: s };
  Z.set(e, i);
  let l = r();
  return i.used ? (Object.getPrototypeOf(s) !== Object.getPrototypeOf(l) && (c = t, s = l, Z.set(e, { target: s }), l = r()), Object.assign(s, l)) : (i.target = l, l);
};
j[28].handlesRead = !0;
j[29] = (r) => {
  let e = Z.get(r);
  return e.used = !0, e.target;
};
j[258] = (r) => new Set(r);
(j[259] = (r) => (A.mapsAsObjects && (A.mapsAsObjects = !1, xe = !0), r())).handlesRead = !0;
function pe(r, e) {
  return typeof r == "string" ? r + e : r instanceof Array ? r.concat(e) : Object.assign({}, r, e);
}
function le() {
  if (!z)
    if (A.getShared)
      Ze();
    else
      throw new Error("No packed values available");
  return z;
}
const Tr = 1399353956;
je.push((r, e) => {
  if (r >= 225 && r <= 255)
    return pe(le().prefixes[r - 224], e);
  if (r >= 28704 && r <= 32767)
    return pe(le().prefixes[r - 28672], e);
  if (r >= 1879052288 && r <= 2147483647)
    return pe(le().prefixes[r - 1879048192], e);
  if (r >= 216 && r <= 223)
    return pe(e, le().suffixes[r - 216]);
  if (r >= 27647 && r <= 28671)
    return pe(e, le().suffixes[r - 27639]);
  if (r >= 1811940352 && r <= 1879048191)
    return pe(e, le().suffixes[r - 1811939328]);
  if (r == Tr)
    return {
      packedValues: z,
      structures: F.slice(0),
      version: e
    };
  if (r == 55799)
    return e;
});
const Ar = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1, ct = [
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
], br = [64, 68, 69, 70, 71, 72, 77, 78, 79, 85, 86];
for (let r = 0; r < ct.length; r++)
  Pr(ct[r], br[r]);
function Pr(r, e) {
  let t = "get" + r.name.slice(0, -5), n;
  typeof r == "function" ? n = r.BYTES_PER_ELEMENT : r = null;
  for (let s = 0; s < 2; s++) {
    if (!s && n == 1)
      continue;
    let i = n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : 0;
    j[s ? e : e - 4] = n == 1 || s == Ar ? (l) => {
      if (!r)
        throw new Error("Could not find typed array for code " + e);
      return !A.copyBuffers && (n === 1 || n === 2 && !(l.byteOffset & 1) || n === 4 && !(l.byteOffset & 3) || n === 8 && !(l.byteOffset & 7)) ? new r(l.buffer, l.byteOffset, l.byteLength >> i) : new r(Uint8Array.prototype.slice.call(l, 0).buffer);
    } : (l) => {
      if (!r)
        throw new Error("Could not find typed array for code " + e);
      let h = new DataView(l.buffer, l.byteOffset, l.byteLength), E = l.length >> i, S = new r(E), x = h[t];
      for (let U = 0; U < E; U++)
        S[U] = x.call(h, U << i, s);
      return S;
    };
  }
}
function Ur() {
  let r = Ee(), e = c + b();
  for (let n = 2; n < r; n++) {
    let s = Ee();
    c += s;
  }
  let t = c;
  return c = e, K = [$e(Ee()), $e(Ee())], K.position0 = 0, K.position1 = 0, K.postBundlePosition = c, c = t, b();
}
function Ee() {
  let r = w[c++] & 31;
  if (r > 23)
    switch (r) {
      case 24:
        r = w[c++];
        break;
      case 25:
        r = V.getUint16(c), c += 2;
        break;
      case 26:
        r = V.getUint32(c), c += 4;
        break;
    }
  return r;
}
function Ze() {
  if (A.getShared) {
    let r = _t(() => (w = null, A.getShared())) || {}, e = r.structures || [];
    A.sharedVersion = r.version, z = A.sharedValues = r.packedValues, F === !0 ? A.structures = F = e : F.splice.apply(F, [0, e.length].concat(e));
  }
}
function _t(r) {
  let e = ce, t = c, n = Be, s = Se, i = Ne, l = Z, h = K, E = new Uint8Array(w.slice(0, ce)), S = F, x = A, U = me, L = r();
  return ce = e, c = t, Be = n, Se = s, Ne = i, Z = l, K = h, w = E, me = U, F = S, A = x, V = new DataView(w.buffer, w.byteOffset, w.byteLength), L;
}
function Ge() {
  w = null, Z = null, F = null;
}
const Ye = new Array(147);
for (let r = 0; r < 256; r++)
  Ye[r] = +("1e" + Math.floor(45.15 - r * 0.30103));
let Xe = new we({ useRecords: !1 });
const k = Xe.decode;
Xe.decodeMultiple;
let Pe;
try {
  Pe = new TextEncoder();
} catch {
}
let ve, Ot;
const Le = typeof globalThis == "object" && globalThis.Buffer, Re = typeof Le < "u", Ie = Re ? Le.allocUnsafeSlow : Uint8Array, ut = Re ? Le : Uint8Array, dt = 256, ht = Re ? 4294967296 : 2144337920;
let Ce, f, C, o = 0, fe, $ = null;
const Nr = 61440, Br = /[\u0080-\uFFFF]/, J = Symbol("record-id");
class et extends we {
  constructor(e) {
    super(e), this.offset = 0;
    let t, n, s, i, l;
    e = e || {};
    let h = ut.prototype.utf8Write ? function(a, g) {
      return f.utf8Write(a, g, f.byteLength - g);
    } : Pe && Pe.encodeInto ? function(a, g) {
      return Pe.encodeInto(a, f.subarray(g)).written;
    } : !1, E = this, S = e.structures || e.saveStructures, x = e.maxSharedStructures;
    if (x == null && (x = S ? 128 : 0), x > 8190)
      throw new Error("Maximum maxSharedStructure is 8190");
    let U = e.sequential;
    U && (x = 0), this.structures || (this.structures = []), this.saveStructures && (this.saveShared = this.saveStructures);
    let L, q, v = e.sharedValues, I;
    if (v) {
      I = /* @__PURE__ */ Object.create(null);
      for (let a = 0, g = v.length; a < g; a++)
        I[v[a]] = a;
    }
    let H = [], se = 0, M = 0;
    this.mapEncode = function(a, g) {
      if (this._keyMap && !this._mapped)
        switch (a.constructor.name) {
          case "Array":
            a = a.map((d) => this.encodeKeys(d));
            break;
        }
      return this.encode(a, g);
    }, this.encode = function(a, g) {
      if (f || (f = new Ie(8192), C = new DataView(f.buffer, 0, 8192), o = 0), fe = f.length - 10, fe - o < 2048 ? (f = new Ie(f.length), C = new DataView(f.buffer, 0, f.length), fe = f.length - 10, o = 0) : g === yt && (o = o + 7 & 2147483640), t = o, E.useSelfDescribedHeader && (C.setUint32(o, 3654940416), o += 3), l = E.structuredClone ? /* @__PURE__ */ new Map() : null, E.bundleStrings && typeof a != "string" ? ($ = [], $.size = 1 / 0) : $ = null, n = E.structures, n) {
        if (n.uninitialized) {
          let p = E.getShared() || {};
          E.structures = n = p.structures || [], E.sharedVersion = p.version;
          let u = E.sharedValues = p.packedValues;
          if (u) {
            I = {};
            for (let m = 0, _ = u.length; m < _; m++)
              I[u[m]] = m;
          }
        }
        let d = n.length;
        if (d > x && !U && (d = x), !n.transitions) {
          n.transitions = /* @__PURE__ */ Object.create(null);
          for (let p = 0; p < d; p++) {
            let u = n[p];
            if (!u)
              continue;
            let m, _ = n.transitions;
            for (let O = 0, T = u.length; O < T; O++) {
              _[J] === void 0 && (_[J] = p);
              let P = u[O];
              m = _[P], m || (m = _[P] = /* @__PURE__ */ Object.create(null)), _ = m;
            }
            _[J] = p | 1048576;
          }
        }
        U || (n.nextId = d);
      }
      if (s && (s = !1), i = n || [], q = I, e.pack) {
        let d = /* @__PURE__ */ new Map();
        if (d.values = [], d.encoder = E, d.maxValues = e.maxPrivatePackedValues || (I ? 16 : 1 / 0), d.objectMap = I || !1, d.samplingPackedValues = L, Ue(a, d), d.values.length > 0) {
          f[o++] = 216, f[o++] = 51, re(4);
          let p = d.values;
          R(p), re(0), re(0), q = Object.create(I || null);
          for (let u = 0, m = p.length; u < m; u++)
            q[p[u]] = u;
        }
      }
      Ce = g & ke;
      try {
        if (Ce)
          return;
        if (R(a), $ && Et(t, R), E.offset = o, l && l.idsToInsert) {
          o += l.idsToInsert.length * 2, o > fe && W(o), E.offset = o;
          let d = Cr(f.subarray(t, o), l.idsToInsert);
          return l = null, d;
        }
        return g & yt ? (f.start = t, f.end = o, f) : f.subarray(t, o);
      } finally {
        if (n) {
          if (M < 10 && M++, n.length > x && (n.length = x), se > 1e4)
            n.transitions = null, M = 0, se = 0, H.length > 0 && (H = []);
          else if (H.length > 0 && !U) {
            for (let d = 0, p = H.length; d < p; d++)
              H[d][J] = void 0;
            H = [];
          }
        }
        if (s && E.saveShared) {
          E.structures.length > x && (E.structures = E.structures.slice(0, x));
          let d = f.subarray(t, o);
          return E.updateSharedData() === !1 ? E.encode(a) : d;
        }
        g & Dr && (o = t);
      }
    }, this.findCommonStringsToPack = () => (L = /* @__PURE__ */ new Map(), I || (I = /* @__PURE__ */ Object.create(null)), (a) => {
      let g = a && a.threshold || 4, d = this.pack ? a.maxPrivatePackedValues || 16 : 0;
      v || (v = this.sharedValues = []);
      for (let [p, u] of L)
        u.count > g && (I[p] = d++, v.push(p), s = !0);
      for (; this.saveShared && this.updateSharedData() === !1; )
        ;
      L = null;
    });
    const R = (a) => {
      o > fe && (f = W(o));
      var g = typeof a, d;
      if (g === "string") {
        if (q) {
          let _ = q[a];
          if (_ >= 0) {
            _ < 16 ? f[o++] = _ + 224 : (f[o++] = 198, _ & 1 ? R(15 - _ >> 1) : R(_ - 16 >> 1));
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
          if (($.size += p) > Nr) {
            let O, T = ($[0] ? $[0].length * 3 + $[1].length : 0) + 10;
            o + T > fe && (f = W(o + T)), f[o++] = 217, f[o++] = 223, f[o++] = 249, f[o++] = $.position ? 132 : 130, f[o++] = 26, O = o - t, o += 4, $.position && Et(t, R), $ = ["", ""], $.size = 0, $.position = O;
          }
          let _ = Br.test(a);
          $[_ ? 0 : 1] += a, f[o++] = _ ? 206 : 207, R(p);
          return;
        }
        let u;
        p < 32 ? u = 1 : p < 256 ? u = 2 : p < 65536 ? u = 3 : u = 5;
        let m = p * 3;
        if (o + m > fe && (f = W(o + m)), p < 64 || !h) {
          let _, O, T, P = o + u;
          for (_ = 0; _ < p; _++)
            O = a.charCodeAt(_), O < 128 ? f[P++] = O : O < 2048 ? (f[P++] = O >> 6 | 192, f[P++] = O & 63 | 128) : (O & 64512) === 55296 && ((T = a.charCodeAt(_ + 1)) & 64512) === 56320 ? (O = 65536 + ((O & 1023) << 10) + (T & 1023), _++, f[P++] = O >> 18 | 240, f[P++] = O >> 12 & 63 | 128, f[P++] = O >> 6 & 63 | 128, f[P++] = O & 63 | 128) : (f[P++] = O >> 12 | 224, f[P++] = O >> 6 & 63 | 128, f[P++] = O & 63 | 128);
          d = P - o - u;
        } else
          d = h(a, o + u, m);
        d < 24 ? f[o++] = 96 | d : d < 256 ? (u < 2 && f.copyWithin(o + 2, o + 1, o + 1 + d), f[o++] = 120, f[o++] = d) : d < 65536 ? (u < 3 && f.copyWithin(o + 3, o + 2, o + 2 + d), f[o++] = 121, f[o++] = d >> 8, f[o++] = d & 255) : (u < 5 && f.copyWithin(o + 5, o + 3, o + 3 + d), f[o++] = 122, C.setUint32(o, d), o += 4), o += d;
      } else if (g === "number")
        if (!this.alwaysUseFloat && a >>> 0 === a)
          a < 24 ? f[o++] = a : a < 256 ? (f[o++] = 24, f[o++] = a) : a < 65536 ? (f[o++] = 25, f[o++] = a >> 8, f[o++] = a & 255) : (f[o++] = 26, C.setUint32(o, a), o += 4);
        else if (!this.alwaysUseFloat && a >> 0 === a)
          a >= -24 ? f[o++] = 31 - a : a >= -256 ? (f[o++] = 56, f[o++] = ~a) : a >= -65536 ? (f[o++] = 57, C.setUint16(o, ~a), o += 2) : (f[o++] = 58, C.setUint32(o, ~a), o += 4);
        else if (!this.alwaysUseFloat && a < 0 && a >= -4294967296 && Math.floor(a) === a)
          f[o++] = 58, C.setUint32(o, -1 - a), o += 4;
        else {
          let p;
          if ((p = this.useFloat32) > 0 && a < 4294967296 && a >= -2147483648) {
            f[o++] = 250, C.setFloat32(o, a);
            let u;
            if (p < 4 || // this checks for rounding of numbers that were encoded in 32-bit float to nearest significant decimal digit that could be preserved
            (u = a * Ye[(f[o] & 127) << 1 | f[o + 1] >> 7]) >> 0 === u) {
              o += 4;
              return;
            } else
              o--;
          }
          f[o++] = 251, C.setFloat64(o, a), o += 8;
        }
      else if (g === "object")
        if (!a)
          f[o++] = 246;
        else {
          if (l) {
            let u = l.get(a);
            if (u) {
              if (f[o++] = 216, f[o++] = 29, f[o++] = 25, !u.references) {
                let m = l.idsToInsert || (l.idsToInsert = []);
                u.references = [], m.push(u);
              }
              u.references.push(o - t), o += 2;
              return;
            } else
              l.set(a, { offset: o - t });
          }
          let p = a.constructor;
          if (p === Object)
            this.skipFunction === !0 && (a = Object.fromEntries([...Object.keys(a).filter((u) => typeof a[u] != "function").map((u) => [u, a[u]])])), X(a);
          else if (p === Array) {
            d = a.length, d < 24 ? f[o++] = 128 | d : re(d);
            for (let u = 0; u < d; u++)
              R(a[u]);
          } else if (p === Map)
            if ((this.mapsAsObjects ? this.useTag259ForMaps !== !1 : this.useTag259ForMaps) && (f[o++] = 217, f[o++] = 1, f[o++] = 3), d = a.size, d < 24 ? f[o++] = 160 | d : d < 256 ? (f[o++] = 184, f[o++] = d) : d < 65536 ? (f[o++] = 185, f[o++] = d >> 8, f[o++] = d & 255) : (f[o++] = 186, C.setUint32(o, d), o += 4), E.keyMap)
              for (let [u, m] of a)
                R(E.encodeKey(u)), R(m);
            else
              for (let [u, m] of a)
                R(u), R(m);
          else {
            for (let u = 0, m = ve.length; u < m; u++) {
              let _ = Ot[u];
              if (a instanceof _) {
                let O = ve[u], T = O.tag;
                T == null && (T = O.getTag && O.getTag.call(this, a)), T < 24 ? f[o++] = 192 | T : T < 256 ? (f[o++] = 216, f[o++] = T) : T < 65536 ? (f[o++] = 217, f[o++] = T >> 8, f[o++] = T & 255) : T > -1 && (f[o++] = 218, C.setUint32(o, T), o += 4), O.encode.call(this, a, R, W);
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
                R(u);
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
                return R(u);
            }
            X(a);
          }
        }
      else if (g === "boolean")
        f[o++] = a ? 245 : 244;
      else if (g === "bigint") {
        if (a < BigInt(1) << BigInt(64) && a >= 0)
          f[o++] = 27, C.setBigUint64(o, a);
        else if (a > -(BigInt(1) << BigInt(64)) && a < 0)
          f[o++] = 59, C.setBigUint64(o, -a - BigInt(1));
        else if (this.largeBigIntToFloat)
          f[o++] = 251, C.setFloat64(o, Number(a));
        else {
          a >= BigInt(0) ? f[o++] = 194 : (f[o++] = 195, a = BigInt(-1) - a);
          let p = [];
          for (; a; )
            p.push(Number(a & BigInt(255))), a >>= BigInt(8);
          Qe(new Uint8Array(p.reverse()), W);
          return;
        }
        o += 8;
      } else if (g === "undefined")
        f[o++] = 247;
      else
        throw new Error("Unknown type: " + g);
    }, X = this.useRecords === !1 ? this.variableMapSize ? (a) => {
      let g = Object.keys(a), d = Object.values(a), p = g.length;
      if (p < 24 ? f[o++] = 160 | p : p < 256 ? (f[o++] = 184, f[o++] = p) : p < 65536 ? (f[o++] = 185, f[o++] = p >> 8, f[o++] = p & 255) : (f[o++] = 186, C.setUint32(o, p), o += 4), E.keyMap)
        for (let u = 0; u < p; u++)
          R(E.encodeKey(g[u])), R(d[u]);
      else
        for (let u = 0; u < p; u++)
          R(g[u]), R(d[u]);
    } : (a) => {
      f[o++] = 185;
      let g = o - t;
      o += 2;
      let d = 0;
      if (E.keyMap)
        for (let p in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(p)) && (R(E.encodeKey(p)), R(a[p]), d++);
      else
        for (let p in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(p)) && (R(p), R(a[p]), d++);
      f[g++ + t] = d >> 8, f[g + t] = d & 255;
    } : (a, g) => {
      let d, p = i.transitions || (i.transitions = /* @__PURE__ */ Object.create(null)), u = 0, m = 0, _, O;
      if (this.keyMap) {
        O = Object.keys(a).map((P) => this.encodeKey(P)), m = O.length;
        for (let P = 0; P < m; P++) {
          let it = O[P];
          d = p[it], d || (d = p[it] = /* @__PURE__ */ Object.create(null), u++), p = d;
        }
      } else
        for (let P in a) (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(P)) && (d = p[P], d || (p[J] & 1048576 && (_ = p[J] & 65535), d = p[P] = /* @__PURE__ */ Object.create(null), u++), p = d, m++);
      let T = p[J];
      if (T !== void 0)
        T &= 65535, f[o++] = 217, f[o++] = T >> 8 | 224, f[o++] = T & 255;
      else if (O || (O = p.__keys__ || (p.__keys__ = Object.keys(a))), _ === void 0 ? (T = i.nextId++, T || (T = 0, i.nextId = 1), T >= dt && (i.nextId = (T = x) + 1)) : T = _, i[T] = O, T < x) {
        f[o++] = 217, f[o++] = T >> 8 | 224, f[o++] = T & 255, p = i.transitions;
        for (let P = 0; P < m; P++)
          (p[J] === void 0 || p[J] & 1048576) && (p[J] = T), p = p[O[P]];
        p[J] = T | 1048576, s = !0;
      } else {
        if (p[J] = T, C.setUint32(o, 3655335680), o += 3, u && (se += M * u), H.length >= dt - x && (H.shift()[J] = void 0), H.push(p), re(m + 2), R(57344 + T), R(O), g) return;
        for (let P in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(P)) && R(a[P]);
        return;
      }
      if (m < 24 ? f[o++] = 128 | m : re(m), !g)
        for (let P in a)
          (typeof a.hasOwnProperty != "function" || a.hasOwnProperty(P)) && R(a[P]);
    }, W = (a) => {
      let g;
      if (a > 16777216) {
        if (a - t > ht)
          throw new Error("Encoded buffer would be larger than maximum buffer size");
        g = Math.min(
          ht,
          Math.round(Math.max((a - t) * (a > 67108864 ? 1.25 : 2), 4194304) / 4096) * 4096
        );
      } else
        g = (Math.max(a - t << 2, f.length - 1) >> 12) + 1 << 12;
      let d = new Ie(g);
      return C = new DataView(d.buffer, 0, g), f.copy ? f.copy(d, 0, t, a) : d.set(f.slice(t, a)), o -= t, t = 0, fe = d.length - 10, f = d;
    };
    let Q = 100, ee = 1e3;
    this.encodeAsIterable = function(a, g) {
      return _e(a, g, ie);
    }, this.encodeAsAsyncIterable = function(a, g) {
      return _e(a, g, Oe);
    };
    function* ie(a, g, d) {
      let p = a.constructor;
      if (p === Object) {
        let u = E.useRecords !== !1;
        u ? X(a, !0) : pt(Object.keys(a).length, 160);
        for (let m in a) {
          let _ = a[m];
          u || R(m), _ && typeof _ == "object" ? g[m] ? yield* ie(_, g[m]) : yield* de(_, g, m) : R(_);
        }
      } else if (p === Array) {
        let u = a.length;
        re(u);
        for (let m = 0; m < u; m++) {
          let _ = a[m];
          _ && (typeof _ == "object" || o - t > Q) ? g.element ? yield* ie(_, g.element) : yield* de(_, g, "element") : R(_);
        }
      } else if (a[Symbol.iterator] && !a.buffer) {
        f[o++] = 159;
        for (let u of a)
          u && (typeof u == "object" || o - t > Q) ? g.element ? yield* ie(u, g.element) : yield* de(u, g, "element") : R(u);
        f[o++] = 255;
      } else De(a) ? (pt(a.size, 64), yield f.subarray(t, o), yield a, oe()) : a[Symbol.asyncIterator] ? (f[o++] = 159, yield f.subarray(t, o), yield a, oe(), f[o++] = 255) : R(a);
      d && o > t ? yield f.subarray(t, o) : o - t > Q && (yield f.subarray(t, o), oe());
    }
    function* de(a, g, d) {
      let p = o - t;
      try {
        R(a), o - t > Q && (yield f.subarray(t, o), oe());
      } catch (u) {
        if (u.iteratorNotHandled)
          g[d] = {}, o = t + p, yield* ie.call(this, a, g[d]);
        else throw u;
      }
    }
    function oe() {
      Q = ee, E.encode(null, ke);
    }
    function _e(a, g, d) {
      return g && g.chunkThreshold ? Q = ee = g.chunkThreshold : Q = 100, a && typeof a == "object" ? (E.encode(null, ke), d(a, E.iterateProperties || (E.iterateProperties = {}), !0)) : [E.encode(a)];
    }
    async function* Oe(a, g) {
      for (let d of ie(a, g, !0)) {
        let p = d.constructor;
        if (p === ut || p === Uint8Array)
          yield d;
        else if (De(d)) {
          let u = d.stream().getReader(), m;
          for (; !(m = await u.read()).done; )
            yield m.value;
        } else if (d[Symbol.asyncIterator])
          for await (let u of d)
            oe(), u ? yield* Oe(u, g.async || (g.async = {})) : yield E.encode(u);
        else
          yield d;
      }
    }
  }
  useBuffer(e) {
    f = e, C = new DataView(f.buffer, f.byteOffset, f.byteLength), o = 0;
  }
  clearSharedData() {
    this.structures && (this.structures = []), this.sharedValues && (this.sharedValues = void 0);
  }
  updateSharedData() {
    let e = this.sharedVersion || 0;
    this.sharedVersion = e + 1;
    let t = this.structures.slice(0), n = new Tt(t, this.sharedValues, this.sharedVersion), s = this.saveShared(
      n,
      (i) => (i && i.version || 0) == e
    );
    return s === !1 ? (n = this.getShared() || {}, this.structures = n.structures || [], this.sharedValues = n.packedValues, this.sharedVersion = n.version, this.structures.nextId = this.structures.length) : t.forEach((i, l) => this.structures[l] = i), s;
  }
}
function pt(r, e) {
  r < 24 ? f[o++] = e | r : r < 256 ? (f[o++] = e | 24, f[o++] = r) : r < 65536 ? (f[o++] = e | 25, f[o++] = r >> 8, f[o++] = r & 255) : (f[o++] = e | 26, C.setUint32(o, r), o += 4);
}
class Tt {
  constructor(e, t, n) {
    this.structures = e, this.packedValues = t, this.version = n;
  }
}
function re(r) {
  r < 24 ? f[o++] = 128 | r : r < 256 ? (f[o++] = 152, f[o++] = r) : r < 65536 ? (f[o++] = 153, f[o++] = r >> 8, f[o++] = r & 255) : (f[o++] = 154, C.setUint32(o, r), o += 4);
}
const Lr = typeof Blob > "u" ? function() {
} : Blob;
function De(r) {
  if (r instanceof Lr)
    return !0;
  let e = r[Symbol.toStringTag];
  return e === "Blob" || e === "File";
}
function Ue(r, e) {
  switch (typeof r) {
    case "string":
      if (r.length > 3) {
        if (e.objectMap[r] > -1 || e.values.length >= e.maxValues)
          return;
        let n = e.get(r);
        if (n)
          ++n.count == 2 && e.values.push(r);
        else if (e.set(r, {
          count: 1
        }), e.samplingPackedValues) {
          let s = e.samplingPackedValues.get(r);
          s ? s.count++ : e.samplingPackedValues.set(r, {
            count: 1
          });
        }
      }
      break;
    case "object":
      if (r)
        if (r instanceof Array)
          for (let n = 0, s = r.length; n < s; n++)
            Ue(r[n], e);
        else {
          let n = !e.encoder.useRecords;
          for (var t in r)
            r.hasOwnProperty(t) && (n && Ue(t, e), Ue(r[t], e));
        }
      break;
    case "function":
      console.log(r);
  }
}
const Ir = new Uint8Array(new Uint16Array([1]).buffer)[0] == 1;
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
    encode(r, e) {
      let t = r.getTime() / 1e3;
      (this.useTimestamp32 || r.getMilliseconds() === 0) && t >= 0 && t < 4294967296 ? (f[o++] = 26, C.setUint32(o, t), o += 4) : (f[o++] = 251, C.setFloat64(o, t), o += 8);
    }
  },
  {
    // Set
    tag: 258,
    // https://github.com/input-output-hk/cbor-sets-spec/blob/master/CBOR_SETS.md
    encode(r, e) {
      let t = Array.from(r);
      e(t);
    }
  },
  {
    // Error
    tag: 27,
    // http://cbor.schmorp.de/generic-object
    encode(r, e) {
      e([r.name, r.message]);
    }
  },
  {
    // RegExp
    tag: 27,
    // http://cbor.schmorp.de/generic-object
    encode(r, e) {
      e(["RegExp", r.source, r.flags]);
    }
  },
  {
    // Tag
    getTag(r) {
      return r.tag;
    },
    encode(r, e) {
      e(r.value);
    }
  },
  {
    // ArrayBuffer
    encode(r, e, t) {
      Qe(r, t);
    }
  },
  {
    // Uint8Array
    getTag(r) {
      if (r.constructor === Uint8Array && (this.tagUint8Array || Re && this.tagUint8Array !== !1))
        return 64;
    },
    encode(r, e, t) {
      Qe(r, t);
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
    encode(r, e) {
      let t = r.packedValues || [], n = r.structures || [];
      if (t.values.length > 0) {
        f[o++] = 216, f[o++] = 51, re(4);
        let s = t.values;
        e(s), re(0), re(0), packedObjectMap = Object.create(sharedPackedObjectMap || null);
        for (let i = 0, l = s.length; i < l; i++)
          packedObjectMap[s[i]] = i;
      }
      if (n) {
        C.setUint32(o, 3655335424), o += 3;
        let s = n.slice(0);
        s.unshift(57344), s.push(new ue(r.version, 1399353956)), e(s);
      } else
        e(new ue(r.version, 1399353956));
    }
  }
];
function te(r, e) {
  return !Ir && e > 1 && (r -= 4), {
    tag: r,
    encode: function(n, s) {
      let i = n.byteLength, l = n.byteOffset || 0, h = n.buffer || n;
      s(Re ? Le.from(h, l, i) : new Uint8Array(h, l, i));
    }
  };
}
function Qe(r, e) {
  let t = r.byteLength;
  t < 24 ? f[o++] = 64 + t : t < 256 ? (f[o++] = 88, f[o++] = t) : t < 65536 ? (f[o++] = 89, f[o++] = t >> 8, f[o++] = t & 255) : (f[o++] = 90, C.setUint32(o, t), o += 4), o + t >= f.length && e(o + t), f.set(r.buffer ? r : new Uint8Array(r), o), o += t;
}
function Cr(r, e) {
  let t, n = e.length * 2, s = r.length - n;
  e.sort((i, l) => i.offset > l.offset ? 1 : -1);
  for (let i = 0; i < e.length; i++) {
    let l = e[i];
    l.id = i;
    for (let h of l.references)
      r[h++] = i >> 8, r[h] = i & 255;
  }
  for (; t = e.pop(); ) {
    let i = t.offset;
    r.copyWithin(i + n, i, s), n -= 2;
    let l = i + n;
    r[l++] = 216, r[l++] = 28, s = i;
  }
  return r;
}
function Et(r, e) {
  C.setUint32($.position + r, o - $.position - r + 1);
  let t = $;
  $ = null, e(t[0]), e(t[1]);
}
let tt = new et({ useRecords: !1 });
tt.encode;
tt.encodeAsIterable;
tt.encodeAsAsyncIterable;
const yt = 512, Dr = 1024, ke = 2048, B = new et({ tagUint8Array: !1 }), y = {
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
  EPHEMERAL_LIST_RESPONSE: 51,
  BOOTSTRAP_ADD: 52,
  BOOTSTRAP_REMOVE: 53,
  BOOTSTRAP_LIST: 54,
  BOOTSTRAP_LIST_RESPONSE: 55
}, kr = {
  loaded: 0,
  partial: 1,
  failed: 2
}, ne = { cbor: 0, base58: 1, qrcode: 2 }, Ve = {
  [ne.cbor]: "application/cbor",
  [ne.base58]: "text/plain",
  [ne.qrcode]: "image/x-portable-pixmap"
}, Mr = {
  OK: 0,
  BAD_REQUEST: 1,
  NOT_FOUND: 2,
  INTERNAL_ERROR: 3,
  RANGE_NOT_SATISFIABLE: 4,
  UNAUTHORIZED: 5,
  CONFLICT: 6
};
function rt(r) {
  const e = k(r);
  return Array.isArray(e) ? e[0] : null;
}
function nt(r) {
  const e = new TextEncoder().encode(r);
  return B.encode([y.AUTH_REQUEST, e]);
}
function We(r, e = null) {
  const t = r.recyclerUrls || [], n = [
    y.PUT_REQUEST,
    r.contentType,
    r.fileName,
    r.streamLength,
    r.serverAddress || null,
    e || new Uint8Array(0),
    t,
    r.temporary ? 1 : 0
  ];
  return r.tupleSize !== void 0 && n.push(r.tupleSize), r.recycleEphemeral !== void 0 && r.recycleEphemeral !== 0 && (r.tupleSize === void 0 && n.push(null), n.push(r.recycleEphemeral)), B.encode(n);
}
function At(r) {
  return B.encode([y.PUT_DATA, r]);
}
function bt() {
  return B.encode([y.PUT_END]);
}
function ze(r) {
  const e = k(r);
  if (e[0] !== y.PUT_RESPONSE) throw new Error("Not a put response");
  return { oriString: e[1] };
}
function Pt(r, e) {
  const t = e && (e.start !== void 0 || e.end !== void 0), n = [y.GET_REQUEST, r, t ? 1 : 0];
  return t && (n.push(e.start || 0), n.push(e.end || 0)), B.encode(n);
}
function Ut(r) {
  const e = k(r);
  if (e[0] !== y.GET_RESPONSE_START) throw new Error("Not a get response start");
  return {
    contentType: e[1],
    contentLength: e[2],
    hasRange: e[3] === 1,
    rangeStart: e[3] ? e[4] : void 0,
    rangeEnd: e[3] ? e[5] : void 0
  };
}
function Nt(r) {
  const e = k(r);
  if (e[0] !== y.GET_DATA) throw new Error("Not a get data");
  return e[1];
}
function Bt(r) {
  const e = k(r);
  return Array.isArray(e) && e[0] === y.GET_END;
}
function Lt(r, e) {
  return e && (e.start !== void 0 || e.end !== void 0) ? B.encode([
    y.LOAD_REQUEST,
    r,
    1,
    e.start || 0,
    e.end || 0
  ]) : B.encode([y.LOAD_REQUEST, r]);
}
function It(r) {
  const e = k(r);
  if (!(Array.isArray(e) && e[0] === y.LOAD_PROGRESS))
    throw new Error("Not a load progress");
  return { tuplesLoaded: e[1], tuplesTotal: e[2] };
}
function Ct(r) {
  const e = k(r);
  return Array.isArray(e) && e[0] === y.LOAD_END;
}
function Dt(r) {
  const e = k(r);
  if (!(Array.isArray(e) && e[0] === y.LOAD_END))
    throw new Error("Not a load end");
  return { status: e[1], tuplesLoaded: e[2], tuplesTotal: e[3] };
}
function kt(r) {
  const e = k(r);
  return !Array.isArray(e) || e[0] !== y.ERROR ? null : { statusCode: e[1], message: e[2] };
}
function Mt(r, e = 0) {
  return B.encode([y.BLOCK_PUT_REQUEST, r, e]);
}
function Ft(r) {
  const e = k(r);
  if (e[0] !== y.BLOCK_PUT_RESPONSE) throw new Error("Not a block put response");
  return { status: e[1], hash: e[2] };
}
function Ht(r) {
  return B.encode([y.BLOCK_GET_REQUEST, r]);
}
function jt(r) {
  const e = k(r);
  if (e[0] !== y.BLOCK_GET_RESPONSE) throw new Error("Not a block get response");
  return { status: e[1], data: e[2] };
}
function qt(r, e = 0) {
  return e ? B.encode([y.BLOCK_DELETE_REQUEST, r, 1]) : B.encode([y.BLOCK_DELETE_REQUEST, r]);
}
function $t(r) {
  const e = k(r);
  if (e[0] !== y.BLOCK_DELETE_RESPONSE) throw new Error("Not a block delete response");
  return { status: e[1] };
}
function Kt() {
  return B.encode([y.HEALTH_REQUEST]);
}
function Gt(r) {
  const e = k(r);
  if (e[0] !== y.HEALTH_RESPONSE) throw new Error("Not a health response");
  return { json: e[1] };
}
function vt(r = 0) {
  return r === 0 ? B.encode([y.PEER_INFO_REQUEST]) : B.encode([y.PEER_INFO_REQUEST, r]);
}
function Qt(r) {
  const e = k(r);
  if (e[0] !== y.PEER_INFO_RESPONSE) throw new Error("Not a peer info response");
  return { format: e[1], data: e[2] };
}
function Vt(r, e) {
  return B.encode([y.PEER_CONNECT, r, e]);
}
function Wt(r) {
  const e = k(r);
  if (e[0] !== y.PEER_CONNECT_RESULT) throw new Error("Not a peer connect result");
  return { status: e[1] };
}
function zt() {
  return B.encode([y.PEER_LIST_REQUEST]);
}
function Jt(r) {
  const e = k(r);
  if (e[0] !== y.PEER_LIST_RESPONSE) throw new Error("Not a peer list response");
  return e[1];
}
function Zt(r, e) {
  return B.encode([y.FRIEND_ADD, r, e]);
}
function Yt(r) {
  return B.encode([y.FRIEND_REMOVE, r]);
}
function Xt() {
  return B.encode([y.FRIEND_LIST]);
}
function er(r) {
  const e = k(r);
  if (e[0] !== y.FRIEND_LIST_RESPONSE) throw new Error("Not a friend list response");
  return e[1];
}
function tr(r) {
  return B.encode([y.BOOTSTRAP_ADD, r]);
}
function rr(r) {
  return B.encode([y.BOOTSTRAP_REMOVE, r]);
}
function nr() {
  return B.encode([y.BOOTSTRAP_LIST]);
}
function sr(r) {
  const e = k(r);
  if (e[0] !== y.BOOTSTRAP_LIST_RESPONSE) throw new Error("Not a bootstrap list response");
  return e[1];
}
function ir() {
  return B.encode([y.CONFIG_SHOW_REQUEST]);
}
function or(r) {
  const e = k(r);
  if (e[0] !== y.CONFIG_SHOW_RESPONSE) throw new Error("Not a config show response");
  return { json: e[1] };
}
function ar(r, e) {
  return B.encode([y.CONFIG_SET_REQUEST, r, e]);
}
function fr(r) {
  const e = k(r);
  if (e[0] !== y.CONFIG_SET_RESPONSE) throw new Error("Not a config set response");
  return { status: e[1], restartRequired: e[2] === 1, message: e[3] };
}
function lr() {
  return B.encode([y.CONFIG_RELOAD_REQUEST]);
}
function cr(r) {
  const e = k(r);
  if (e[0] !== y.CONFIG_RELOAD_RESPONSE) throw new Error("Not a config reload response");
  return { status: e[1], message: e[2] };
}
function ur(r, e) {
  return B.encode([r, e]);
}
function dr(r) {
  const e = k(r);
  return { status: e[1], blocks: e[2] };
}
function hr() {
  return B.encode([y.EPHEMERAL_LIST_REQUEST]);
}
function pr(r) {
  const e = k(r), t = (e[2] || []).map((n) => ({
    hash: Array.from(n[0], (s) => s.toString(16).padStart(2, "0")).join(""),
    claims: n[1],
    pins: n[2]
  }));
  return { status: e[1], entries: t };
}
const nn = /* @__PURE__ */ Object.freeze(/* @__PURE__ */ Object.defineProperty({
  __proto__: null,
  LOAD_STATUS: kr,
  MSG: y,
  PEER_CONTENT_TYPES: Ve,
  PEER_FORMATS: ne,
  STATUS: Mr,
  decodeBlockDeleteResponse: $t,
  decodeBlockGetResponse: jt,
  decodeBlockPutResponse: Ft,
  decodeBootstrapListResponse: sr,
  decodeConfigReloadResponse: cr,
  decodeConfigSetResponse: fr,
  decodeConfigShowResponse: or,
  decodeEphemeralListResponse: pr,
  decodeError: kt,
  decodeFriendListResponse: er,
  decodeGetData: Nt,
  decodeGetResponseStart: Ut,
  decodeHealthResponse: Gt,
  decodeLoadEnd: Dt,
  decodeLoadProgress: It,
  decodePeerConnectResult: Wt,
  decodePeerInfoResponse: Qt,
  decodePeerListResponse: Jt,
  decodePutResponse: ze,
  decodeRepResponse: dr,
  encodeAuthRequest: nt,
  encodeBlockDeleteRequest: qt,
  encodeBlockGetRequest: Ht,
  encodeBlockPutRequest: Mt,
  encodeBootstrapAdd: tr,
  encodeBootstrapListRequest: nr,
  encodeBootstrapRemove: rr,
  encodeConfigReloadRequest: lr,
  encodeConfigSetRequest: ar,
  encodeConfigShowRequest: ir,
  encodeEphemeralListRequest: hr,
  encodeFriendAdd: Zt,
  encodeFriendListRequest: Xt,
  encodeFriendRemove: Yt,
  encodeGetRequest: Pt,
  encodeHealthRequest: Kt,
  encodeLoadRequest: Lt,
  encodePeerConnect: Vt,
  encodePeerInfoRequest: vt,
  encodePeerListRequest: zt,
  encodePutData: At,
  encodePutEnd: bt,
  encodePutRequest: We,
  encodeRepRequest: ur,
  getMessageType: rt,
  isGetEnd: Bt,
  isLoadEnd: Ct
}, Symbol.toStringTag, { value: "Module" }));
class N {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, t, n) {
    /** @type {string} */
    D(this, "baseUrl");
    /** @type {string|undefined} */
    D(this, "apiKey");
    /** @type {AbortController|null} */
    D(this, "abortController", null);
    this.baseUrl = e.replace(/\/$/, ""), this.apiKey = t;
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
  async put(e, t) {
    var h, E;
    const n = {
      ...this.authHeaders(),
      type: e.contentType,
      "file-name": e.fileName,
      "stream-length": String(e.streamLength)
    };
    e.serverAddress && (n["server-address"] = e.serverAddress), (h = e.recyclerUrls) != null && h.length && (n.recycler = JSON.stringify(e.recyclerUrls)), e.temporary && (n.temporary = "true"), e.tupleSize !== void 0 && (n["tuple-size"] = String(e.tupleSize)), e.recycleEphemeral === 1 && (n["recycle-ephemeral"] = "commit"), e.recycleEphemeral === 2 && (n["recycle-ephemeral"] = "propagate");
    let s = t;
    t && typeof t.getReader == "function" && (s = await this._readStream(t));
    const i = await fetch(this.url("/offsystem"), {
      method: "PUT",
      headers: n,
      body: s,
      signal: (E = this.abortController) == null ? void 0 : E.signal
    });
    if (!i.ok) {
      const S = await i.text();
      throw new Error(`Upload failed: ${i.status} ${S}`);
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
    const t = e.getReader(), n = [];
    let s = 0;
    for (; ; ) {
      const { done: h, value: E } = await t.read();
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
  async get(e, t) {
    var U, L, q, v, I, H, se;
    const n = await fetch(e, {
      method: "GET",
      headers: this.authHeaders(),
      signal: (U = this.abortController) == null ? void 0 : U.signal
    });
    if (!n.ok) {
      const M = await n.text();
      (L = t.onError) == null || L.call(t, n.status, M);
      return;
    }
    const s = n.headers.get("content-type") || "application/octet-stream", i = parseInt(n.headers.get("content-length") || "0", 10), l = n.status === 206, h = n.headers.get("content-range");
    let E, S;
    if (h) {
      const M = h.match(/bytes (\d+)-(\d+)\//);
      M && (E = parseInt(M[1], 10), S = parseInt(M[2], 10));
    }
    (q = t.onStart) == null || q.call(t, s, i, l, E, S);
    const x = (v = n.body) == null ? void 0 : v.getReader();
    if (!x) {
      (I = t.onEnd) == null || I.call(t);
      return;
    }
    try {
      for (; ; ) {
        const { done: M, value: R } = await x.read();
        if (M) break;
        R && t.onData(R);
      }
      (H = t.onEnd) == null || H.call(t);
    } catch (M) {
      (se = t.onError) == null || se.call(t, 0, String(M));
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
  async load(e, t = {}, n) {
    var S, x, U;
    const s = e.includes("?") ? "&" : "?", i = await fetch(`${e}${s}load=1`, {
      method: "GET",
      headers: n ? { ...this.authHeaders(), Range: `bytes=${n.start || 0}-${n.end || 0}` } : this.authHeaders(),
      signal: (S = this.abortController) == null ? void 0 : S.signal
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
        let I;
        for (; (I = E.indexOf(`
`)) !== -1; ) {
          const H = E.slice(0, I).trim();
          E = E.slice(I + 1), H && this._handleLoadLine(H, t);
        }
      }
      E += h.decode();
      const L = E.trim();
      L && this._handleLoadLine(L, t);
    } catch (L) {
      (U = t.onError) == null || U.call(t, 0, String(L));
    }
  }
  /**
   * Parse one ndjson progress/status line and dispatch to callbacks.
   * @param {string} line
   * @param {import('../types.js').OffsGetCallbacks} callbacks
   */
  _handleLoadLine(e, t) {
    var s, i;
    let n;
    try {
      n = JSON.parse(e);
    } catch {
      throw new Error(`Bad ndjson line: ${e}`);
    }
    n.status !== void 0 ? (s = t.onEnd) == null || s.call(t, n.status, n.tuples_loaded || 0, n.tuples_total || 0) : (i = t.onProgress) == null || i.call(t, n.tuples_loaded || 0, n.tuples_total || 0);
  }
  /**
   * Delete content.
   * @param {string} offUrl
   * @returns {Promise<void>}
   */
  async delete(e) {
    var n;
    const t = await fetch(e, {
      method: "DELETE",
      headers: this.authHeaders(),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    });
    if (!t.ok) {
      const s = await t.text();
      throw new Error(`Delete failed: ${t.status} ${s}`);
    }
  }
  /**
   * @param {Uint8Array} data
   * @param {number} [encoding]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(e, t = 0) {
    var l;
    const n = t === 1 ? "?encoding=base58" : "", s = await fetch(this.url(`/blocks${n}`), {
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
    const t = await fetch(this.url(`/blocks/${e}`), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (s = this.abortController) == null ? void 0 : s.signal
    });
    if (!t.ok)
      return { status: 2, data: new Uint8Array(0) };
    const n = await t.arrayBuffer();
    return { status: 0, data: new Uint8Array(n) };
  }
  /**
   * @param {string} base58Hash
   * @param {number} [force] 1 removes pinned / ephemeral-claimed blocks too
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(e, t = 0) {
    var i;
    const n = t ? "?force=1" : "", s = await fetch(this.url(`/blocks/${e}${n}`), {
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
    var t;
    const e = await fetch(this.url("/health"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (t = this.abortController) == null ? void 0 : t.signal
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
    const t = ne[e] ?? 0, n = await fetch(this.url(`/peer/info?format=${e}`), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (i = this.abortController) == null ? void 0 : i.signal
    });
    if (!n.ok) throw new Error(`Peer info failed: ${n.status}`);
    const s = await n.arrayBuffer();
    return { format: t, data: new Uint8Array(s) };
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(e, t = 0) {
    var s;
    const n = await fetch(this.url("/peer/connect"), {
      method: "POST",
      headers: { ...this.authHeaders(), "Content-Type": Ve[t] ?? "application/cbor" },
      body: t === ne.base58 ? new TextDecoder().decode(e) : e,
      signal: (s = this.abortController) == null ? void 0 : s.signal
    });
    if (!n.ok) throw new Error(`Peer connect failed: ${n.status}`);
    return { status: 0 };
  }
  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    var t;
    const e = await fetch(this.url("/peers"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (t = this.abortController) == null ? void 0 : t.signal
    });
    if (!e.ok) throw new Error(`Peer list failed: ${e.status}`);
    return e.json();
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(e, t = 0) {
    var s;
    const n = await fetch(this.url("/friends"), {
      method: "POST",
      headers: { ...this.authHeaders(), "Content-Type": Ve[t] ?? "application/cbor" },
      body: t === ne.base58 ? new TextDecoder().decode(e) : e,
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
    const t = await fetch(this.url(`/friends/${e}`), {
      method: "DELETE",
      headers: this.authHeaders(),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    });
    if (!t.ok) throw new Error(`Friend remove failed: ${t.status}`);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    var t;
    const e = await fetch(this.url("/friends"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (t = this.abortController) == null ? void 0 : t.signal
    });
    if (!e.ok) throw new Error(`Friend list failed: ${e.status}`);
    return e.json();
  }
  /**
   * Add a bootstrap endpoint via POST /bootstrap (JSON body {"endpoint"}).
   * @param {string} endpoint "host:port" or "[ipv6]:port"
   * @returns {Promise<void>}
   */
  async bootstrapAdd(e) {
    var n;
    const t = await fetch(this.url("/bootstrap"), {
      method: "POST",
      headers: { ...this.authHeaders(), "Content-Type": "application/json" },
      body: JSON.stringify({ endpoint: e }),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    });
    if (!t.ok) throw new Error(`Bootstrap add failed: ${t.status}`);
  }
  /**
   * Remove a bootstrap endpoint via DELETE /bootstrap (JSON body {"endpoint"}).
   * @param {string} endpoint
   * @returns {Promise<void>}
   */
  async bootstrapRemove(e) {
    var n;
    const t = await fetch(this.url("/bootstrap"), {
      method: "DELETE",
      headers: { ...this.authHeaders(), "Content-Type": "application/json" },
      body: JSON.stringify({ endpoint: e }),
      signal: (n = this.abortController) == null ? void 0 : n.signal
    });
    if (!t.ok) throw new Error(`Bootstrap remove failed: ${t.status}`);
  }
  /**
   * List bootstrap endpoints via GET /bootstrap, returning the daemon's
   * {config: [{host, port}...], managed: [...]} JSON shape.
   * @returns {Promise<any>}
   */
  async bootstrapList() {
    var t;
    const e = await fetch(this.url("/bootstrap"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (t = this.abortController) == null ? void 0 : t.signal
    });
    if (!e.ok) throw new Error(`Bootstrap list failed: ${e.status}`);
    return e.json();
  }
  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    var t;
    const e = await fetch(this.url("/config"), {
      method: "GET",
      headers: this.authHeaders(),
      signal: (t = this.abortController) == null ? void 0 : t.signal
    });
    if (!e.ok) throw new Error(`Config show failed: ${e.status}`);
    return e.json();
  }
  /**
   * @param {string} field
   * @param {string} value
   * @returns {Promise<{staged: any, rejected: any, restart_required: boolean}>}
   */
  async configSet(e, t) {
    var s;
    const n = await fetch(this.url("/config"), {
      method: "PUT",
      headers: { ...this.authHeaders(), "Content-Type": "application/json" },
      body: JSON.stringify({ [e]: t }),
      signal: (s = this.abortController) == null ? void 0 : s.signal
    });
    if (!n.ok) throw new Error(`Config set failed: ${n.status}`);
    return n.json();
  }
  /**
   * @returns {Promise<void>}
   */
  async configReload() {
    var t;
    const e = await fetch(this.url("/config/restart"), {
      method: "POST",
      headers: this.authHeaders(),
      signal: (t = this.abortController) == null ? void 0 : t.signal
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
  async _repOp(e, t) {
    var i;
    const n = await fetch(this.url(e), {
      method: "POST",
      headers: { ...this.authHeaders(), "Content-Type": "text/plain" },
      body: t,
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
class Fr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, t, n) {
    /** @type {WebSocket|null} */
    D(this, "socket", null);
    /** @type {string|undefined} */
    D(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    D(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    D(this, "openPromise", null);
    this.url = e, this.apiKey = t;
  }
  /**
   * @returns {Promise<void>}
   */
  connect() {
    return this.socket ? this.openPromise || Promise.resolve() : (this.socket = new WebSocket(this.url), this.socket.binaryType = "arraybuffer", this.openPromise = new Promise((e, t) => {
      const n = this.socket;
      if (!n) return t(new Error("Socket not created"));
      n.onopen = () => {
        this.apiKey && this.send(nt(this.apiKey)), e();
      }, n.onerror = (s) => {
        var l;
        const i = s.message || ((l = s.error) == null ? void 0 : l.message) || "unknown";
        t(new Error(`WebSocket error: ${i}`));
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
class Hr {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {any} [_options]
   */
  constructor(e, t, n) {
    /** @type {WebTransport|null} */
    D(this, "transport", null);
    /** @type {WritableStreamWriter|null} */
    D(this, "writer", null);
    /** @type {ReadableStreamReader|null} */
    D(this, "reader", null);
    /** @type {string|undefined} */
    D(this, "apiKey");
    /** @type {((type: number, bytes: Uint8Array) => void)|null} */
    D(this, "messageHandler", null);
    /** @type {Promise<void>|null} */
    D(this, "openPromise", null);
    /** @type {boolean} */
    D(this, "running", !1);
    this.url = e, this.apiKey = t;
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
    var e, t, n;
    this.running = !1, (e = this.writer) == null || e.releaseLock(), (t = this.reader) == null || t.releaseLock(), (n = this.transport) == null || n.close(), this.writer = null, this.reader = null, this.transport = null, this.openPromise = null;
  }
  isConnected() {
    return this.transport !== null && this.transport.state === "connected";
  }
  /**
   * @param {Uint8Array} bytes
   */
  async send(e) {
    if (!this.writer) throw new Error("WebTransport not connected");
    const t = new Uint8Array(4);
    new DataView(t.buffer).setUint32(0, e.length, !1), await this.writer.write(t), await this.writer.write(e);
  }
  /**
   * @param {(type: number, bytes: Uint8Array) => void} handler
   */
  setMessageHandler(e) {
    this.messageHandler = e;
  }
  async _readLoop() {
    var t;
    let e = null;
    try {
      for (; this.running; ) {
        const { done: n, value: s } = await this.reader.read();
        if (n) break;
        const i = s instanceof Uint8Array ? s : new Uint8Array(s.buffer, s.byteOffset, s.byteLength);
        for (e = e ? jr(e, i) : i; e.length >= 4; ) {
          const h = new DataView(e.buffer, e.byteOffset, e.length).getUint32(0, !1);
          if (e.length < 4 + h) break;
          const E = e.subarray(4, 4 + h), S = rt(E);
          S !== null && ((t = this.messageHandler) == null || t.call(this, S, E)), e = e.subarray(4 + h);
        }
      }
    } catch {
    }
  }
}
function jr(r, e) {
  const t = new Uint8Array(r.length + e.length);
  return t.set(r, 0), t.set(e, r.length), t;
}
const Je = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz", st = new Int8Array(128);
st.fill(-1);
for (let r = 0; r < Je.length; r++)
  st[Je.charCodeAt(r)] = r;
function ye(r) {
  if (r.length === 0) return null;
  let e = 0;
  for (; e < r.length && r[e] === "1"; )
    e++;
  const t = [];
  for (let n = e; n < r.length; n++) {
    const s = r.charCodeAt(n);
    if (s >= 128) return null;
    const i = st[s];
    if (i < 0) return null;
    let l = i;
    for (let h = 0; h < t.length; h++)
      l += t[h] * 58, t[h] = l & 255, l >>= 8;
    for (; l > 0; )
      t.push(l & 255), l >>= 8;
  }
  for (let n = 0; n < e; n++)
    t.push(0);
  return t.reverse(), new Uint8Array(t);
}
function Me(r) {
  if (r.length === 0) return "";
  const e = Array.from(r);
  let t = 0;
  for (; t < e.length && e[t] === 0; )
    t++;
  const n = [];
  for (let i = t; i < e.length; i++) {
    let l = e[i];
    for (let h = 0; h < n.length; h++)
      l += n[h] * 256, n[h] = l % 58, l = Math.floor(l / 58);
    for (; l > 0; )
      n.push(l % 58), l = Math.floor(l / 58);
  }
  return "1".repeat(t) + n.reverse().map((i) => Je[i]).join("");
}
function wt(r) {
  const e = r.indexOf("/offsystem/v3/");
  if (e < 0) return null;
  const n = r.slice(e + 14).split("/");
  if (n.length < 4) return null;
  const s = n[n.length - 4], i = n[n.length - 3], l = n[n.length - 2], h = n.slice(n.length - 1).join("/"), E = parseInt(s, 10);
  return !Number.isFinite(E) || ye(i) === null || ye(l) === null ? null : {
    fileHashB58: i,
    descriptorHashB58: l,
    streamLength: E,
    fileName: decodeURIComponent(h)
  };
}
function qr(r) {
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
  }, t = r.lastIndexOf(".");
  if (t < 0 || t === r.length - 1) return "application/octet-stream";
  const n = r.slice(t + 1).toLowerCase();
  return e[n] || "application/octet-stream";
}
function $r(r) {
  return typeof r.arrayBuffer == "function" ? r.arrayBuffer().then((e) => new Uint8Array(e)) : new Promise((e, t) => {
    const n = new FileReader();
    n.onload = () => e(new Uint8Array(n.result)), n.onerror = () => t(n.error), n.readAsArrayBuffer(r);
  });
}
function be(r) {
  const t = r.replace(/\\\\/g, "/").split("/").filter(Boolean);
  return t.length > 0 ? t[t.length - 1] : "file";
}
function gt(r, e = 65536) {
  let t = 0;
  return new ReadableStream({
    pull(n) {
      if (t >= r.size) {
        n.close();
        return;
      }
      const s = Math.min(t + e, r.size), i = r.slice(t, s);
      return $r(i).then((l) => {
        n.enqueue(l), t = s;
      });
    }
  });
}
function Kr(r) {
  if (typeof FileList < "u" && r instanceof FileList) {
    const e = [];
    for (let t = 0; t < r.length; t++) {
      const n = r[t];
      let s = n.webkitRelativePath || n.name;
      e.push({ path: s, file: n });
    }
    return e;
  }
  return Array.isArray(r) ? r.map((e) => e instanceof File || e instanceof Blob ? { path: e.webkitRelativePath || e.name, file: e } : { path: e.path, file: e.file }) : Object.entries(r).map(([e, t]) => ({ path: e, file: t }));
}
function Gr(r, e = "http://localhost:23402") {
  if (!r || /^https?:\/\//i.test(r)) return r;
  let t = r;
  t.startsWith("offs://") && (t = t.slice(7));
  const n = "/offsystem/v3/", s = t.indexOf(n);
  return s >= 0 && (t = t.slice(s)), t.startsWith(n) ? `${e.replace(/\/$/, "")}${t}` : r;
}
const vr = new et({ tagUint8Array: !1, useRecords: !1 });
new we({ useRecords: !1 });
const Qr = 128e3, Vr = 3;
function Wr({
  name: r,
  fileHash: e,
  descriptorHash: t,
  finalByte: n,
  blockType: s = Qr,
  tupleSize: i = Vr,
  fileOffset: l = 0
}) {
  return {
    name: r,
    isDirectory: !1,
    fileHash: e,
    descriptorHash: t,
    finalByte: n,
    blockType: s,
    tupleSize: i,
    fileOffset: l
  };
}
function zr({ name: r, dirHash: e, descriptorHash: t, dirSize: n }) {
  return { name: r, isDirectory: !0, dirHash: e, descriptorHash: t, dirSize: n };
}
function Jr(r) {
  const e = r.map((t) => {
    const n = {
      n: t.name,
      t: t.isDirectory ? 1 : 0
    };
    return t.isDirectory ? (n.d = t.dirHash, t.descriptorHash && (n.D = t.descriptorHash), t.dirSize !== void 0 && (n.s = t.dirSize)) : (n.f = t.fileHash, n.D = t.descriptorHash, n.s = t.finalByte, n.B = t.blockType, n.T = t.tupleSize, n.o = t.fileOffset), n;
  });
  return vr.encode({ v: 1, entries: e });
}
function Zr() {
  return {
    connectTimeoutMs: 5e3,
    requestTimeoutMs: 3e4
  };
}
function Yr(r, e, t) {
  return r.startsWith("ws://") || r.startsWith("wss://") ? new Fr(r, e, t) : r.startsWith("wt://") || r.startsWith("wts://") ? new Hr(r, e, t) : new N(r, e, t);
}
class sn {
  /**
   * @param {string} url
   * @param {string} [apiKey]
   * @param {OffsClientConfig & {transport?: any}} [config]
   */
  constructor(e, t, n) {
    /** @type {string} */
    D(this, "url");
    /** @type {string|undefined} */
    D(this, "apiKey");
    /** @type {OffsClientConfig} */
    D(this, "config");
    /** @type {HttpTransport|WsTransport|WtTransport} */
    D(this, "transport");
    /** @type {Map<number, PendingRequest>} */
    D(this, "pending", /* @__PURE__ */ new Map());
    /** @type {{type: number, bytes: Uint8Array}[]} */
    D(this, "inboundQueue", []);
    /** @type {number} */
    D(this, "nextRequestId", 1);
    /** @type {boolean} */
    D(this, "streamingPut", !1);
    /** @type {OffsPutOptions|null} */
    D(this, "streamOptions", null);
    /** @type {boolean} */
    D(this, "connected", !1);
    this.url = e, this.apiKey = t, this.config = { ...Zr(), ...n }, this.transport = (n == null ? void 0 : n.transport) || Yr(e, t, n), this.transport.setMessageHandler(this._onMessage.bind(this));
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
  _request(e, t, n) {
    return new Promise((s, i) => {
      const l = {
        id: e,
        type: t,
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
  _waitForResponse(e, t) {
    const n = this.nextRequestId++, s = this._request(n, e, t), i = this._dequeueMatching(e);
    return i !== null && this._resolve(n, i), s;
  }
  /**
   * @param {number} id
   * @param {any} value
   */
  _resolve(e, t) {
    const n = this.pending.get(e);
    n && (n.timer && clearTimeout(n.timer), this.pending.delete(e), n.resolve(t));
  }
  /**
   * @param {number} id
   * @param {any} reason
   */
  _reject(e, t) {
    const n = this.pending.get(e);
    n && (n.timer && clearTimeout(n.timer), this.pending.delete(e), n.reject(t));
  }
  /**
   * @param {number} type
   * @param {Uint8Array} bytes
   */
  _onMessage(e, t) {
    if (e === y.ERROR) {
      const n = kt(t);
      if (n)
        for (const s of this.pending.values())
          this._reject(s.id, new Error(`Server error ${n.statusCode}: ${n.message}`));
      return;
    }
    for (const n of this.pending.values())
      if (Array.isArray(n.type) ? n.type.includes(e) : n.type === e) {
        this._resolve(n.id, t);
        return;
      }
    this.inboundQueue.push({ type: e, bytes: t });
  }
  /**
   * @param {number|number[]} type
   * @returns {Uint8Array|null}
   */
  _dequeueMatching(e) {
    const t = Array.isArray(e) ? e : [e], n = this.inboundQueue.findIndex((i) => t.includes(i.type));
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
  async _sendAndWait(e, t, n) {
    const s = this.nextRequestId++, i = this._request(s, t, n);
    return await this.transport.send(e), i;
  }
  /**
   * @param {string|OffsPutOptions} options
   * @param {Uint8Array|undefined} data
   * @returns {Promise<{oriString: string}>}
   */
  async put(e, t) {
    if (typeof e == "string")
      throw new Error("Use object options (contentType, fileName, streamLength)");
    const n = {
      ...e,
      fileName: be(e.fileName)
    };
    if (this.transport instanceof N) {
      const l = t || new Uint8Array(0);
      return this.transport.put(n, l);
    }
    const s = We(n, t), i = await this._sendAndWait(s, y.PUT_RESPONSE);
    return ze(i);
  }
  /**
   * @param {OffsPutOptions} options
   * @returns {Promise<void>}
   */
  async putStreamStart(e) {
    if (this.streamingPut = !0, this.streamOptions = e, this.transport instanceof N)
      return;
    const t = We(e);
    await this.transport.send(t);
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
    const t = await this._request(this.nextRequestId - 1, y.PUT_RESPONSE);
    return ze(t);
  }
  /**
   * @param {string} oriString
   * @param {OffsGetCallbacks} callbacks
   * @param {{start?: number, end?: number}} [range]
   */
  async get(e, t, n) {
    var h, E;
    if (this.transport instanceof N)
      return this.transport.get(e, t);
    const s = Pt(e, n), i = await this._sendAndWait(s, y.GET_RESPONSE_START), l = Ut(i);
    for ((h = t.onStart) == null || h.call(t, l.contentType, l.contentLength, l.hasRange, l.rangeStart, l.rangeEnd); ; ) {
      const S = await this._waitForResponse([y.GET_DATA, y.GET_END]);
      if (Bt(S)) break;
      const x = Nt(S);
      t.onData(x);
    }
    (E = t.onEnd) == null || E.call(t);
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
  async load(e, t = {}, n) {
    var h, E;
    if (this.transport instanceof N)
      return this.transport.load(e, t, n);
    const s = Lt(e, n);
    await this.transport.send(s);
    let i = null;
    for (; ; ) {
      const S = await this._waitForResponse([y.LOAD_PROGRESS, y.LOAD_END]);
      if (Ct(S)) {
        i = S;
        break;
      }
      const x = It(S);
      (h = t.onProgress) == null || h.call(t, x.tuplesLoaded, x.tuplesTotal);
    }
    const l = Dt(i);
    (E = t.onEnd) == null || E.call(t, l.status, l.tuplesLoaded, l.tuplesTotal);
  }
  /**
   * @param {Uint8Array} data
   * @param {number} [encoding=0]
   * @returns {Promise<{status: number, hash: Uint8Array|string}>}
   */
  async blockPut(e, t = 0) {
    if (this.transport instanceof N)
      return this.transport.blockPut(e, t);
    const n = Mt(e, t), s = await this._sendAndWait(n, y.BLOCK_PUT_RESPONSE);
    return Ft(s);
  }
  /**
   * @param {string|Uint8Array} hash
   * @returns {Promise<{status: number, data: Uint8Array}>}
   */
  async blockGet(e) {
    if (typeof e == "string") return this.transport.blockGet(e);
    if (this.transport instanceof N)
      return this.transport.blockGet(Me(e));
    const t = Ht(e), n = await this._sendAndWait(t, y.BLOCK_GET_RESPONSE);
    return jt(n);
  }
  /**
   * @param {string|Uint8Array} hash
   * @param {{force?: boolean}} [options] force=true removes pinned /
   *   ephemeral-claimed blocks too; otherwise those deletes resolve with
   *   status CONFLICT (wire.STATUS.CONFLICT).
   * @returns {Promise<{status: number}>}
   */
  async blockDelete(e, t = {}) {
    const n = t.force ? 1 : 0;
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
    const e = Kt(), t = await this._sendAndWait(e, y.HEALTH_RESPONSE), { json: n } = Gt(t);
    return JSON.parse(n);
  }
  /**
   * @param {string} [format='cbor']
   * @returns {Promise<{format: number, data: Uint8Array}>}
   */
  async peerInfo(e = "cbor") {
    if (this.transport instanceof N)
      return this.transport.peerInfo(e);
    const t = vt(ne[e] ?? 0), n = await this._sendAndWait(t, y.PEER_INFO_RESPONSE);
    return Qt(n);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<{status: number}>}
   */
  async peerConnect(e, t = 0) {
    if (this.transport instanceof N)
      return this.transport.peerConnect(e, t);
    const n = Vt(t, e), s = await this._sendAndWait(n, y.PEER_CONNECT_RESULT);
    return Wt(s);
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
  static offUrlToHttpUrl(e, t) {
    return Gr(e, t);
  }
  /**
   * Shared plumbing for the representation ephemeral/pin operations.
   * @private
   * @param {number} requestCode
   * @param {number} responseCode
   * @param {string} url
   * @returns {Promise<{status: number, blocks: number}>}
   */
  async _repOp(e, t, n) {
    const s = ur(e, n), i = await this._sendAndWait(s, t);
    return dr(i);
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
    const e = hr(), t = await this._sendAndWait(e, y.EPHEMERAL_LIST_RESPONSE);
    return pr(t);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async peerList() {
    if (this.transport instanceof N)
      return this.transport.peerList();
    const e = zt(), t = await this._sendAndWait(e, y.PEER_LIST_RESPONSE);
    return Jt(t);
  }
  /**
   * @param {Uint8Array} peerInfo
   * @param {number} [format=0]
   * @returns {Promise<void>}
   */
  async friendAdd(e, t = 0) {
    if (this.transport instanceof N)
      return this.transport.friendAdd(e, t);
    const n = Zt(t, e);
    await this.transport.send(n);
  }
  /**
   * @param {string|Uint8Array} nodeId
   * @returns {Promise<void>}
   */
  async friendRemove(e) {
    if (this.transport instanceof N)
      return this.transport.friendRemove(typeof e == "string" ? e : Me(e));
    const t = typeof e == "string" ? new TextEncoder().encode(e) : e, n = Yt(t);
    await this.transport.send(n);
  }
  /**
   * @returns {Promise<any[]>}
   */
  async friendList() {
    if (this.transport instanceof N)
      return this.transport.friendList();
    const e = Xt(), t = await this._sendAndWait(e, y.FRIEND_LIST_RESPONSE);
    return er(t);
  }
  /**
   * Add a bootstrap endpoint ("host:port" or "[ipv6]:port").
   * @param {string} endpoint
   * @returns {Promise<void>}
   */
  async bootstrapAdd(e) {
    if (this.transport instanceof N)
      return this.transport.bootstrapAdd(e);
    const t = tr(e);
    await this.transport.send(t);
  }
  /**
   * Remove a bootstrap endpoint ("host:port" or "[ipv6]:port").
   * @param {string} endpoint
   * @returns {Promise<void>}
   */
  async bootstrapRemove(e) {
    if (this.transport instanceof N)
      return this.transport.bootstrapRemove(e);
    const t = rr(e);
    await this.transport.send(t);
  }
  /**
   * List configured bootstrap endpoints. Each entry is [host, port, source]
   * with source 0=config and 1=managed.
   * @returns {Promise<any[]>} entries of [host, port, source]
   */
  async bootstrapList() {
    if (this.transport instanceof N)
      return this.transport.bootstrapList();
    const e = nr(), t = await this._sendAndWait(e, y.BOOTSTRAP_LIST_RESPONSE);
    return sr(t);
  }
  /**
   * @returns {Promise<any>}
   */
  async configShow() {
    if (this.transport instanceof N)
      return this.transport.configShow();
    const e = ir(), t = await this._sendAndWait(e, y.CONFIG_SHOW_RESPONSE), { json: n } = or(t);
    return JSON.parse(n);
  }
  /**
   * @param {string} field
   * @param {string} value
   * @returns {Promise<{status: number, restartRequired: boolean, message: string}>}
   */
  async configSet(e, t) {
    if (this.transport instanceof N)
      return this.transport.configSet(e, t);
    const n = ar(e, t), s = await this._sendAndWait(n, y.CONFIG_SET_RESPONSE);
    return fr(s);
  }
  /**
   * @returns {Promise<{status: number, message: string}>}
   */
  async configReload() {
    if (this.transport instanceof N)
      return this.transport.configReload();
    const e = lr(), t = await this._sendAndWait(e, y.CONFIG_RELOAD_RESPONSE);
    return cr(t);
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
  async putFolder(e, t = {}) {
    const n = Kr(e);
    if (n.length === 0)
      throw new Error("No files to upload");
    const s = t.recyclerUrls || [], i = n.length;
    let l = 0;
    const h = (x) => {
      var U;
      l++, (U = t.onProgress) == null || U.call(t, x, l, i);
    }, E = Xr(n.map((x) => x.path)), S = async (x) => {
      const U = be(x || E || "root"), q = en(n, x), v = tn(n, x), I = [];
      for (const M of v) {
        const X = (await S(M)).oriString, W = wt(X);
        if (!W)
          throw new Error(`Failed to parse subdirectory URL: ${X}`);
        const Q = ye(W.fileHashB58), ee = ye(W.descriptorHashB58);
        if (!Q || !ee)
          throw new Error(`Invalid directory hash in URL: ${X}`);
        I.push(zr({
          name: be(M),
          dirHash: Q,
          descriptorHash: ee,
          dirSize: W.streamLength
        }));
      }
      for (const M of q) {
        const R = be(M.path), X = qr(R), W = M.file.size;
        let Q;
        if (this.transport instanceof N) {
          const oe = gt(M.file);
          Q = (await this.put({
            contentType: X,
            fileName: R,
            streamLength: W,
            serverAddress: t.serverAddress,
            recyclerUrls: s,
            temporary: t.temporary
          }, oe)).oriString;
        } else {
          await this.putStreamStart({
            contentType: X,
            fileName: R,
            streamLength: W,
            serverAddress: t.serverAddress,
            recyclerUrls: s,
            temporary: t.temporary
          });
          const oe = gt(M.file).getReader();
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
        I.push(Wr({
          name: R,
          fileHash: ie,
          descriptorHash: de,
          finalByte: ee.streamLength
        })), h(R);
      }
      if (I.length === 0)
        throw new Error(`Empty directory: ${x || E}`);
      const H = Jr(I), se = `${U}.ofd`;
      return this.transport instanceof N ? this.put({
        contentType: "offsystem/directory",
        fileName: se,
        streamLength: H.length,
        serverAddress: t.serverAddress,
        recyclerUrls: s,
        temporary: t.temporary
      }, H) : (await this.putStreamStart({
        contentType: "offsystem/directory",
        fileName: se,
        streamLength: H.length,
        serverAddress: t.serverAddress,
        recyclerUrls: s,
        temporary: t.temporary
      }), await this.putStreamData(H), this.putStreamEnd());
    };
    return S(E);
  }
}
function Xr(r) {
  if (r.length === 0) return "";
  const e = r.map((i) => i.split("/").filter(Boolean)), t = e[0];
  let n = t.length;
  for (let i = 1; i < e.length; i++) {
    const l = e[i];
    let h = 0;
    for (; h < Math.min(n, l.length) && t[h] === l[h]; )
      h++;
    if (n = h, n === 0) break;
  }
  const s = Math.min(n, t.length - 1);
  return t.slice(0, s).join("/");
}
function en(r, e) {
  const t = e ? `${e}/` : "";
  return r.filter((n) => {
    if (!n.path.startsWith(t)) return !1;
    const s = n.path.slice(t.length);
    return s.length > 0 && !s.includes("/");
  });
}
function tn(r, e) {
  const t = e ? `${e}/` : "", n = /* @__PURE__ */ new Set();
  for (const s of r) {
    if (!s.path.startsWith(t)) continue;
    const i = s.path.slice(t.length);
    if (!i) continue;
    const l = i.indexOf("/");
    l > 0 && n.add(t + i.slice(0, l));
  }
  return Array.from(n);
}
var Fe = globalThis.OffsClient;
Fe && Fe.OffsClient && (globalThis.OffsClient = Fe.OffsClient);
export {
  sn as OffsClient,
  ye as base58Decode,
  Me as base58Encode,
  qr as mimeFromExtension,
  Gr as offUrlToHttpUrl,
  wt as parseOffUrl,
  nn as wire
};
//# sourceMappingURL=offs-client.esm.js.map
