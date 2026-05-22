# TLS Certificate Validation — Design

Date: 2026-05-22

## Overview

Enable peer certificate validation on all QUIC connections when a CA certificate has been
loaded into `authority_t`. The CA certificate is stored as DER bytes in CBOR (application
managed, not user-editable config). When `authority->ca_cert_data` is non-NULL, peer
certificates are verified against it using OpenSSL. When no CA is loaded, the existing
unchecked behavior is preserved.

## Architecture

```
CLI tool (offs-cert)
  └── reads ca.pem, converts to DER, stores in CBOR file
        └── authority_t { ca_cert_data, ca_cert_len }
              └── network_t.authority
                    ├── quic_listener: validates peer certs against CA on incoming connections
                    ├── relay_client: validates relay server cert against CA on outgoing connections
                    └── relay_server: validates connecting peers against CA
```

## Component Design

### 1. authority_t Changes (`src/Network/authority.h/.c`)

Replace `char* ca_cert_path` with in-memory DER bytes:

```c
// Before:
char* ca_cert_path;

// After:
uint8_t* ca_cert_data;   // DER-encoded CA certificate (NULL if none)
size_t   ca_cert_len;    // Length of ca_cert_data
```

New functions:

```c
// Load a PEM-encoded CA certificate file, convert to DER, store in authority.
// This is called by an admin/tool before node startup.
// Returns 0 on success, -1 on failure.
int authority_load_ca_cert(authority_t* authority, const char* pem_path);
```

`authority_load_ca_cert` uses OpenSSL to read the PEM file and convert to DER:
- `BIO_new_file` → `PEM_read_bio_X509` → `i2d_X509` to get DER bytes
- Stores result in `authority->ca_cert_data` / `authority->ca_cert_len`
- The data persists via CBOR: `authority_save` writes it, `authority_load` reads it back

`authority_save` CBOR structure updated (v2). Positional arrays replace named-key maps
to prevent casual inspection and manipulation:

```
[
  uint8 (2),                        // version (index 0)
  bytes[32] (local_id),             // index 1
  bytes (DER-encoded CA cert),      // index 2 — empty byte string if none
  [ [bytes[32], float], ... ],      // hebbian (index 3)
  [ [bytes[32], uint32, uint16, float, float, float, uint8, float], ... ]  // peers (index 4)
]
```

Bump `PEER_STORE_VERSION` from 1 to 2. The v1 map format `{"local_id": ..., "hebbian": [...], ...}`
is still readable — `authority_load` detects map vs array to choose the decode path. When loading
a v2 array, a zero-length bytestring at index 2 means no CA cert.

`authority_load` reads index 2 and populates `ca_cert_data`/`ca_cert_len`.

`authority_destroy` frees `ca_cert_data`.

### 2. Peer Certificate Verifier (`src/Network/peer_verify.h/.c`)

New module that accepts DER bytes for the CA certificate:

```c
typedef struct peer_verify_ctx_t {
    X509_STORE* store;
} peer_verify_ctx_t;

// Create from DER-encoded CA certificate bytes. Returns NULL if data is NULL or parsing fails.
peer_verify_ctx_t* peer_verify_ctx_create(const uint8_t* ca_cert_data, size_t ca_cert_len);

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx);

// Verify a peer certificate against the CA store.
// certificate is a QUIC_CERTIFICATE* from MsQuic's CERTIFICATE_RECEIVED event.
// Returns 0 on success, -1 on failure.
int peer_verify_validate(peer_verify_ctx_t* ctx, void* certificate);
```

**Implementation:** `d2i_X509` to parse DER bytes, `X509_STORE_add_cert` to add to store,
`X509_STORE_CTX_new` + `X509_verify_cert` to validate peer cert against the store.

### 3. quic_listener.c Changes

- Store `peer_verify_ctx_t*` on `quic_listener_t`
- When `authority->ca_cert_data` is non-NULL:
  - Create `peer_verify_ctx_t` via `peer_verify_ctx_create(authority->ca_cert_data, authority->ca_cert_len)`
  - Set `cred_config.Flags = QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED`
    instead of `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`
- Handle `QUIC_LISTENER_EVENT_PEER_CERTIFICATE_RECEIVED`:
  call `peer_verify_validate(ctx, event->CERTIFICATE_RECEIVED.Certificate)`;
  if validation fails, abort the connection
- On destroy: call `peer_verify_ctx_destroy`

### 4. relay_client.c Changes

- Add `peer_verify_ctx_t*` to `relay_client_t`
- Add `network_t*` or `authority_t*` parameter to `relay_client_create`
- Create `peer_verify_ctx_t` from `network->authority->ca_cert_data`/`ca_cert_len` if available
- Same MsQuic flag and callback pattern as quic_listener

### 5. relay_server.c Changes

- Replace `ca_cert_path` field with `ca_cert_data`/`ca_cert_len` (or keep `peer_verify_ctx_t*`)
- Set from relay server configuration at creation time
- Same MsQuic flag and callback pattern

### 6. Unchanged Sites

- `offs_client.c` — Already correctly conditional on its existing `is_secure` flag
- `wt_transport.c` — Already uses `QUIC_CREDENTIAL_FLAG_NONE` (enables validation)
  when cert_path/key_path are provided

## Tooling (Separate Tickets)

- **OFFS-139**: CLI tool to generate CA cert + key, sign node certs, and store CA cert as
  DER in the CBOR peer store
- **OFFS-140**: CA signing server for automated certificate enrollment

## Data Flow

```
1. Admin runs: offs-cert init --ca ca.pem --out peers.cbor
   → converts ca.pem to DER, stores in CBOR

2. Node starts: authority_load(peer_store_path)
   → reads ca_cert bytes from CBOR into authority->ca_cert_data

3. network_create → quic_listener/relay_client/relay_server
   → peer_verify_ctx_create(authority->ca_cert_data, authority->ca_cert_len)
   → credential config uses INDICATE_CERTIFICATE_RECEIVED flag
   → peer certs validated against CA on every connection
```

## Error Handling

| Scenario | Behavior |
|----------|----------|
| ca_cert_data is NULL | peer_verify_ctx_create returns NULL, credential config uses NO_CERTIFICATE_VALIDATION flag (existing behavior) |
| ca_cert_data is corrupt/invalid DER | peer_verify_ctx_create logs error and returns NULL, falls back to unchecked |
| Peer cert fails validation | peer_verify_validate returns -1, connection is rejected with error log |
| CBOR store has empty ca_cert bytestring | authority_load leaves ca_cert_data NULL, unchecked behavior |

## Testing

- Unit test `peer_verify_validate` with a test CA cert + signed node cert generated via OpenSSL CLI
- Test that valid peer cert passes, cert from different CA fails
- Test NULL/empty ca_cert_data → fallback to unchecked
- Cert data persistence: save via authority_save, reload via authority_load, verify CA cert bytes match
- Integration test: QUIC connection with cert validation enabled
