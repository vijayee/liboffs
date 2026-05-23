# Authentication Framework — Design

Date: 2026-05-23

## Overview

Add a shared-secret API key authentication layer to the HTTP server and client API
wire protocol. Auth is optional (disabled when no key is configured), backward
compatible, and designed for future per-block permission extensions.

## Configuration

`config_t` gains one field:

```c
char* api_key_hash;  // bcrypt hash ($2b$ prefix), NULL if auth disabled
```

`config_validate()` checks: if `api_key_hash` is non-NULL, it must start with `$2b$`
and be exactly 60 characters.

`authority_t` is unchanged — API auth is unrelated to P2P identity.

## HTTP Auth Middleware

**File:** `src/ClientAPI/HTTP/auth.h`

```c
typedef struct auth_middleware_t auth_middleware_t;

auth_middleware_t* auth_middleware_create(const char* api_key, const char* bcrypt_hash);
void auth_middleware_destroy(auth_middleware_t* auth);
```

The middleware checks `Authorization: Bearer <key>` on every request.

### Three-tier enforcement

| Connection | Behavior |
|------------|----------|
| Remote + TLS | Validate Bearer token against bcrypt hash |
| Localhost (127.0.0.1, ::1) | Pass through, mark unauthenticated |
| Remote + plaintext | Reject `403 TLS required` |

### Request metadata

`http_request_t` gains:

```c
bool is_authenticated;  // set true by auth middleware on successful validation
bool is_tls;            // set by connection layer based on SSL context
```

`is_authenticated` is the hook for future per-block permission middleware.

### Registration

In `off_routes.c`, the auth middleware is registered only when
`config->api_key_hash != NULL`:

```c
if (config->api_key_hash) {
    auth_middleware_t* auth = auth_middleware_create(api_key, config->api_key_hash);
    http_server_use(server, auth_middleware_handler(), auth, auth_middleware_destroy);
}
```

The embedding application sources `api_key` from its own mechanism (CLI argument,
secure store, etc.) — the library does not read environment variables.

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
- Same localhost exemption: loopback transports skip AUTH requirement
- Same TLS requirement: remote plaintext transports reject AUTH attempts

### Encoder/decoder

New functions in `client_api_wire.c`:

```c
void client_api_auth_encode(cbor_encoder_t* encoder, const uint8_t* key, size_t key_len);
int client_api_auth_decode(cbor_decoder_t* decoder, uint8_t** key_out, size_t* key_len_out);
```

## Dependencies

- `bcrypt` / `crypt_blowfish` for `bcrypt_check()` — POSIX `crypt()` with `$2b$` prefix,
  or a lightweight embedded implementation (e.g., OpenBSD-style bcrypt)
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
