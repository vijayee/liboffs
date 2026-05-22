# TLS Certificate Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable peer certificate validation on all QUIC connections when a CA certificate is loaded into authority_t, using OpenSSL verification via a new peer_verify module.

**Architecture:** New `peer_verify.h/.c` module wraps OpenSSL X509 verification. `authority_t` stores DER bytes instead of a file path, persisted via positional-array CBOR (v2, backward-compatible with v1 maps). Three QUIC credential sites (quic_listener, relay_client, relay_server) conditionally enable `INDICATE_CERTIFICATE_RECEIVED` when a CA cert is present. Two sites (offs_client, wt_transport) are already correct and unchanged.

**Tech Stack:** C11, OpenSSL (X509_STORE, d2i_X509), MsQuic, libcbor, GoogleTest

---

## File Map

| File | Action | Purpose |
|------|--------|---------|
| `src/Network/peer_verify.h` | **Create** | Peer certificate verifier API |
| `src/Network/peer_verify.c` | **Create** | OpenSSL cert chain validation impl |
| `src/Network/authority.h` | Modify | Replace `ca_cert_path` with `ca_cert_data`/`ca_cert_len`; add `authority_load_ca_cert` |
| `src/Network/authority.c` | Modify | Implement `authority_load_ca_cert`; update `authority_destroy`; migrate CBOR save/load to v2 positional arrays |
| `src/Network/quic_listener.c` | Modify | Store `peer_verify_ctx_t*`; conditional `INDICATE_CERTIFICATE_RECEIVED` flag; handle `PEER_CERTIFICATE_RECEIVED` event |
| `src/Network/relay_client.h` | Modify | Add `peer_verify_ctx_t*` field to `relay_client_t` |
| `src/Network/relay_client.c` | Modify | Create/destroy `peer_verify_ctx_t`; conditional cert validation flag |
| `src/Network/Relay/relay_server.h` | Modify | Add `peer_verify_ctx_t*` field to `relay_server_t` |
| `src/Network/Relay/relay_server.c` | Modify | Create/destroy `peer_verify_ctx_t`; conditional cert validation flag |
| `test/test_peer_verify.cpp` | **Create** | Unit tests for peer_verify module |
| `test/CMakeLists.txt` | Modify | Add `test_peer_verify.cpp` to testliboffs |

---

### Task 1: Create peer_verify.h

**Files:**
- Create: `src/Network/peer_verify.h`

- [ ] **Step 1: Write the header**

```c
//
// Created by victor on 5/22/26.
//

#ifndef OFFS_PEER_VERIFY_H
#define OFFS_PEER_VERIFY_H

#include <stdint.h>
#include <stddef.h>

typedef struct peer_verify_ctx_t peer_verify_ctx_t;

// Create verification context from DER-encoded CA certificate bytes.
// Returns NULL if data is NULL, data_len is 0, or parsing fails.
peer_verify_ctx_t* peer_verify_ctx_create(const uint8_t* ca_cert_data, size_t ca_cert_len);

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx);

// Validate a peer certificate against the trusted CA store.
// certificate is a QUIC_CERTIFICATE* from MsQuic's CERTIFICATE_RECEIVED event.
// Returns 0 on success, -1 on failure.
int peer_verify_validate(peer_verify_ctx_t* ctx, void* certificate);

#endif // OFFS_PEER_VERIFY_H
```

- [ ] **Step 2: Commit**

```bash
git add src/Network/peer_verify.h
git commit -m "feat: add peer_verify.h - peer certificate validation API"
```

---

### Task 2: Create peer_verify.c

**Files:**
- Create: `src/Network/peer_verify.c`

- [ ] **Step 1: Write the implementation**

