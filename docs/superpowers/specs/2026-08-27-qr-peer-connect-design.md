# QR Peer Connect — Design

Date: 2026-08-27
Status: Approved design (brainstorm session 2026-08-27)

## Goal

Peer information can already be shared as a QR image over HTTP
(`GET /peer/info?format=qrcode`), but only conditionally (system libqrencode)
and only in one direction: there is no decode path anywhere. This feature
makes QR a first-class, symmetric transport for peer info across **every**
client API surface — HTTP, the CBOR socket transports (unix/TCP/WS/WT), the
C client library, the JS client library, and the `offs` CLI:

- **Generate**: any transport can produce a QR image (PPM) of the local
  peer info.
- **Decode**: any transport can submit a QR image (PPM) to connect to a peer
  or add a friend.

Decisions made during brainstorming:

1. Decode is **daemon-side**: clients submit image bytes; the daemon decodes.
2. Accepted image format = **the format we produce** (binary P6 PPM). No
   general image-codec dependency.
3. Full parity: QR generation is added to the socket wire protocol too, not
   left HTTP-only.
4. Both `peer connect` and `friend add` accept QR images.

## 1. Dependencies and build

Two new git submodules, added to `.gitmodules` and wired with
`add_subdirectory` following the existing `bcrypt`/`libcbor` pattern:

| Submodule | Role | Notes |
|---|---|---|
| `deps/libqrencode` | QR encoder | Vendored; replaces the pkg-config probe. `HAS_QRENCODE` becomes unconditional and the `501 QR code generation not available` branch in `peer_routes.c` is deleted. |
| `deps/quirc` | QR decoder | Small pure-C BSD library, no transitive deps. Linked into the liboffs core (not per-transport). |

Both are required (loud `FATAL_ERROR` on missing submodule, like bcrypt), not
optional probes.

## 2. QR codec module — `src/QR/`

Exactly two public operations; all transports call these so behavior cannot
drift:

```c
/* Serialize the CBOR payload into a P6 PPM QR image (libqrencode,
   QR_ECLEVEL_M, 4x pixel scale — byte-compatible with today's output).
   Returns buffer (caller frees) or NULL on encode failure. */
buffer_t* qr_encode_to_ppm(const uint8_t* cbor_data, size_t cbor_len);

/* Parse a P6 PPM, decode the QR with quirc, return the raw payload bytes.
   Returns buffer or NULL (bad image / no QR found / decode failure). */
buffer_t* qr_decode_from_ppm(const uint8_t* ppm_data, size_t ppm_len);
```

- The PPM rendering code currently inside `_peer_info_handler`
  (`src/ClientAPI/HTTP/peer_routes.c:180-244`) moves here verbatim.
- The PPM parser is strict: `P6` magic, whitespace, dimensions, `255` maxval,
  binary pixel data — exactly what we generate. RGB → luma conversion feeds
  quirc's grayscale buffer.
- Neither function knows about peer_info, HTTP, or CBOR framing; callers
  decide what the payload means. The module is independently testable.

## 3. Wire protocol changes (`src/ClientAPI/client_api_wire.h`)

Format byte **2** = "PPM QR image", consistently:

| Frame | Current | Change |
|---|---|---|
| `PEER_INFO_REQUEST` (21) | `[21]` | `[21]` or `[21, format]`; 0 = raw CBOR (default, backward compatible), 1 = base58, 2 = PPM QR image |
| `PEER_INFO_RESPONSE` (22) | `[22, format_byte, data]` | unchanged shape; format 2 legal, `data` = PPM bytes |
| `PEER_CONNECT` (23) | `[23, format_byte, data]` | format 2 accepted: `data` = PPM image → decode → peer_info CBOR → connect |
| `FRIEND_ADD` (27) | `[27, format_byte, data]` | format 2 accepted, same flow |

Backward compatibility: frame decoders accept the old shapes unchanged
(1-element `PEER_INFO_REQUEST`, formats 0/1 everywhere). Old clients keep
working.

## 4. HTTP changes (`src/ClientAPI/HTTP/peer_routes.c`)

- `GET /peer/info?format=qrcode` — handler shrinks to
  `qr_encode_to_ppm(peer_info_encode(info))`. Vendored encoder means this
  always works; the `#ifdef HAS_QRENCODE` / 501 branches are removed.
- `POST /peer/connect` and `POST /friends` — body dispatch by `Content-Type`:
  - `application/cbor` → as today
  - text (base58) → as today
  - `image/x-portable-pixmap` → `qr_decode_from_ppm()` → peer_info CBOR →
    existing flow

Both routes share one helper (`peer_info_from_body()`) so HTTP and sockets
cannot drift on what a valid QR peer is.

## 5. Client libraries

**C client** (`src/ClientLibs/c/offs_client.h`) — generalized `_ex` forms
with explicit format byte, plus QR sugar:

```c
offs_client_peer_info_ex(client, format /*0|1|2*/, cb, ctx);
offs_client_peer_connect_ex(client, format, data, data_len, cb, ctx);
offs_client_friend_add_ex(client, format, data, data_len, cb, ctx);
offs_client_peer_info_qr(...);   /* format 2 sugar: returns PPM */
offs_client_peer_connect_qr(client, ppm, ppm_len, cb, ctx);
offs_client_friend_add_qr(client, ppm, ppm_len, cb, ctx);
```

**JS client** (`src/ClientLibs/js/offs-client/`):

- `peerInfo(format)` — format passthrough (`'cbor' | 'base58' | 'qrcode'`);
  HTTP transport maps `qrcode` to the query param, CBOR transports send
  format byte 2.
- `peerConnectQr(ppmBytes)` / `friendAddQr(ppmBytes)` — HTTP: POST with
  `Content-Type: image/x-portable-pixmap`; CBOR transports: format 2 frame.
  `dist/` rebuilt via the package's existing build.

## 6. CLI (`OFFS/src/offs/`)

- `offs peer info --qr <file>` — format-2 request; writes PPM to `<file>`
  (`-` = stdout).
- `offs peer connect --qr <file>` / `offs friend add --qr <file>` — read
  PPM from file, send format 2, print usual `ok`/error.

## 7. Error handling

One rule everywhere — *image problems* vs *peer problems* are distinct:

| Failure | Socket | HTTP |
|---|---|---|
| Bad PPM header / truncated image | `ERROR`, status `BAD_REQUEST`, "qr decode failed: <reason>" | 400 text/plain |
| QR found but payload not valid peer info | `ERROR`, status `BAD_REQUEST`, "invalid peer info in qr" | 400 text/plain |
| Connect-level outcome (already connected, rejected, ...) | unchanged `PEER_CONNECT_RESULT` | unchanged `{"status": 0..4}` |

## 8. Testing

Following the existing `test/` gtest layout:

- `test_qr.cpp` — unit: PPM parser edge cases (bad magic, truncated data,
  wrong maxval), encode→decode round-trip (bytes in == bytes out), decode of
  a hand-built PPM.
- `test_peer_routes.cpp` additions — HTTP round trip:
  `GET /peer/info?format=qrcode` → feed PPM into `POST /peer/connect` and
  `POST /friends` (QR content type); 400 paths for garbage images.
- Wire-frame tests — format-2 encode/decode; old-shape backward compat.
- Valgrind pass with `-gdwarf-4` per project convention; no new leaks.

## 9. Out of scope

- PNG/JPEG/camera image support (daemon accepts only the PPM it produces).
- QR generation/decode in the GUI itself (separate project; this feature
  gives it the server-side primitives).
- Non-peer-info QR payloads.