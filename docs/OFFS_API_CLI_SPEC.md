# OFFS API / CLI Feature Specification for GUI Download Manager

This document catalogs every feature exposed by the OFFS HTTP API, the
socket (CBOR) wire protocol, and the `offs` CLI, including the exact data
types each sends and receives. It is intended as the requirements source
for building a GUI download/upload manager with full feature parity.

Sources of truth (verify against these if anything here seems wrong):

| Area | File |
|---|---|
| GET/PUT `/offsystem` routes | `src/ClientAPI/HTTP/off_routes.c` |
| Block routes | `src/ClientAPI/HTTP/block_routes.c` |
| Peer/friend routes | `src/ClientAPI/HTTP/peer_routes.c` |
| Config routes | `src/ClientAPI/HTTP/config_routes.c` |
| Health | `src/ClientAPI/health_handler.c`, `health_routes.c` |
| Auth middleware | `src/ClientAPI/HTTP/auth_middleware.c` |
| CBOR wire protocol | `src/ClientAPI/client_api_wire.h` |
| C client library | `src/ClientLibs/c/offs_client.h/.c` |
| JS client library | `src/ClientLibs/js/offs-client/src/` |
| `offs` CLI | `OFFS/src/offs/` (main.c, cli_util.c, commands/*) |
| `offsd` daemon flags | `OFFS/src/offsd/main.c:198-229` |
| ORI / OFF URL | `src/OFFStreams/off_url.h/.c` |
| OFD directories | `src/OFFStreams/ofd.h/.c`, JS mirror `ofd.js` |
| Blocks / cache | `src/BlockCache/block.h`, `block_cache.h` |
| Peer info | `src/Network/peer_info.h/.c`, `node_id.h` |
| Config fields | `src/Configuration/config_json.c` |
| Size limits | `src/Util/validation.h` |

---

## 1. Core concepts (needed to design the GUI)

### 1.1 Blocks

- OFFS splits every file into fixed-size **blocks** and XOR-mixes each data
  block with `tuple_size - 1` random blocks. A **tuple** is the ordered list
  of block hashes needed to reconstruct one data block.
- Block sizes (`src/BlockCache/block.h`):
  - `mega` = 1,000,000 bytes
  - `standard` = 128,000 bytes (default for all URL-based transfers)
  - `mini` = 64,000
  - `nano` = 136
- Every block is content-addressed by a **32-byte BLAKE3 hash**, rendered
  as **base58** text in URLs and APIs
  (alphabet `123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz`).

### 1.2 ORI / OFF URL

The universal file reference. `PUT /offsystem` returns one; `GET` consumes one.

String format (`src/OFFStreams/off_url.c:216-222`):

```
[server-address]/offsystem/v3/{content-type}/{stream-length}/{file-hash-b58}/{descriptor-hash-b58}/{file-name}
```

- `server-address` — e.g. `http://localhost:23402` (default when parsing a bare ORI)
- `content-type` — MIME type, may contain `/` (e.g. `application/octet-stream`),
  or the special value `offsystem/directory` for OFD directories
- `stream-length` — total file size in bytes (decimal)
- `file-hash`, `descriptor-hash` — 32-byte BLAKE3, base58-encoded
- `file-name` — free text (no `/`)

In-memory form (`ori_t`, `src/OFFStreams/ori.h:49-59`):

```c
typedef struct {
  refcounter_t refcounter;
  buffer_t* descriptor_hash;   // 32 bytes
  size_t descriptor_offset;
  block_size_e block_type;     // 128000 default
  size_t tuple_size;           // 3 default
  buffer_t* file_hash;         // 32 bytes
  size_t file_offset;
  char* file_name;
  size_t final_byte;           // total file size
} ori_t;
```

URL-parseable form (`off_url_t`, `src/OFFStreams/off_url.h:16-24`):

```c
typedef struct {
  char* server_address;
  char* content_type;
  size_t stream_length;
  size_t stream_offset;
  buffer_t* file_hash;       // 32-byte BLAKE3
  buffer_t* descriptor_hash; // 32-byte BLAKE3
  char* file_name;
} off_url_t;
```

A GUI needs an ORI parser/renderer: split into the 7 fields above, and
re-compose for display/copy.

### 1.3 OFD — OFF File Directory

A directory is itself an OFFS object ("OFF File Directory") stored as an
OFF URL with content type `offsystem/directory`. Its body is CBOR:
`{"v": 1, "entries": [...]}` where each entry is a map:

| Key | Meaning | Present for |
|---|---|---|
| `n` | name (string) | all |
| `t` | type: 0 = file, 1 = directory | all |
| `f` | file hash (bstr, 32) | files |
| `D` | descriptor hash (bstr, 32) | files |
| `s` | final byte / file size | files |
| `B` | block type (default 128000) | files |
| `T` | tuple size (default 3) | files |
| `o` | file offset | files |
| `d` | directory hash (bstr) | directories |

Defined in `src/OFFStreams/ofd.c:77-155`; JS mirror in
`src/ClientLibs/js/offs-client/src/ofd.js:86-138`.

Downloading a directory URL resolves a path inside it: `/index.html` is
served if the requested name ends in `.ofd`; otherwise the named entry is
looked up. `?ofd=raw` returns the raw OFD CBOR bytes as `application/cbor`.

The JS client's `putFolder()` shows the intended directory workflow:
recursively upload the tree, build an OFD CBOR per directory, upload each
directory as `content type: offsystem/directory` with file name `<dir>.ofd`,
and return the root directory's ORI. A GUI file browser should be able to
parse an OFD and list/download its children.

### 1.4 Limits and validation

From `src/Util/validation.h` and route code:

- `OFFS_MAX_CBOR_MESSAGE_SIZE` = 64 MB (also `OFFS_MAX_STREAM_LENGTH`)
- Block PUT body: `0 < size <= 128000`
- `type` header ≤ 256 chars; `file-name` ≤ 1024 chars, no `/`
- `tuple-size` default 3, must be ≤ node config `max_tuple_size` (daemon default 5)
- Single Range header only (no multi-range)
- Uploads are rejected with 500 if the cache cannot fit
  `writeable_off_stream_estimate_required_bytes(stream_length, tuple_size, 32)`

---

## 2. HTTP API (REST surface)

Base: the daemon's HTTP port (default **23402**; HTTPS port configurable).

### 2.1 Download — `GET /offsystem/v3/{type}/{stream-length}/{file-hash}/{descriptor-hash}/{file-name}`

Route pattern (`off_routes.c:71`):

```
/offsystem/v3/([-+._a-zA-Z0-9]+/[-+._a-zA-Z0-9-]+|[-+._a-zA-Z0-9]+)/([0-9]+)/([base58]+)/([base58]+)/([^!`&*()+]+|\[ !$`&*()+]+)+
```

Request:

- Path params: content type (percent-decoded), stream length (decimal),
  file hash (base58, decodes to 32 bytes), descriptor hash (base58, 32 bytes),
  file name.
- Query params:
  - `?ofd=raw` — for `offsystem/directory` type: return raw OFD bytes as
    `application/cbor` (404 if neither the OFD cache nor block cache has it).
  - Without `?ofd=raw`, directory URLs resolve `index.html` (if the name ends
    in `.ofd`) or the named path inside the OFD; 404 if unresolvable.
  - `?load=1` (or bare `?load`) — **cache-load mode**: the daemon pulls the
    file's tuples into its block cache WITHOUT sending any file data. Any
    other value (`?load=0`, etc.) is ignored and data is served normally.
- Optional header `Range: bytes=start-end | start- | -suffix`
  (single range; multi-range rejected). With `?load=1`, Range selects which
  tuples are loaded (trimmed to the byte range); 206/416 semantics are the
  same as a ranged GET.
- `?load=1` response is `application/x-ndjson`, one JSON object per line:
  - Progress (one per resolved tuple): `{"tuples_loaded":n,"tuples_total":m}`
  - Terminal: `{"status":"loaded|partial|failed","tuples_loaded":n,"tuples_total":m}`
    (`partial` = some tuples skipped; `failed` = none loaded)
- `?load=1` on a directory ORI (after resolution) →
  `400` "Load requires a file ORI, not a directory".
- Known limitation: loads taking longer than ~60 s are truncated by the
  connection idle/hard timers (tracked as OFFS-190).

Response:

| Status | Meaning | Body |
|---|---|---|
| 200 | Full content | `Content-Type` from URL type or MIME-from-extension, `Accept-Ranges: bytes`, `Content-Length`, streamed body |
| 206 | Valid Range | `Content-Range: bytes start-end/size`, partial body |
| 416 | Invalid Range | `Content-Range: bytes */{size}` |
| 404 | Unresolved directory path / missing raw OFD | — |
| 400 | URL failed to parse | — |

GUI notes: the body streams as blocks arrive from cache/network — support
progress by counting received bytes against `Content-Length` (or the range
length). Range support enables resume/partial download.

### 2.2 Upload — `PUT /offsystem`

Required headers:

| Header | Constraints |
|---|---|
| `type` | content type, ≤ 256 chars |
| `file-name` | ≤ 1024 chars, no `/` |
| `stream-length` | decimal, 1..64 MB |

Optional headers:

| Header | Meaning |
|---|---|
| `server-address` | embedded into the returned ORI string |
| `recycler` | JSON array of OFF URLs, e.g. `["/offsystem/v3/..."]` — recycles blocks from those files instead of allocating fresh ones |
| `temporary` | `"true"` marks upload temporary |
| `tuple-size` | erasure width, default 3; 400 if > node `max_tuple_size` |
| `Content-Type: multipart/form-data` | body parsed as multipart; first file part used (forces buffered path) |

Body: raw bytes (may be chunked/streamed) or multipart. Streaming path:
after headers pass validation, body chunks stream directly into the
block-writing pipeline; bytes beyond `stream-length` are dropped.

Responses:

| Status | Meaning | Body |
|---|---|---|
| 200 | Success | `text/plain` — the ORI (OFF URL) string |
| 400 | Missing/invalid headers or tuple-size | — |
| 500 | Cache full ("configure larger max_capacity_bytes") | — |

CORS: `Access-Control-Allow-Origin: *`,
`Access-Control-Expose-Headers: Content-Type, Content-Range, Content-Length`.

### 2.3 Blocks (auth required)

Registered only when an API key is configured.

#### `PUT /blocks`

- Optional query `?encoding=base58` → hash returned as base58 text.
- Body: raw block bytes, `0 < size <= 128000`, else 400.
- Response `201 Created`: 32-byte hash (`application/octet-stream`) or
  base58 string (`text/plain`); 500 on failure.

#### `GET /blocks/{base58-hash}`

- Path hash must decode to exactly 32 bytes else 400.
- `200` `application/octet-stream` raw block data (padded to block size);
  `404` if absent.

#### `DELETE /blocks/{base58-hash}`

- `204 No Content` on success; `404` if not removed.

#### `POST /blocks/defragment`

- Query: `threshold=<0.0..1.0>` (default 0.5).
- `200` JSON: `{"result": <int>, "sections_defragmented": <n>, "blocks_relocated": <n>}`.

### 2.4 Peers and friends (auth required)

#### `GET /peer/info`

- Requires a node identity (CA + node cert): without one the daemon has no
  authority public key and cannot produce peer info.
- Query `format=`: `cbor` (default) | `base58` | `qrcode`.
- `200`:
  - `cbor`: `application/cbor` peer-info map (§5.5)
  - `base58`: `text/plain` base58 of the CBOR
  - `qrcode`: `image/x-portable-pixmap` QR of the CBOR bytes (always
    available — vendored libqrencode; image includes a 4-module quiet zone)
- LAN (HOST) candidates included only when the request is authenticated.

#### `POST /peer/connect`

- Body, one of:
  - `application/cbor` peer-info map
  - plain-text base58 (default)
  - `image/x-portable-pixmap` — P6 PPM QR image; the daemon decodes the QR
    and parses the embedded peer-info CBOR
- `200` JSON: `{"status": <0..4>, "message": "..."}` —
  0 OK "Connection initiated", 1 already connected, 2 invalid peer info,
  3 failed, 4 rejected. An undecodable image or invalid peer info is
  **not** a 400: it returns HTTP 200 with
  `{"status": 2, "message": "Invalid peer info"}`.

#### `GET /peers`

- `200` JSON array: `[{"node_id": string, "connected": bool, "is_friend": bool, "in_ring": bool}]`
  (connection-manager peers plus gossip/ring members, the latter
  `in_ring: true, connected: false`).

#### `POST /friends`

- Body: peer info — CBOR map, base58 text, or `image/x-portable-pixmap`
  QR image (decoded server-side like `/peer/connect`; undecodable input
  yields HTTP 200 `{"status": 2, "message": "Invalid peer info"}`).
- `201`/`200` `{"status":"added"}`; `409` `{"status":"already_friend"}`;
  400 invalid. Persists and attempts immediate connect.

#### `DELETE /friends/{node_id}`

- `200` `{"status":"removed"}`; `404` unknown; `400` bad node_id.

#### `GET /friends`

- `200` JSON array: `[{"node_id": string, "connected": bool}]`.

### 2.5 Config (local-binding protected)

#### `GET /config`

- `200` full config as JSON (§7). 401 if unauthenticated.

#### `PUT /config`

- Body: JSON object of field → value (known fields in §7).
- `200` JSON: `{"staged": [field...], "rejected": [{"field":..., "reason":...}], "restart_required": bool}`
- Mutations on non-loopback bindings → `403 {"error":"config mutation requires local transport"}`.

#### `POST /config/restart`

- `202 {"message":"restarting"}` if a pending config exists;
  `409 {"error":"no pending config to apply"}` otherwise.

### 2.6 Health — `GET /health`

`200` `application/json` (`src/ClientAPI/health_handler.c:95-142`):

```json
{
  "status": "running|stopped|draining|unknown",
  "uptime_seconds": 0,
  "node_id": "<48-char base58 string>",
  "peer_count": 0,
  "total_connections": 0,
  "avg_hebbian_weight": 0.0,
  "block_cache": { "current_bytes": 0, "max_bytes": 0, "block_count": 0 },
  "rate_limits": [
    { "type": "find_block|store_block|seeking_blocks|ping_capacity|ping",
      "accepted": 0, "rejected": 0, "avg_tokens": 0.0, "effective_rate": 0.0 }
  ],
  "rpc_calls": [ { "name": "<rpc name>", "count": 0 } ]
}
```

`node_id` omitted if unknown. `rate_limits` and `rpc_calls` are arrays of
the above shapes (only non-zero RPC names listed).

### 2.7 Authentication (HTTP)

- Enabled when the node has an API key configured. Middleware chain on all
  routes: draining check → CORS → bearer auth.
- `Authorization: Bearer <token>`; token checked against the stored bcrypt
  hash. Missing header/scheme → `401` + `WWW-Authenticate: Bearer`;
  wrong key → `403`.
- Loopback bindings may bypass bearer via config `config_local_binding_no_auth`.
- `/offsystem` GET/PUT and `/health` are registered regardless of auth;
  `/blocks`, `/peer/*`, `/friends` are only registered when auth is enabled.

---

## 3. Socket wire protocol (Unix / TCP / WebSocket / WebTransport)

All non-HTTP transports (the daemon's Unix socket — default
`/var/run/offs.sock` — TCP, WS, and WebTransport) speak a **length-prefixed
CBOR** protocol. Every frame is a **CBOR array whose first element is the
message type**. Defined in `src/ClientAPI/client_api_wire.h`.

- Unix / TCP / WebTransport: length-prefixed via `stream_frame_encode`.
- WebSocket: binary WS frames (no extra length prefix), upgrade at `GET /offs`.
- Max frame: 64 MB; clients auto-chunk large buffers (C client: 256 KB chunks).
- On auth-enabled sockets, frames are rejected with status 5 (UNAUTHORIZED)
  until an `AUTH_REQUEST` succeeds.

Message types (`client_api_wire.h:13-48`):

| Type | Name | Payload shape |
|---|---|---|
| 1 | PUT_REQUEST | see below |
| 2 | PUT_DATA | `[2, bytestring]` |
| 3 | PUT_END | `[3]` |
| 4 | PUT_RESPONSE | `[4, ori_string]` |
| 5 | GET_REQUEST | `[5, ori_string, has_range, range_start?, range_end?]` |
| 6 | GET_RESPONSE_START | `[6, content_type, content_length, has_range, range_start?, range_end?]` |
| 7 | GET_DATA | `[7, bytestring]` |
| 8 | GET_END | `[8]` |
| 11 | ERROR | `[11, status_code, message]` |
| 12 | AUTH_REQUEST | `[12, api_key: bytestring]` |
| 13 | BLOCK_PUT_REQUEST | `[13, data: bstr, encoding: uint]` (0=raw, 1=base58) |
| 14 | BLOCK_PUT_RESPONSE | `[14, status: uint, hash: bstr|tstr]` |
| 15 | BLOCK_GET_REQUEST | `[15, hash: bstr]` |
| 16 | BLOCK_GET_RESPONSE | `[16, status: uint, data: bstr]` |
| 17 | BLOCK_DELETE_REQUEST | `[17, hash: bstr]` |
| 18 | BLOCK_DELETE_RESPONSE | `[18, status: uint]` |
| 19 | HEALTH_REQUEST | `[19]` |
| 20 | HEALTH_RESPONSE | `[20, json_string]` |
| 21 | PEER_INFO_REQUEST | `[21]` or `[21, format]` (format 0=raw CBOR default, 1=base58, 2=PPM QR image) |
| 22 | PEER_INFO_RESPONSE | `[22, format_byte, data: bstr]` (format 0=raw CBOR, 1=base58 text, 2=PPM QR image) |
| 23 | PEER_CONNECT | `[23, format_byte, data: bstr]` (format 0=raw CBOR, 1=base58, 2=PPM QR image) |
| 24 | PEER_CONNECT_RESULT | `[24, status: uint]` |
| 25 | PEER_LIST_REQUEST | `[25]` |
| 26 | PEER_LIST_RESPONSE | `[26, peers: cbor_array]` |
| 27 | FRIEND_ADD | `[27, format_byte, data: bstr]` (format 0=raw CBOR, 1=base58, 2=PPM QR image) |
| 28 | FRIEND_REMOVE | `[28, node_id: bstr]` |
| 29 | FRIEND_LIST | `[29]` |
| 30 | FRIEND_LIST_RESPONSE | `[30, friends: cbor_array]` |
| 31 | UPDATE_STATUS_REQUEST | `[31]` |
| 32 | UPDATE_STATUS_RESPONSE | `[32, json_string]` |
| 33 | CONFIG_SHOW_REQUEST | `[33]` |
| 34 | CONFIG_SHOW_RESPONSE | `[34, json_string]` |
| 35 | CONFIG_SET_REQUEST | `[35, field: tstr, value: tstr]` (value always a string) |
| 36 | CONFIG_SET_RESPONSE | `[36, status: uint, restart_required: uint, message: tstr]` (status 0=staged, 1=rejected) |
| 37 | CONFIG_RELOAD_REQUEST | `[37]` |
| 38 | CONFIG_RELOAD_RESPONSE | `[38, status: uint, message: tstr]` (0=restarting, 1=none/error) |
| 39 | LOAD_REQUEST | `[39, ori_string]` or `[39, ori_string, has_range, range_start, range_end]` — same optional-range shape as GET_REQUEST; pulls the file's blocks into the cache, no data sent back |
| 40 | LOAD_PROGRESS | `[40, tuples_loaded: uint, tuples_total: uint]` (one per resolved tuple; `total - loaded` includes in-flight and skipped) |
| 41 | LOAD_END | `[41, status: uint, tuples_loaded: uint, tuples_total: uint]` (status 0=loaded, 1=partial, 2=failed) |

Peer-info payloads (PEER_INFO_RESPONSE/PEER_CONNECT/FRIEND_ADD data) are
capped at 2 MB (`CLIENT_API_PEER_INFO_MAX_PAYLOAD`, `client_api_wire.c`) —
raised to fit QR PPM images, which are far larger than the raw CBOR blob.

Status codes (`client_api_wire.h:51-56`):
0 OK, 1 BAD_REQUEST, 2 NOT_FOUND, 3 INTERNAL_ERROR, 4 RANGE_NOT_SATISFIABLE,
5 UNAUTHORIZED.

### 3.1 PUT flow (socket)

1. `[1, content_type, file_name, stream_length, server_address, data,
   recycler_urls, temporary, tuple_size?]`
   - `data` empty for streaming; 9-element form (with `tuple_size`) only
     when tuple size is set.
   - `recycler_urls` = array of OFF URL strings.
2. `[2, chunk: bstr]` repeated (client chunks at 63 MiB; framer cap forces
   chunking — the C client chunks >1 MB buffers into 256 KB).
3. `[3]` PUT_END.
4. Reply `[4, ori_string]` or `[11, status, message]`.

`tuple_size` must be ≤ daemon `max_tuple_size` (default 5).

### 3.2 GET flow (socket)

1. `[5, ori_string, has_range, range_start?, range_end?]`
2. `[6, content_type, content_length, has_range, range_start?, range_end?]`
3. `[7, chunk: bstr]` repeated
4. `[8]` GET_END, or `[11, status, message]` (status 4 = range not satisfiable).

### 3.3 LOAD flow (socket)

1. `[39, ori_string, has_range?, range_start?, range_end?]`
2. `[40, tuples_loaded, tuples_total]` repeated (one per resolved tuple)
3. `[41, status, tuples_loaded, tuples_total]`, or `[11, status, message]`
   (daemon-side rejections — bad ORI, unauthorized — arrive as ERROR frames
   before the first LOAD_PROGRESS).

Transport note: LOAD is **network-aware on the unix transport** (the unix
connection has access to the node's network actor, so missing tuples are
fetched from peers). WS/TCP LOAD is **cache-only** — those connections carry
no network actor, so only tuples already in the block cache resolve (missing
tuples are skipped → `partial`). WebTransport has no LOAD (it has no GET
support either).

### 3.4 Other socket-only surfaces

- Update status (31/32) JSON:
  `{"enabled": bool, "channel": str, "check_interval_hours": n, "state": str|"idle", "current_version": str, "available_version": str|"none"}`
- Config show (33/34) returns the same JSON as HTTP `GET /config`.

---

## 4. `offs` CLI (feature parity checklist)

Entry: `OFFS/src/offs/main.c`. Global flags: `--socket <path>` (default
`/var/run/offs.sock`), `--lang <code>`. Commands (`cli_util.c:25-40`):
`start, stop, restart, put, get, block, peer, config, friend, health, status,
version, help`. Transport: Unix socket, CBOR frames (§3).

| Command | Args/flags | Operation | Output |
|---|---|---|---|
| `offs put <file>` | `--temporary`, `--recycler <url>`, `--tuple-size N`, `--help` | Streaming PUT (§3.1); content type from extension; chunks 63 MiB | progress on stderr (`Putting <file>: n/total bytes (pct)`); success prints ORI |
| `offs get <ori> [--output <path>]` | `--output` | GET flow (§3.2); detects truncation (no GET_END) | bytes to stdout/file |
| `offs load <ori>` | — | LOAD flow (§3.3): pull the file's tuples into the daemon cache, no file data | progress on stderr; exit 0 loaded/partial, 1 failed |
| `offs block put <data>` | `--encoding base58` | BLOCK_PUT | hash |
| `offs block get <hash>` | hash | BLOCK_GET | raw block bytes |
| `offs block delete <hash>` | hash | BLOCK_DELETE | "ok" |
| `offs peer info` | `--qr <file>` or `--qr -` (stdout) | PEER_INFO | "Peer Info" + base58 blob, or QR PPM image |
| `offs peer list` | — | PEER_LIST | peer count |
| `offs peer connect <b58>` | peer info base58, or `--qr <file>` (read PPM QR image) | PEER_CONNECT | "ok" |
| `offs friend add <b58>` | peer info, or `--qr <file>` (read PPM QR image) | FRIEND_ADD | "ok" |
| `offs friend remove <node_id>` | node id | FRIEND_REMOVE | "ok" |
| `offs friend list` | — | FRIEND_LIST | friend count |
| `offs health` | — | HEALTH | pretty-printed health JSON |
| `offs status` | — | HEALTH + UPDATE_STATUS | health JSON + update status (enabled/channel/current_version/state/available_version/check_interval_hours) |
| `offs version` | — | client-side | `offs version 0.1.0` |
| `offs start/stop/restart` | — | daemon lifecycle via service/pid files (no socket) | — |
| `offs config show` | — | CONFIG_SHOW | config JSON |
| `offs config get <field>` | field | CONFIG_SHOW | value |
| `offs config set <field>=<value>` | — | CONFIG_SET | status |
| `offs config add/remove <field> <value>` | — | CONFIG_SET | status |
| `offs config set-auth <hash>` | bcrypt hash | CONFIG_SET | status |
| `offs config generate-auth <key> [--cost N]` | bcrypt client-side | CONFIG_SET | status |
| `offs config reload` | — | CONFIG_RELOAD | status |

`start`/`stop`/`restart`/`version` never open the socket.

### 4.1 `offsd` daemon flags (for a GUI that manages the daemon)

`OFFS/src/offsd/main.c:198-229`: `--config`, `--host`, `--port` (HTTP, default
23402), `--quic-port` (23401), `--unix <path>`, `--cache-dir`, `--data-dir`,
`--pid-file`, `--workers`, `--foreground`, `--log-file`, `--log-level`,
`--log-structured`, `--metrics-server`, `--ca-cert`, `--node-cert`,
`--node-key`, `--relay-url`, `--max-capacity-bytes` (default 5 GiB),
`--api-key` (random one generated + printed if omitted), `--ws-port`,
`--wt-port`, `--wt-h3-port`, `--ws-cert/--ws-key`, `--wt-cert/--wt-key`,
`--allow-secure`, `--help`.

---

## 5. Data types reference

### 5.1 Block

```c
typedef enum { mega = 1000000, standard = 128000, mini = 64000, nano = 136 } block_size_e;

typedef struct {
  refcounter_t refcounter;
  buffer_t* data;   // payload, padded to block size
  buffer_t* hash;   // 32-byte BLAKE3
} block_t;
```

### 5.2 Tuple (XOR recipe)

Ordered list of block hashes (`tuple_size` entries) that reconstruct one
data block. Descriptor = sequence of tuples + 32-byte pad; descriptor
itself is stored as blocks.

### 5.3 Recipes

- `new_blocks_recipe_t` — allocate fresh random blocks (default).
- `recycler_recipe_t` — recycle blocks from existing ORIs (the `recycler`
  header / `--recycler` flag: JSON array of OFF URLs).

### 5.4 ORI — see §1.2.

### 5.5 Peer info

```c
#define NODE_ID_HASH_SIZE 32
#define NODE_ID_STRING_SIZE 48
typedef struct node_id_t { uint8_t hash[32]; char str[48]; } node_id_t;

typedef enum { PEER_ADDR_DIRECT=0, PEER_ADDR_RELAY=1, PEER_ADDR_HOST=2, PEER_ADDR_SRFLX=3 } peer_addr_type_e;
typedef struct peer_address_t { peer_addr_type_e type; char* host; uint16_t port; uint32_t relay_id; } peer_address_t;

#define PEER_INFO_MAX_ADDRESSES 8
typedef struct {
  node_id_t node_id;
  uint8_t* public_key; size_t public_key_len;
  peer_address_t* addresses; size_t address_count;
} peer_info_t;
```

CBOR encoding (`src/Network/peer_info.c:62-121`): map of 3 pairs —
`node_id` (bstr 32), `public_key` (bstr), `addresses` (array of maps:
`type` u8, `host` tstr, `port` u16, `relay_id` u32). Base58 form =
base58(CBOR bytes). This blob is what `peer connect` / `friend add`
consume and what `peer info` produces — QR-code exchange is the intended
sharing mechanism.

### 5.6 OFD — see §1.3.

---

## 6. Client libraries (reference behavior)

### 6.1 C client (`src/ClientLibs/c/offs_client.h`)

- Connect URLs: `unix://path`, `tcp://host:port`, `ws://`, `wss://`, `wt://`,
  `wts://`. Config defaults: connect timeout 5000 ms, max retries 3,
  retry base delay 1000 ms with exponential backoff + jitter,
  `allow_secure=false`.
- Sends `AUTH_REQUEST [12, api_key]` immediately on connect when a key is set.
- Operations: `put` (buffered, auto-chunks >1 MB), `put_stream_start/data/end`,
  `get` (data/end/error callbacks, range supported on the wire),
  `load` (`offs_client_load(ori, has_range, range_start, range_end,
  progress_cb, progress_ctx, end_cb, end_ctx)` — cache-load without file
  data; progress_cb fires per resolved tuple, end_cb fires exactly once with
  status `CLIENT_API_LOAD_STATUS_LOADED/PARTIAL/FAILED`; one load
  outstanding per connection),
  `block_put/get/delete`, `health`, plus `offs_http_get(url)` (blocking HTTP/1.1).
- Callback model (`offs_client.h:52-61`) — a GUI download manager should
  mirror this event model:
  `put_response(ctx, ori)`, `get_data(ctx, data, len)`, `get_end(ctx)`,
  `error(ctx, status_code, message)`, `block_put(ctx, status, hash, len, is_text)`,
  `block_get(ctx, status, data, len)`, `block_delete(ctx, status)`,
  `health(ctx, json)`.

### 6.2 JS client (`src/ClientLibs/js/offs-client/`)

`OffsClient` (HTTP / WS / WT transports, auto-selected by URL scheme):

- `put(options, data)` → PUT `/offsystem` (headers `type`, `file-name`,
  `stream-length`, optional `server-address`, `recycler` JSON array,
  `temporary: true`, `tuple-size`); response body text = ORI string.
- `putStreamStart/putStreamData/putStreamEnd` (CBOR transports only).
- `get(ori, {onStart, onData, onEnd, onError}, range)` — parses
  `Content-Type`, `Content-Length`, `Content-Range`.
- `load(ori, {onProgress, onEnd, onError}, range)` — cache-load without file
  data. On HTTP transports streams the `?load=1` ndjson body; on CBOR
  transports uses LOAD_PROGRESS/LOAD_END frames (status is the string
  `"loaded"|"partial"|"failed"` on HTTP, numeric on CBOR —
  `wire.LOAD_STATUS` map: `loaded: 0, partial: 1, failed: 2`).
- `delete(offUrl)`.
- `blockPut(data, encoding)`, `blockGet(hash)`, `blockDelete(hash)`.
- `health()`, `peerInfo(format)` (format `'cbor'` (default) | `'base58'` |
  `'qrcode'` — PPM QR image), `peerConnect(peerInfo, format)`,
  `peerConnectQr(ppmBytes)` (shorthand for format 2), `peerList()`,
  `friendAdd`/`friendAddQr(ppmBytes)`/`friendRemove`/`friendList`,
  `configShow()`, `configSet(f,v)`, `configReload()`.
- `putFolder(items, options)`: recursive directory upload, builds OFD CBOR
  per directory, uploads directories as `offsystem/directory` named
  `<dir>.ofd`, returns root ORI. Progress callback
  `onProgress(name, uploaded, total)`.
- Static `OffsClient.offUrlToHttpUrl(ori, baseUrl)`.

---

## 7. Configuration fields (`GET/PUT /config`, CONFIG_SET)

Known fields (`src/Configuration/config_json.c:15-33`):

| Kind | Fields |
|---|---|
| string | `api_key_hash`, `https_cert_path`, `https_key_path`, `tcp_tls_cert_path`, `tcp_tls_key_path` |
| bool | `http_enabled`, `https_enabled`, `unix_enabled`, `tcp_enabled`, `ws_enabled`, `wt_enabled`, `tcp_tls_enabled`, `allow_secure`, `fsync_data` |
| number | `cache_size`, `max_snapshots`, `max_wals`, `max_capacity_bytes`, `scheduler_thread_count`, `http_port`, `https_port`, `tcp_port`, `ws_port`, `wt_port` |

Semantics: PUT stages changes to `{data_dir}/pending_config.json` and
returns which fields staged/rejected plus `restart_required`;
`POST /config/restart` applies pending config (409 if none). Config
mutations are refused (403) on non-loopback bindings regardless of auth.

Update status (wire 31/32) JSON:
`{"enabled": bool, "channel": str, "check_interval_hours": n, "state": str|"idle", "current_version": str, "available_version": str|"none"}`.
Channels: `stable|rc|dev`.

---

## 8. Security model

- HTTP admin endpoints (blocks, peers, friends, config): `Authorization:
  Bearer <api-key>`, bcrypt-checked server-side. 401 + `WWW-Authenticate:
  Bearer` if missing; 403 on wrong key.
- CBOR transports: send `[12, api_key: bstr]` as the first frame; all frames
  rejected with status 5 until auth succeeds.
- `/config` mutations additionally require a loopback transport.
- `allow_secure` (default false): encryption-only + reputation mode;
  when true, node certificates are validated against the CA (secure mode).

---

## 9. GUI feature checklist (derived from the above)

**Downloads**
- [ ] Accept an ORI/OFF URL string (parse + validate the 7 fields)
- [ ] Streamed download with progress (bytes received / `Content-Length` or range length)
- [ ] Range requests: `bytes=start-end`, `start-`, `-suffix` → resume, partial download
- [ ] 416 handling (`Content-Range: bytes */size`) → query size then retry
- [ ] Directory (OFD) URLs: parse `?ofd=raw` CBOR, list entries, download children,
      resolve `index.html` for `.ofd` names
- [ ] Error surface: 400 bad URL, 404 unresolved, socket statuses 1–5
- [ ] Known gap: GET of a missing block can hang (no wanted-list expiry — see
      `docs/PRODUCTION_BLOCKERS.md`); the GUI should implement its own timeout/cancel
- [x] Cache load / pin to node (pre-fetch a file's tuples into the daemon block
      cache without receiving file data) — implemented across all surfaces:
      HTTP `GET ...?load=1` (ndjson progress), wire frames 39–41, C
      `offs_client_load`, JS `load()`, Dart `loadContent`, CLI `offs load`

**Uploads**
- [ ] File upload with `type`, `file-name`, `stream-length` headers (or wire PUT flow)
- [ ] Optional: `server-address`, `recycler` (list of ORIs), `temporary`, `tuple-size`
- [ ] Streaming upload (chunked body / PUT_DATA frames) for large files
- [ ] Multipart form upload variant
- [ ] Display returned ORI; copy to clipboard
- [ ] Folder upload (recursive, build OFD per directory, upload as
      `offsystem/directory`) with per-file progress

**Blocks**
- [ ] Put block (raw or base58 encoding option)
- [ ] Get block (display hex/base58/raw)
- [ ] Delete block
- [ ] Defragment with threshold slider (0.0–1.0) and result counts

**Peers / friends**
- [x] Show my peer info (CBOR / base58 / QR code) — supported server-side:
      HTTP `?format=qrcode`, wire format 2, CLI `offs peer info --qr <file>|-`
- [x] Connect to peer (paste base58 or scan QR) — supported server-side:
      HTTP `Content-Type: image/x-portable-pixmap` body, wire format 2,
      CLI `offs peer connect --qr <file>` / `offs friend add --qr <file>`
- [ ] Peer list with connected / friend / in-ring state
- [ ] Friend add/remove/list

**Node management**
- [ ] Health dashboard (status, uptime, node_id, peer/connections, hebbian weight, block cache bytes/count, rate-limit stats, RPC counters)
- [ ] Config viewer/editor (§7 fields), staged/rejected feedback, restart prompt
- [ ] Daemon start/stop/restart; update status display
- [ ] Bearer API key management (bcrypt generate/check)

**Cross-cutting**
- [ ] Transport choice: HTTP REST and/or socket CBOR frames (unix/tcp/ws/wt)
- [ ] Auth: bearer header (HTTP) or AUTH_REQUEST first frame (sockets)
- [ ] ORI copy/paste everywhere (parse, render, validate base58 hashes)