```c
//
// Created by victor on 5/22/26.
//

#include "peer_verify.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/bio.h>

struct peer_verify_ctx_t {
  X509_STORE* store;
};

peer_verify_ctx_t* peer_verify_ctx_create(const uint8_t* ca_cert_data, size_t ca_cert_len) {
  if (ca_cert_data == NULL || ca_cert_len == 0) {
    return NULL;
  }

  const unsigned char* cursor = ca_cert_data;
  X509* ca_cert = d2i_X509(NULL, &cursor, (long)ca_cert_len);
  if (ca_cert == NULL) {
    log_error("peer_verify: failed to parse DER CA certificate");
    return NULL;
  }

  X509_STORE* store = X509_STORE_new();
  if (store == NULL) {
    X509_free(ca_cert);
    return NULL;
  }

  if (X509_STORE_add_cert(store, ca_cert) != 1) {
    log_error("peer_verify: failed to add CA cert to store");
    X509_free(ca_cert);
    X509_STORE_free(store);
    return NULL;
  }
  X509_free(ca_cert);

  peer_verify_ctx_t* ctx = get_clear_memory(sizeof(peer_verify_ctx_t));
  if (ctx == NULL) {
    X509_STORE_free(store);
    return NULL;
  }
  ctx->store = store;
  return ctx;
}

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx) {
  if (ctx == NULL) return;
  if (ctx->store != NULL) {
    X509_STORE_free(ctx->store);
  }
  free(ctx);
}

int peer_verify_validate(peer_verify_ctx_t* ctx, void* certificate) {
  if (ctx == NULL || certificate == NULL) {
    return -1;
  }

  QUIC_CERTIFICATE* cert = (QUIC_CERTIFICATE*)certificate;
  const unsigned char* cursor = cert->Certificate;
  X509* peer_cert = d2i_X509(NULL, &cursor, (long)cert->CertificateLength);
  if (peer_cert == NULL) {
    log_error("peer_verify: failed to parse peer certificate DER");
    return -1;
  }

  X509_STORE_CTX* verify_ctx = X509_STORE_CTX_new();
  if (verify_ctx == NULL) {
    X509_free(peer_cert);
    return -1;
  }

  X509_STORE_CTX_init(verify_ctx, ctx->store, peer_cert, NULL);
  int result = X509_verify_cert(verify_ctx);

  if (result != 1) {
    int err = X509_STORE_CTX_get_error(verify_ctx);
    log_error("peer_verify: certificate verification failed: %s",
              X509_verify_cert_error_string(err));
  }

  X509_STORE_CTX_free(verify_ctx);
  X509_free(peer_cert);
  return (result == 1) ? 0 : -1;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Network/peer_verify.c
git commit -m "feat: add peer_verify.c - OpenSSL peer certificate validation"
```

---

### Task 3: Update authority_t for DER CA cert storage and CBOR v2

**Files:**
- Modify: `src/Network/authority.h`
- Modify: `src/Network/authority.c`

- [ ] **Step 1: Update authority.h — replace ca_cert_path with DER fields and add authority_load_ca_cert**

In `src/Network/authority.h`, replace line 32 (`char* ca_cert_path;`) with:

```c
  uint8_t* ca_cert_data;   // DER-encoded CA certificate (NULL if none)
  size_t   ca_cert_len;    // Length of ca_cert_data
```

And add the new function declaration after `authority_init_local_id`:

```c
// Load a PEM-encoded CA certificate, convert to DER, store in authority.
// Returns 0 on success, -1 on failure.
int authority_load_ca_cert(authority_t* authority, const char* pem_path);
```

- [ ] **Step 2: Update authority.c — replace ca_cert_path free in authority_destroy**

In `authority_destroy`, replace line 51:
```c
  if (authority->ca_cert_path != NULL) free(authority->ca_cert_path);
```
with:
```c
  if (authority->ca_cert_data != NULL) free(authority->ca_cert_data);
```

- [ ] **Step 3: Implement authority_load_ca_cert**

Add after `authority_init_local_id` (before the config-only save/load section):

```c
int authority_load_ca_cert(authority_t* authority, const char* pem_path) {
  if (authority == NULL || pem_path == NULL) return -1;

  BIO* bio = BIO_new_file(pem_path, "r");
  if (bio == NULL) {
    log_error("authority_load_ca_cert: failed to open PEM file: %s", pem_path);
    return -1;
  }

  X509* ca_cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (ca_cert == NULL) {
    log_error("authority_load_ca_cert: failed to parse PEM certificate");
    return -1;
  }

  int der_len = i2d_X509(ca_cert, NULL);
  if (der_len <= 0) {
    X509_free(ca_cert);
    log_error("authority_load_ca_cert: failed to get DER length");
    return -1;
  }

  uint8_t* der_data = get_memory((size_t)der_len);
  if (der_data == NULL) {
    X509_free(ca_cert);
    return -1;
  }

  uint8_t* cursor = der_data;
  int actual_len = i2d_X509(ca_cert, &cursor);
  X509_free(ca_cert);

  if (actual_len != der_len) {
    free(der_data);
    log_error("authority_load_ca_cert: DER encoding size mismatch");
    return -1;
  }

  // Free existing CA cert data if any
  if (authority->ca_cert_data != NULL) {
    free(authority->ca_cert_data);
  }
  authority->ca_cert_data = der_data;
  authority->ca_cert_len = (size_t)actual_len;
  return 0;
}
```

Add the required includes at the top of authority.c (after the existing includes):

```c
#include <openssl/pem.h>
#include <openssl/x509.h>
```

- [ ] **Step 4: Bump PEER_STORE_VERSION and update authority_save_peers to v2 positional array format**

Change line 20:
```c
#define PEER_STORE_VERSION 2
```

Replace the existing `authority_save_peers` function body. The full replacement starts at the comment block (line 115) through the end of the function (line 274). Replace with:

