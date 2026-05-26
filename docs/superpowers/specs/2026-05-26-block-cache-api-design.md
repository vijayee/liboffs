# Block Cache Client API Design

## Overview

Add direct block cache operations (GET/PUT/DELETE) to all Client API transports
(HTTP, TCP, WS, Unix, WT). Data up to 128KB is padded to the standard block size
and stored. The BLAKE3 hash is returned on PUT. These endpoints require
authentication and are disabled when auth is not configured.

## Wire Protocol (CBOR)

Six new message types for the CBOR transport layer. The hash field in
BLOCK_PUT_RESPONSE is polymorphic: raw bytes when encoding=0, text string when
encoding=1.

```
BLOCK_PUT_REQUEST       (13): [13, data: bstr, encoding: uint]
BLOCK_PUT_RESPONSE      (14): [14, status: uint, hash: bstr|tstr]
BLOCK_GET_REQUEST       (15): [15, hash: bstr]
BLOCK_GET_RESPONSE      (16): [16, status: uint, data: bstr]
BLOCK_DELETE_REQUEST    (17): [17, hash: bstr]
BLOCK_DELETE_RESPONSE   (18): [18, status: uint]
```

Encoding values: 0 = raw bytes, 1 = base58 text.

Status codes reuse the existing `CLIENT_API_STATUS_*` defines (OK=0, NOT_FOUND=2,
INTERNAL_ERROR=3, UNAUTHORIZED=5).

## HTTP API

| Method   | Path             | Body          | Response                          |
|----------|------------------|---------------|-----------------------------------|
| `PUT`    | `/blocks`        | raw data      | 201 + raw hash bytes              |
| `GET`    | `/blocks/<hash>` | none          | 200 + raw block bytes             |
| `DELETE` | `/blocks/<hash>` | none          | 204 no body                       |

- PUT query parameter `?encoding=base58` returns the hash as base58 text instead
  of raw bytes.
- `<hash>` in the URL path is base58-encoded BLAKE3 hash.
- GET returns the full padded block (128KB standard block size).
- 404 when the block is not found. 401/403 when unauthenticated.

## Authentication

Block routes are only registered when `config->api_key_hash != NULL`. When auth
is disabled, the endpoints are absent entirely.

For HTTP, the existing global auth middleware protects all registered routes, so
no per-route flag is needed. The block routes are registered after the auth
middleware in the chain.

For CBOR transports (TCP/WS/Unix/WT), each block handler checks
`conn->is_authenticated` and returns `CLIENT_API_STATUS_UNAUTHORIZED` if false,
matching the existing pattern in every transport's `_handle_get` / `_handle_put`.

## Block Operations

PUT: `block_create_by_type(data, standard)` pads data under 128KB with random
bytes, then `block_cache_put(bc, block, 0, reply_to)` stores it.

GET: `block_cache_get(bc, hash, reply_to)` retrieves the block. Returns the full
padded block data.

DELETE: `block_cache_remove(bc, hash, reply_to)` removes the block from cache
and index.

## Async Bridging

The block cache API is actor-based. Each HTTP handler spawns an actor state
machine: send the async cache request with `reply_to` set to the actor, then the
actor's dispatch function sends the HTTP response when the result arrives. This
is the same pattern used by the OFD directory GET handler.

For CBOR transports, the connection actor dispatches to the block handler, which
sends the async cache request with the connection's actor as the reply target.
The connection's dispatch function routes the cache result back to complete the
CBOR response.

## File Organization

- `src/ClientAPI/client_api_wire.h` / `.c` — 6 new encode/decode/destroy pairs
- `src/ClientAPI/HTTP/block_routes.h` / `.c` — HTTP route handlers, registered
  via `block_routes_register(server, pool, bc, config, api_key)`
- `src/ClientAPI/block_handlers.h` / `.c` — shared handler logic for CBOR
  transports (TCP/WS/Unix/WT), avoiding duplication across 4 connection files

## Base58 Encoding

The existing `base58_encode` / `base58_decode` in `src/Util/base58.h` is used.
BLAKE3 hashes are 32 bytes, encoding to ~45 base58 characters, well within the
existing 100-byte input limit.
