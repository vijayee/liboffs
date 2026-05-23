# Authentication Framework — Design

Date: 2026-05-23

## Overview

Add a shared-secret API key authentication layer to the HTTP server and client API
wire protocol. Auth is optional (disabled when no key is configured), backward
compatible, and designed for future per-block permission extensions.

## Configuration

`config_t` gains fields for enabling each client API, their ports, and auth:

```c
/* REST API */
bool     http_enabled;
uint16_t http_port;           // default 80
bool     https_enabled;
uint16_t https_port;          // default 443
char*    https_cert_path;
char*    https_key_path;

/* Wire protocol transports */
bool     unix_enabled;        // Unix domain socket (local-only)
bool     tcp_enabled;
uint16_t tcp_port;
bool     ws_enabled;          // WebSocket
uint16_t ws_port;
bool     wt_enabled;          // WebTransport (QUIC, always encrypted)
uint16_t wt_port;

/* Auth (applies to all enabled APIs) */
char*    api_key_hash;        // bcrypt hash, NULL = auth disabled
```

`config_validate()` checks:

- `https_enabled` → `https_cert_path` and `https_key_path` must be non-NULL
- `api_key_hash` non-NULL → must start with `$2b$` and be exactly 60 characters
- `api_key_hash` non-NULL with `tcp_enabled` or `ws_enabled` → reject (API keys
  over plaintext remote transports)
- At least one API enabled → otherwise it's a headless P2P-only node (also valid)

`authority_t` is unchanged — API auth is unrelated to P2P identity.

The embedding application sources the plaintext API key from its own mechanism (CLI
argument, secure store, etc.) and passes it to the auth middleware. The library never
reads environment variables.

## Deployment Model

The HTTP server is single-socket, all-or-nothing TLS (`SSL_CTX*` applies to every
accepted connection). An application that wants both encrypted and unencrypted access
creates two server instances on different ports:

- **TLS server** (e.g. port 443): auth middleware registered → all requests require
  `Authorization: Bearer <key>`
- **Plaintext server** (e.g. port 80): no auth middleware → open access, should be
  bound to loopback (`127.0.0.1`)

Ports and enabled flags are in `config_t`. The embedding application reads config
to decide which servers to create, e.g.:
```c
if (config->https_enabled) {
    server = http_server_create_ssl(pool, "0.0.0.0", config->https_port,
                                    config->https_cert_path, config->https_key_path);
}
```

## HTTP Auth Middleware

**File:** `src/ClientAPI/HTTP/auth.h`

```c
typedef struct auth_middleware_t auth_middleware_t;

auth_middleware_t* auth_middleware_create(const char* api_key, const char* bcrypt_hash);
void auth_middleware_destroy(auth_middleware_t* auth);
```

The middleware extracts the `Authorization` header, validates it is `Bearer <key>`,
and checks the key against the bcrypt hash via `bcrypt_check()`.

- Missing / malformed header → `401 Unauthorized` with `WWW-Authenticate: Bearer`
- Invalid key → `403 Forbidden`
- Valid key → sets `request->is_authenticated = true`, continues chain

The middleware does **not** check whether the connection is TLS or localhost — those
are deployment decisions (which server instance gets the middleware registered).

### Request metadata

`http_request_t` gains:

```c
bool is_authenticated;  // set true by auth middleware on successful validation
```

This is the hook for future per-block permission middleware.

### Registration (in the embedding application)

```c
if (config->api_key_hash) {
    auth_middleware_t* auth = auth_middleware_create(api_key, config->api_key_hash);
    http_server_use(tls_server, auth_middleware_handler(), auth, auth_middleware_destroy);
}
```

## Client API Wire Protocol

**File:** `src/ClientAPI/client_api_wire.h`

New message type:

```c
#define CLIENT_API_AUTH 12
```

New status code:

```c
#define CLIENT_API_STATUS_UNAUTHORIZED 5
```

### Wire format

CBOR array: `[12, bytestring(api_key)]`

### Protocol rules

- AUTH must be the **first message** sent after connect
- Any other message before AUTH receives `CLIENT_API_ERROR` with status
  `CLIENT_API_STATUS_UNAUTHORIZED` and the connection is closed
- On success, the connection is marked authenticated
- Same deployment pattern as HTTP: TLS transports register the AUTH requirement;
  plaintext transports bound to loopback skip it

### Encoder/decoder

New functions in `client_api_wire.c`:

```c
void client_api_auth_encode(cbor_encoder_t* encoder, const uint8_t* key, size_t key_len);
int client_api_auth_decode(cbor_decoder_t* decoder, uint8_t** key_out, size_t* key_len_out);
```

## Dependencies

- `bcrypt` — a portable implementation of the OpenBSD bcrypt algorithm for
  `bcrypt_check()`. The `$2b$` prefix variant is required.
- No new dependencies for the wire protocol (CBOR encoder/decoder already exists)

## Future: Per-Block Permissions

The `is_authenticated` flag on `http_request_t` and the connection-level authenticated
state in the wire protocol provide the foundation. Future work would add:

- A `CLIENT_API_PERMISSION_REQUEST` message type carrying requested operation
  (store/retrieve/delete) and ORI
- Server-side ACL store mapping identities to permitted operations on blocks
- Permission middleware that checks `is_authenticated` first, then validates
  the specific operation against the ACL

## Backward Compatibility

- `config->api_key_hash == NULL` → no auth middleware registered, no AUTH required
  on wire connections. Existing behavior unchanged.
- New status code `5` (unauthorized) is distinct from existing codes (0-4), so
  existing clients won't misinterpret it.