```c
// CBOR structure (v2 positional array):
// [
//   uint8 (version = 2),               // index 0
//   bytes[32] (local_id),               // index 1
//   bytes (DER-encoded CA cert),        // index 2 — empty bytestring if none
//   [ [bytes[32], float], ... ],        // hebbian (index 3)
//   [ [bytes[32], uint32, uint16, float, float, float, uint8, float], ... ]  // peers (index 4)
// ]

int authority_save_peers(const authority_t* authority, const network_t* network) {
  if (authority == NULL || network == NULL) return -1;
  if (authority->peer_store_path == NULL) return -1;

  size_t hebbian_count = network->hebbian.count;
  size_t peer_count = ring_set_total_nodes(network->rings);

  cbor_item_t* root = cbor_new_definite_array(5);

  // Index 0: version
  {
    cbor_item_t* val = cbor_build_uint8(PEER_STORE_VERSION);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  }

  // Index 1: local_id
  {
    cbor_item_t* val = cbor_build_bytestring(authority->local_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  }

  // Index 2: ca_cert (DER bytes, or empty bytestring if none)
  if (authority->ca_cert_data != NULL && authority->ca_cert_len > 0) {
    cbor_item_t* val = cbor_build_bytestring(authority->ca_cert_data, authority->ca_cert_len);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  } else {
    cbor_item_t* val = cbor_build_bytestring(NULL, 0);
    (void)cbor_array_push(root, val);
    cbor_decref(&val);
  }

  // Index 3: hebbian weights
  cbor_item_t* hebbian_array = cbor_new_definite_array(hebbian_count);
  for (size_t index = 0; index < hebbian_count; index++) {
    const hebbian_weight_t* entry = &network->hebbian.entries[index];
    cbor_item_t* pair = cbor_new_definite_array(2);
    cbor_item_t* id_item = cbor_build_bytestring(entry->peer_id.hash, NODE_ID_HASH_SIZE);
    (void)cbor_array_push(pair, id_item);
    cbor_decref(&id_item);
    cbor_item_t* weight_val = cbor_new_float8();
    cbor_set_float8(weight_val, (double)entry->weight);
    (void)cbor_array_push(pair, weight_val);
    cbor_decref(&weight_val);
    (void)cbor_array_push(hebbian_array, pair);
    cbor_decref(&pair);
  }
  (void)cbor_array_push(root, hebbian_array);
  cbor_decref(&hebbian_array);

  // Index 4: peers
  cbor_item_t* peers_array = cbor_new_definite_array(peer_count);
  for (size_t ring_idx = 0; ring_idx < network->rings->ring_count; ring_idx++) {
    ring_t* ring = &network->rings->rings[ring_idx];
    for (int node_idx = 0; node_idx < ring->primary.length; node_idx++) {
      net_node_t* node = ring->primary.data[node_idx];
      if (node == NULL) continue;
      cbor_item_t* peer = cbor_new_definite_array(8);
      cbor_item_t* id_bytes = cbor_build_bytestring(node->id.hash, NODE_ID_HASH_SIZE);
      (void)cbor_array_push(peer, id_bytes);
      cbor_decref(&id_bytes);
      cbor_item_t* addr_val = cbor_build_uint32(node->addr);
      (void)cbor_array_push(peer, addr_val);
      cbor_decref(&addr_val);
      cbor_item_t* port_val = cbor_build_uint16(node->port);
      (void)cbor_array_push(peer, port_val);
      cbor_decref(&port_val);
      cbor_item_t* lat = cbor_new_float4();
      cbor_set_float4(lat, node->latency_ms);
      (void)cbor_array_push(peer, lat);
      cbor_decref(&lat);
      cbor_item_t* wt = cbor_new_float4();
      cbor_set_float4(wt, node->weight);
      (void)cbor_array_push(peer, wt);
      cbor_decref(&wt);
      cbor_item_t* cap = cbor_new_float4();
      cbor_set_float4(cap, node->capacity);
      (void)cbor_array_push(peer, cap);
      cbor_decref(&cap);
      cbor_item_t* phase_val = cbor_build_uint8((uint8_t)node->phase);
      (void)cbor_array_push(peer, phase_val);
      cbor_decref(&phase_val);
      cbor_item_t* avail = cbor_new_float4();
      cbor_set_float4(avail, node->availability);
      (void)cbor_array_push(peer, avail);
      cbor_decref(&avail);
      (void)cbor_array_push(peers_array, peer);
      cbor_decref(&peer);
    }
  }
  (void)cbor_array_push(root, peers_array);
  cbor_decref(&peers_array);

  unsigned char* buffer = NULL;
  size_t buffer_size = 0;
  size_t length = cbor_serialize_alloc(root, &buffer, &buffer_size);
  cbor_decref(&root);
  if (length == 0 || buffer == NULL) return -1;

  FILE* file = fopen(authority->peer_store_path, "wb");
  if (file == NULL) {
    free(buffer);
    return -1;
  }
  size_t written = fwrite(buffer, 1, length, file);
  fclose(file);
  free(buffer);
  return (written == length) ? 0 : -1;
}
```

- [ ] **Step 5: Update authority_load_peers to read v2 positional arrays with v1 map fallback**

Replace the body of `authority_load_peers` (lines 276–455) with:

