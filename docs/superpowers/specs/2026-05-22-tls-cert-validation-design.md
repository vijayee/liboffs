# TLS Certificate Validation — Design

Date: 2026-05-22

## Overview

Enable peer certificate validation on all QUIC connections when a CA certificate is
configured in `authority_t`. When `authority->ca_cert_path` is set, peer certificates
are verified against that CA using OpenSSL. When no CA is configured, the existing
unchecked behavior is preserved.

## Architecture

```
config.yaml/app
  └── config_t
        └── authority_t.ca_cert_path = "/etc/offs/ca.pem"
              └── network_t.authority
                    ├── quic_listener: validates peer certs against CA on incoming connections
                    ├── relay_client: validates relay server cert against CA on outgoing connections
                    └── relay_server: validates connecting peers against CA
```

## Component Design

### 1. Peer Certificate Verifier (`src/Network/peer_verify.h/.c`)

New module that loads a CA certificate into an OpenSSL `X509_STORE` and provides a
verification callback for MsQuic peer certificate events.

```c
typedef struct peer_verify_ctx_t {
    X509_STORE* store;
} peer_verify_ctx_t;

// Load CA certificate from file. Returns NULL if path is NULL or loading fails.
peer_verify_ctx_t* peer_verify_ctx_create(const char* ca_cert_path);

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx);

// Verify a peer certificate against the CA store.
// certificate is a QUIC_CERTIFICATE* from MsQuic's CERTIFICATE_RECEIVED event.
// Returns 0 on success, -1 on failure.
int peer_verify_validate(peer_verify_ctx_t* ctx, void* certificate);
```

**Dependencies:** libssl (OpenSSL), which is already linked via `ssl crypto` in CMake.

**Error handling:** If `ca_cert_path` is NULL or the file cannot be loaded, `peer_verify_ctx_create`
returns NULL. Callers fall back to unchecked behavior.

### 2. quic_listener.c Changes

- Store `peer_verify_ctx_t*` on `quic_listener_t`
- When `authority->ca_cert_path` is set:
  - Create `peer_verify_ctx_t` via `peer_verify_ctx_create(authority->ca_cert_path)`
  - Set `cred_config.Flags = QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED`
    instead of `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`
- Handle `QUIC_LISTENER_EVENT_PEER_CERTIFICATE_RECEIVED` in the callback:
  call `peer_verify_validate(ctx, event->CERTIFICATE_RECEIVED.Certificate)`;
  if validation fails, abort the connection
- On destroy: call `peer_verify_ctx_destroy`

### 3. relay_client.c Changes

- Add `peer_verify_ctx_t*` to `relay_client_t`
- Add `ca_cert_path` parameter to `relay_client_create` (can be NULL)
- Caller (`network_connect_relay`) passes `network->authority->ca_cert_path`
- Same MsQuic flag and callback pattern as quic_listener

### 4. relay_server.c Changes

- Add `ca_cert_path` field to `relay_server_t`
- Set from configuration when creating the relay server
- Same MsQuic flag and callback pattern

### 5. Unchanged Sites

- `offs_client.c` — Already correctly conditional on its existing `is_secure` flag
- `wt_transport.c` — Already uses `QUIC_CREDENTIAL_FLAG_NONE` (enables validation)
  when cert_path/key_path are provided

## Config Flow

```
authority_t { ca_cert_path: "/etc/offs/ca.pem", ... }
  → network_create(config) stores config pointer
    → quic_listener_create uses network->authority->ca_cert_path
    → network_connect_relay uses network->authority->ca_cert_path
  → relay_server_create receives ca_cert_path from config
```

## Error Handling

| Scenario | Behavior |
|----------|----------|
| ca_cert_path is NULL | peer_verify_ctx_create returns NULL, credential config uses NO_CERTIFICATE_VALIDATION flag (existing behavior) |
| CA file missing/corrupt | peer_verify_ctx_create logs error and returns NULL, falls back to unchecked |
| Peer cert fails validation | peer_verify_validate returns -1, connection is rejected with error log |
| No authority available (relay_server standalone) | ca_cert_path passed explicitly, NULL means unchecked |

## Testing

- Unit test `peer_verify_validate` with a test CA cert + signed node cert generated via OpenSSL CLI
- Integration test: self-signed CA cert → accept valid peer, reject peer from different CA
- Test fallback: NULL ca_cert_path → unchecked connections work as before
- Generate test certs via OpenSSL in test setup:
  ```
  openssl req -x509 -newkey rsa:2048 -keyout ca_key.pem -out ca.pem -days 365 -nodes -subj "/CN=TestCA"
  openssl req -newkey rsa:2048 -keyout node_key.pem -out node.csr -nodes -subj "/CN=TestNode"
  openssl x509 -req -in node.csr -CA ca.pem -CAkey ca_key.pem -CAcreateserial -out node_cert.pem -days 365
  ```