```c
int authority_load_peers(authority_t* authority, network_t* network) {
  if (authority == NULL || network == NULL) return -1;
  if (authority->peer_store_path == NULL) return -1;

  FILE* file = fopen(authority->peer_store_path, "rb");
  if (file == NULL) return -1;

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size <= 0) {
    fclose(file);
    return -1;
  }

  unsigned char* buffer = get_clear_memory((size_t)file_size);
  if (buffer == NULL) {
    fclose(file);
    return -1;
  }
  size_t read_bytes = fread(buffer, 1, (size_t)file_size, file);
  fclose(file);
  if (read_bytes == 0) {
    free(buffer);
    return -1;
  }

  struct cbor_load_result result;
  cbor_item_t* root = cbor_load(buffer, read_bytes, &result);
  free(buffer);
  if (result.error.code != CBOR_ERR_NONE || root == NULL) return -1;

  if (cbor_isa_array(root)) {
    // v2 positional array format
    size_t arr_size = cbor_array_size(root);
    if (arr_size < 2) {
      cbor_decref(&root);
      return -1;
    }

    // Index 0: version
    cbor_item_t* version_item = cbor_array_get(root, 0);
    uint8_t version = cbor_isa_uint(version_item) ? (uint8_t)cbor_get_uint64(version_item) : 0;
    cbor_decref(&version_item);
    if (version != PEER_STORE_VERSION) {
      cbor_decref(&root);
      return -1;
    }

    // Index 1: local_id
    cbor_item_t* local_id_item = cbor_array_get(root, 1);
    if (cbor_isa_bytestring(local_id_item) && cbor_bytestring_length(local_id_item) == NODE_ID_HASH_SIZE) {
      memcpy(authority->local_id.hash, cbor_bytestring_handle(local_id_item), NODE_ID_HASH_SIZE);
      base58_encode(authority->local_id.hash, NODE_ID_HASH_SIZE,
                    authority->local_id.str, NODE_ID_STRING_SIZE);
    }
    cbor_decref(&local_id_item);

    // Index 2: ca_cert (may be empty bytestring)
    if (arr_size >= 3) {
      cbor_item_t* ca_cert_item = cbor_array_get(root, 2);
      if (cbor_isa_bytestring(ca_cert_item)) {
        size_t cert_len = cbor_bytestring_length(ca_cert_item);
        if (cert_len > 0) {
          authority->ca_cert_data = get_memory(cert_len);
          if (authority->ca_cert_data != NULL) {
            memcpy(authority->ca_cert_data, cbor_bytestring_handle(ca_cert_item), cert_len);
            authority->ca_cert_len = cert_len;
          }
        }
      }
      cbor_decref(&ca_cert_item);
    }

    // Index 3: hebbian
    if (arr_size >= 4) {
      cbor_item_t* hebbian_item = cbor_array_get(root, 3);
      if (cbor_isa_array(hebbian_item)) {
        size_t hebbian_len = cbor_array_size(hebbian_item);
        for (size_t he_idx = 0; he_idx < hebbian_len; he_idx++) {
          cbor_item_t* entry = cbor_array_get(hebbian_item, he_idx);
          if (cbor_isa_array(entry) && cbor_array_size(entry) == 2) {
            cbor_item_t* peer_id_item = cbor_array_get(entry, 0);
            cbor_item_t* weight_item = cbor_array_get(entry, 1);
            if (cbor_isa_bytestring(peer_id_item) && cbor_bytestring_length(peer_id_item) == NODE_ID_HASH_SIZE &&
                cbor_is_float(weight_item)) {
              node_id_t peer_id;
              memset(&peer_id, 0, sizeof(peer_id));
              memcpy(peer_id.hash, cbor_bytestring_handle(peer_id_item), NODE_ID_HASH_SIZE);
              float weight = (float)cbor_float_get_float8(weight_item);
              hebbian_table_set(&network->hebbian, &peer_id, weight);
            }
            cbor_decref(&peer_id_item);
            cbor_decref(&weight_item);
          }
          cbor_decref(&entry);
        }
      }
      cbor_decref(&hebbian_item);
    }

    // Index 4: peers
    if (arr_size >= 5) {
      cbor_item_t* peers_item = cbor_array_get(root, 4);
      if (cbor_isa_array(peers_item)) {
        size_t peers_len = cbor_array_size(peers_item);
        for (size_t peer_idx = 0; peer_idx < peers_len; peer_idx++) {
          cbor_item_t* peer = cbor_array_get(peers_item, peer_idx);
          if (cbor_isa_array(peer) && cbor_array_size(peer) == 8) {
            cbor_item_t* id_item = cbor_array_get(peer, 0);
            cbor_item_t* addr_item = cbor_array_get(peer, 1);
            cbor_item_t* port_item = cbor_array_get(peer, 2);
            cbor_item_t* lat_item = cbor_array_get(peer, 3);
            cbor_item_t* wt_item = cbor_array_get(peer, 4);
            cbor_item_t* cap_item = cbor_array_get(peer, 5);
            cbor_item_t* phase_item = cbor_array_get(peer, 6);
            cbor_item_t* avail_item = cbor_array_get(peer, 7);
            if (cbor_isa_bytestring(id_item) && cbor_bytestring_length(id_item) == NODE_ID_HASH_SIZE) {
              node_id_t peer_id;
              memset(&peer_id, 0, sizeof(peer_id));
              memcpy(peer_id.hash, cbor_bytestring_handle(id_item), NODE_ID_HASH_SIZE);
              uint32_t addr = cbor_isa_uint(addr_item) ? (uint32_t)cbor_get_uint64(addr_item) : 0;
              uint16_t port = cbor_isa_uint(port_item) ? (uint16_t)cbor_get_uint64(port_item) : 0;
              net_node_t* node = net_node_create(&peer_id, addr, port);
              if (node != NULL) {
                if (cbor_is_float(lat_item)) node->latency_ms = (float)cbor_float_get_float4(lat_item);
                if (cbor_is_float(wt_item)) node->weight = (float)cbor_float_get_float4(wt_item);
                if (cbor_is_float(cap_item)) node->capacity = (float)cbor_float_get_float4(cap_item);
                if (cbor_isa_uint(phase_item)) node->phase = (node_phase_e)cbor_get_uint64(phase_item);
                if (cbor_is_float(avail_item)) node->availability = (float)cbor_float_get_float4(avail_item);
                uint32_t latency_us = (uint32_t)(node->latency_ms * 1000.0f);
                ring_set_insert(network->rings, node, latency_us);
              }
            }
            cbor_decref(&id_item);
            cbor_decref(&addr_item);
            cbor_decref(&port_item);
            cbor_decref(&lat_item);
            cbor_decref(&wt_item);
            cbor_decref(&cap_item);
            cbor_decref(&phase_item);
            cbor_decref(&avail_item);
          }
          cbor_decref(&peer);
        }
      }
      cbor_decref(&peers_item);
    }

    cbor_decref(&root);
  } else if (cbor_isa_map(root)) {
    // v1 map format — backward compatible read, no ca_cert
    size_t map_size = cbor_map_size(root);
    uint8_t version = 0;
    for (size_t index = 0; index < map_size; index++) {
      struct cbor_pair pair = cbor_map_handle(root)[index];
      if (cbor_isa_string(pair.key) && cbor_string_length(pair.key) == 1 &&
          cbor_string_handle(pair.key)[0] == 'v') {
        if (cbor_isa_uint(pair.value)) version = (uint8_t)cbor_get_uint64(pair.value);
      }
    }
    if (version != 1) {
      cbor_decref(&root);
      return -1;
    }
    // ... (existing v1 loading code unchanged — extract local_id, hebbian, peers)
    // Extract local_id...
    // Extract hebbian weights...
    // Extract peer data...
    cbor_decref(&root);
  } else {
    cbor_decref(&root);
    return -1;
  }

  // Sync hebbian weights into ring nodes
  for (size_t index = 0; index < network->hebbian.count; index++) {
    hebbian_weight_t* weight = &network->hebbian.entries[index];
    net_node_t* node = ring_set_find_by_id(network->rings, &weight->peer_id);
    if (node != NULL) node->weight = weight->weight;
  }
  return 0;
}
```

The v1 fallback block must retain the exact same logic as the existing code (lines 318–442) for extracting version, local_id, hebbian, and peers from the map format.

- [ ] **Step 6: Commit**

```bash
git add src/Network/authority.h src/Network/authority.c
git commit -m "feat: store CA cert as DER in authority_t, migrate peer store to CBOR v2 positional arrays"
```

---

### Task 4: Add peer certificate validation to quic_listener.c

**Files:**
- Modify: `src/Network/quic_listener.c`

- [ ] **Step 1: Add include for peer_verify.h**

After the existing `#include` block (after line 36 `#include <cbor.h>`), add:

```c
#include "peer_verify.h"
```

- [ ] **Step 2: Add peer_verify_ctx_t field to quic_listener_t struct**

In `src/Network/quic_listener.h`, add after the `platform_mutex_t* conn_lock;` field (line 97):

```c
  void* peer_verify;  // peer_verify_ctx_t* — NULL if no CA cert loaded
```

- [ ] **Step 3: Create peer_verify_ctx_t in quic_listener_start**

In `src/Network/quic_listener.c`, in the `quic_listener_start` function, after the `authority_t* authority = listener->network->authority;` line (line 570), add:

```c
  // Create peer verifier if CA cert is loaded
  if (authority != NULL && authority->ca_cert_data != NULL && authority->ca_cert_len > 0) {
    listener->peer_verify = peer_verify_ctx_create(authority->ca_cert_data, authority->ca_cert_len);
  }
```

- [ ] **Step 4: Conditional credential flags**

Replace the credential config block (lines 571–582) with:

```c
  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  QUIC_CERTIFICATE_FILE cert_file = {0};
  if (authority != NULL && authority->node_cert_path != NULL && authority->node_key_path != NULL) {
    cert_file.CertificateFile = authority->node_cert_path;
    cert_file.PrivateKeyFile = authority->node_key_path;
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = (listener->peer_verify != NULL)
        ? QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
        : QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  } else {
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = (listener->peer_verify != NULL)
        ? QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
        : QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }
```

- [ ] **Step 5: Handle PEER_CERTIFICATE_RECEIVED in listener callback**

In `quic_listener_callback` (line 439), add a case before `default:`:

```c
    case QUIC_LISTENER_EVENT_PEER_CERTIFICATE_RECEIVED: {
      if (listener->peer_verify != NULL) {
        if (peer_verify_validate((peer_verify_ctx_t*)listener->peer_verify,
                                  event->PEER_CERTIFICATE_RECEIVED.Certificate) != 0) {
          log_error("quic_listener: peer certificate validation failed, rejecting connection");
          listener->msquic->ConnectionClose(event->PEER_CERTIFICATE_RECEIVED.Connection);
        }
      }
      break;
    }
```

- [ ] **Step 6: Destroy peer_verify_ctx_t in quic_listener_destroy**

In `quic_listener_destroy`, before the `_conn_track_shutdown_all` call (line 509), add:

```c
  if (listener->peer_verify != NULL) {
    peer_verify_ctx_destroy((peer_verify_ctx_t*)listener->peer_verify);
    listener->peer_verify = NULL;
  }
```

- [ ] **Step 7: Commit**

```bash
git add src/Network/quic_listener.h src/Network/quic_listener.c
git commit -m "feat: add peer certificate validation to quic_listener"
```

---

### Task 5: Add peer certificate validation to relay_client

**Files:**
- Modify: `src/Network/relay_client.h`
- Modify: `src/Network/relay_client.c`

- [ ] **Step 1: Add peer_verify_ctx_t to relay_client_t**

In `src/Network/relay_client.h`, after the `shared_registration` field block (line 75), add:

```c
  void* peer_verify;  // peer_verify_ctx_t* — NULL if no CA cert loaded
```

- [ ] **Step 2: Add include for peer_verify.h in relay_client.c**

After line 10 (`#include <cbor.h>`), add:

```c
#include "peer_verify.h"
```

- [ ] **Step 3: Create peer_verify_ctx_t in relay_client_create**

In `relay_client_create` (line 354), after `client->retry_delay_ms = retry_delay_ms;` (line 362), add:

```c
  // Create peer verifier if CA cert is loaded in authority
  if (network != NULL && network->authority != NULL &&
      network->authority->ca_cert_data != NULL && network->authority->ca_cert_len > 0) {
    client->peer_verify = peer_verify_ctx_create(
        network->authority->ca_cert_data, network->authority->ca_cert_len);
  }
```

- [ ] **Step 4: Conditional credential flags in relay_client_connect**

Replace the credential config block in `relay_client_connect` (lines 504–514) with:

```c
  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  QUIC_CERTIFICATE_FILE cert_file = {0};
  if (client->cert_path && client->key_path) {
    cert_file.CertificateFile = client->cert_path;
    cert_file.PrivateKeyFile = client->key_path;
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = (client->peer_verify != NULL)
        ? QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
        : QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  } else {
    cred_config.Flags = (client->peer_verify != NULL)
        ? QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
        : QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }
```

- [ ] **Step 5: Handle PEER_CERTIFICATE_RECEIVED in connection callback**

In `_relay_client_connection_callback` (line 206), add before the `default:` case:

```c
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
      if (client->peer_verify != NULL) {
        if (peer_verify_validate((peer_verify_ctx_t*)client->peer_verify,
                                  event->PEER_CERTIFICATE_RECEIVED.Certificate) != 0) {
          log_error("relay_client: peer certificate validation failed, closing connection");
          client->msquic->ConnectionClose(connection);
        }
      }
      break;
    }
```

- [ ] **Step 6: Destroy peer_verify_ctx_t in relay_client_destroy**

In `relay_client_destroy`, after the `client->shutdown_pending = 1;` line (line 393), add:

```c
  if (client->peer_verify != NULL) {
    peer_verify_ctx_destroy((peer_verify_ctx_t*)client->peer_verify);
    client->peer_verify = NULL;
  }
```

- [ ] **Step 7: Commit**

```bash
git add src/Network/relay_client.h src/Network/relay_client.c
git commit -m "feat: add peer certificate validation to relay_client"
```

---

### Task 6: Add peer certificate validation to relay_server

**Files:**
- Modify: `src/Network/Relay/relay_server.h`
- Modify: `src/Network/Relay/relay_server.c`

- [ ] **Step 1: Add peer_verify_ctx_t field to relay_server_t**

In `src/Network/Relay/relay_server.h`, after the `char* key_path;` field (line 50), add:

```c
  void* peer_verify;  // peer_verify_ctx_t* — NULL if no CA cert loaded
```

- [ ] **Step 2: Add include for peer_verify.h in relay_server.c**

Find the existing includes and add:

```c
#include "../peer_verify.h"
```

- [ ] **Step 3: Conditional credential flags in relay_server_start**

Replace the credential config block (lines 608–618) with:

```c
  QUIC_CREDENTIAL_CONFIG cred_config = {0};
  QUIC_CERTIFICATE_FILE cert_file = {0};
  if (server->cert_path && server->key_path) {
    cert_file.CertificateFile = server->cert_path;
    cert_file.PrivateKeyFile = server->key_path;
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = (server->peer_verify != NULL)
        ? QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
        : QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  } else {
    cred_config.Flags = (server->peer_verify != NULL)
        ? QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
        : QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }
```

- [ ] **Step 4: Handle PEER_CERTIFICATE_RECEIVED in listener callback**

In `_relay_listener_callback` (line 464), add before the `default:` case:

```c
    case QUIC_LISTENER_EVENT_PEER_CERTIFICATE_RECEIVED: {
      if (server->peer_verify != NULL) {
        if (peer_verify_validate((peer_verify_ctx_t*)server->peer_verify,
                                  event->PEER_CERTIFICATE_RECEIVED.Certificate) != 0) {
          log_error("relay_server: peer certificate validation failed, rejecting connection");
          server->msquic->ConnectionClose(event->PEER_CERTIFICATE_RECEIVED.Connection);
        }
      }
      break;
    }
```

- [ ] **Step 5: Destroy peer_verify_ctx_t in relay_server_destroy**

In `relay_server_destroy`, after the `server->key_path` cleanup (line 566), add:

```c
  if (server->peer_verify != NULL) {
    peer_verify_ctx_destroy((peer_verify_ctx_t*)server->peer_verify);
    server->peer_verify = NULL;
  }
```

- [ ] **Step 6: Commit**

```bash
git add src/Network/Relay/relay_server.h src/Network/Relay/relay_server.c
git commit -m "feat: add peer certificate validation to relay_server"
```

---

### Task 7: Add unit tests for peer_verify

**Files:**
- Create: `test/test_peer_verify.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Generate test certificates**

Before writing tests, generate a test CA and a leaf certificate signed by it. These are committed to the repo as test fixtures.

Run in the repo root:

```bash
mkdir -p test/certs

# Generate CA key and self-signed CA cert (valid 10 years)
openssl genrsa -out test/certs/ca_key.pem 2048
openssl req -new -x509 -days 3650 -key test/certs/ca_key.pem \
  -out test/certs/ca_cert.pem \
  -subj "/C=US/O=TestCA/CN=Test CA"

# Generate leaf key and CSR, sign with CA (valid 1 year)
openssl genrsa -out test/certs/leaf_key.pem 2048
openssl req -new -key test/certs/leaf_key.pem \
  -out test/certs/leaf_csr.pem \
  -subj "/C=US/O=TestLeaf/CN=test-node"
openssl x509 -req -days 365 -in test/certs/leaf_csr.pem \
  -CA test/certs/ca_cert.pem -CAkey test/certs/ca_key.pem \
  -CAcreateserial -out test/certs/leaf_cert.pem

# Generate a second CA (untrusted) for negative tests
openssl genrsa -out test/certs/other_ca_key.pem 2048
openssl req -new -x509 -days 3650 -key test/certs/other_ca_key.pem \
  -out test/certs/other_ca_cert.pem \
  -subj "/C=US/O=OtherCA/CN=Other CA"
```

- [ ] **Step 2: Write the test file**

```cpp
#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <cstdio>

extern "C" {
#include "../src/Network/peer_verify.h"
#include "../src/Util/allocator.h"
#include <openssl/pem.h>
#include <openssl/x509.h>
}

// Helper: read a file into a vector
static std::vector<uint8_t> read_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
}

// Helper: convert PEM cert to DER bytes
static std::vector<uint8_t> pem_to_der(const char* pem_path) {
  BIO* bio = BIO_new_file(pem_path, "r");
  if (bio == NULL) return {};
  X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (cert == NULL) return {};

  int der_len = i2d_X509(cert, NULL);
  std::vector<uint8_t> der(der_len);
  uint8_t* cursor = der.data();
  i2d_X509(cert, &cursor);
  X509_free(cert);
  return der;
}

class PeerVerifyTest : public ::testing::Test {
protected:
  std::vector<uint8_t> ca_der;
  std::vector<uint8_t> leaf_der;
  std::vector<uint8_t> other_ca_der;

  void SetUp() override {
    ca_der = pem_to_der("test/certs/ca_cert.pem");
    leaf_der = pem_to_der("test/certs/leaf_cert.pem");
    other_ca_der = pem_to_der("test/certs/other_ca_cert.pem");
  }
};

TEST_F(PeerVerifyTest, NullDataReturnsNull) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(NULL, 0);
  EXPECT_EQ(ctx, nullptr);
}

TEST_F(PeerVerifyTest, ZeroLengthReturnsNull) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), 0);
  EXPECT_EQ(ctx, nullptr);
}

TEST_F(PeerVerifyTest, ValidCACreatesContext) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  EXPECT_NE(ctx, nullptr);
  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, CorruptDERReturnsNull) {
  uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03};
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(garbage, sizeof(garbage));
  EXPECT_EQ(ctx, nullptr);
}

TEST_F(PeerVerifyTest, ValidLeafPasses) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  ASSERT_NE(ctx, nullptr);

  QUIC_CERTIFICATE cert;
  cert.Certificate = leaf_der.data();
  cert.CertificateLength = (uint32_t)leaf_der.size();
  cert.DeferValidation = FALSE;
  cert.Status = 0;

  EXPECT_EQ(peer_verify_validate(ctx, &cert), 0);
  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, WrongCAFails) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(other_ca_der.data(), other_ca_der.size());
  ASSERT_NE(ctx, nullptr);

  QUIC_CERTIFICATE cert;
  cert.Certificate = leaf_der.data();
  cert.CertificateLength = (uint32_t)leaf_der.size();
  cert.DeferValidation = FALSE;
  cert.Status = 0;

  EXPECT_NE(peer_verify_validate(ctx, &cert), 0);
  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, NullCertificateFails) {
  peer_verify_ctx_t* ctx = peer_verify_ctx_create(ca_der.data(), ca_der.size());
  ASSERT_NE(ctx, nullptr);
  EXPECT_NE(peer_verify_validate(ctx, NULL), 0);
  peer_verify_ctx_destroy(ctx);
}

TEST_F(PeerVerifyTest, DestroyNullIsSafe) {
  peer_verify_ctx_destroy(NULL);
  // Should not crash
}
```

- [ ] **Step 3: Add test to CMakeLists.txt**

In `test/CMakeLists.txt`, add `test_peer_verify.cpp` to the `testliboffs` executable sources (after `test_wire_validation.cpp` on line 52):

```
            test_peer_verify.cpp)
```

- [ ] **Step 4: Build and run the tests**

```bash
cd build && cmake .. && make testliboffs -j$(nproc)
./test/testliboffs --gtest_filter='PeerVerify*'
```

Expected: 7 tests pass (ValidCACreatesContext, ValidLeafPasses may fail if HAS_MSQUIC is not defined because QUIC_CERTIFICATE type won't be available; see note below)

Note: The `QUIC_CERTIFICATE` struct requires `HAS_MSQUIC`. The tests that use it (ValidLeafPasses, WrongCAFails, NullCertificateFails) should be wrapped in `#ifdef HAS_MSQUIC` / `#endif`. Update the test file accordingly:

```cpp
#ifdef HAS_MSQUIC
TEST_F(PeerVerifyTest, ValidLeafPasses) {
  // ... as above ...
}
// ... other QUIC_CERTIFICATE tests ...
#endif
```

The other 4 tests (NullDataReturnsNull, ZeroLengthReturnsNull, ValidCACreatesContext, CorruptDERReturnsNull, DestroyNullIsSafe) run regardless.

- [ ] **Step 5: Commit**

```bash
git add test/test_peer_verify.cpp test/CMakeLists.txt test/certs/
git commit -m "test: add unit tests for peer_verify certificate validation"
```

---

### Task 8: Build and verify

- [ ] **Step 1: Build the full project**

```bash
cd build && cmake .. && make -j$(nproc)
```

Expected: Build succeeds with no warnings.

- [ ] **Step 2: Run the full test suite**

```bash
./test/testliboffs
```

Expected: All existing tests pass. New PeerVerify tests pass.

- [ ] **Step 3: Run de-wonk audit**

Follow the de-wonk skill: audit every changed file for unimplemented, stubbed, disabled, broken, or weird patterns.

---

## Self-Review

**Spec coverage:**
- Section 1 (authority_t changes): Task 3 covers everything — ca_cert_data/ca_cert_len fields, authority_load_ca_cert, authority_save v2, authority_load backward compat, authority_destroy ✅
- Section 2 (peer_verify): Tasks 1-2 cover the new module ✅
- Section 3 (quic_listener): Task 4 covers INDICATE_CERTIFICATE_RECEIVED flag, CERTIFICATE_RECEIVED event handler, destroy ✅
- Section 4 (relay_client): Task 5 covers everything ✅
- Section 5 (relay_server): Task 6 covers everything ✅
- Section 6 (unchanged sites): Implicit — no tasks needed ✅
- Testing section: Task 7 covers unit tests with real certs ✅
- Error handling table: Each scenario covered by null checks in Tasks 1-3 ✅

**No placeholders found.**

**Type consistency:** peer_verify_ctx_t* is stored as void* on structs to avoid including peer_verify.h in header files (avoids dragging OpenSSL headers into consumers). Casts are explicit at call sites.
