# Tier-5b Relay Signed-Nonce Challenge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the deferred signed-nonce challenge for the relay path (audit #8 relay), so a relayed peer proves possession of the private key for its claimed `sender_id` before being trusted. Closes the impersonation vector on the relay path that tier-5's `relay_verified` flag surfaced.

**Architecture:** A challenge-response handshake relayed between peers. When the receiver gets a relayed message from an unverified `sender_id`, it sends a `WIRE_RELAY_CHALLENGE` (with a fresh nonce) back via the relay. The peer signs the nonce with its private key and returns `WIRE_RELAY_CHALLENGE_RESPONSE` (nonce + public_key + signature). The receiver verifies `BLAKE3(public_key) == sender_id` AND the signature is valid for the nonce under `public_key`. On success, the peer's `relay_verified` flag is set true. A timeout sweep expires unanswered challenges.

**Tech Stack:** C11, OpenSSL (`EVP_PKEY` / `EVP_DigestSign` / `EVP_DigestVerify` for Ed25519 sign/verify, or `EVP_PKEY_get_raw_private_key` + a raw sign), MsQuic relay, CBOR wire format, GoogleTest, CMake, valgrind.

**Scope (in):** the deferred signed-nonce challenge for audit #8 relay (the item tier-5 deferred).

**Scope (out):** everything else (the relay path's other concerns are unchanged; the direct path's cert pin from tier-5 is the direct-path fix).

**Build / test commands:**
```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='<filter>'
cmake --build cmake-build-vg -j$(nproc) --target testliboffs
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='<filter>'
```

**Style:** `_t` suffix, `type_action()`, no single-letter names, no TODO/FIXME, no `Co-Authored-By`, de-wonk before done, valgrind for leaks.

---

## File Structure

| File | Responsibility | Touched by task |
| --- | --- | --- |
| `src/Network/wire.h`, `wire.c` | New `WIRE_RELAY_CHALLENGE` + `WIRE_RELAY_CHALLENGE_RESPONSE` types + encode/decode/destroy. | 1 |
| `src/Network/pem_key.h`, `pem_key.c` | `pem_key_sign_nonce` (load the private key from `node_key_path`, sign a nonce) + `pem_key_verify_nonce` (verify a signature under a public key). | 2 |
| `src/Network/authority.h`, `authority.c` | Cache the private key (`EVP_PKEY*`) on the authority for efficient signing; expose `authority_sign_nonce`. | 2 |
| `src/Network/network.h`, `network.c` | The challenge table (`relay_challenge_t`: sender_id + nonce + deadline), `network_relay_send_challenge` (build + send via the relay), `network_handle_relay_challenge` (sign + respond), `network_handle_relay_challenge_response` (verify + set `relay_verified`), the timeout sweep (reuse the `NETWORK_REQUEST_TIMEOUT_TICK`). Wire the challenge trigger into the `NETWORK_RELAY_RECEIVED` handler (send a challenge for unverified senders). | 3, 4 |
| `test/` | Unit tests for sign/verify + the challenge table; integration test if feasible. | 1-4 |

---

## Task 1: New wire types for the challenge/response

**Files:**
- Modify: `src/Network/wire.h` (new structs + enum values), `src/Network/wire.c` (encode/decode/destroy)
- Test: `test/test_wire_validation.cpp` or `test/test_recycler_wire.cpp`

**Why:** The challenge and response are INNER wire messages (carried in the relay payload), relayed between peers like any other message. They need CBOR encode/decode/destroy.

### Step 1: Add the types

In `src/Network/wire.h`, add enum values `WIRE_RELAY_CHALLENGE` and `WIRE_RELAY_CHALLENGE_RESPONSE` (near the other `WIRE_*` values; find the enum).

```c
typedef struct wire_relay_challenge_t {
  node_id_t challenger_id;   // the node sending the challenge (the receiver of the original relayed message)
  uint8_t  nonce[32];        // a fresh random nonce
} wire_relay_challenge_t;

typedef struct wire_relay_challenge_response_t {
  node_id_t responder_id;    // the node responding (the original relayed sender)
  uint8_t  nonce[32];        // the nonce being responded to
  uint8_t* public_key;       // the responder's public key (BLAKE3 of this must equal responder_id)
  size_t   public_key_len;
  uint8_t* signature;        // the responder's signature of the nonce under the private key for public_key
  size_t   signature_len;
} wire_relay_challenge_response_t;
```

### Step 2: Encode/decode/destroy

In `src/Network/wire.c`, add encode/decode/destroy for both (mirror the existing patterns — e.g., `wire_ping_encode` uses `cbor_new_definite_array` + `_node_id_encode`). The challenge is `[type, challenger_id, nonce_bytestring]`. The response is `[type, responder_id, nonce_bytestring, public_key_bytestring, signature_bytestring]`. The decode allocates `public_key`/`signature` via `get_clear_memory` + `memcpy`. The destroy frees them.

### Step 3: Test

Round-trip tests: encode → decode → assert fields match. Add to `test/test_wire_validation.cpp`. Confirm `BLAKE3(public_key) == responder_id` is NOT checked by the decode (it's checked by the receiver's verification logic in Task 3).

### Step 4: Commit

```bash
git add src/Network/wire.h src/Network/wire.c test/test_wire_validation.cpp
git commit -m "feat(wire): add WIRE_RELAY_CHALLENGE + WIRE_RELAY_CHALLENGE_RESPONSE

New inner wire types for the relay signed-nonce challenge (audit #8 relay,
deferred from tier-5). The challenge carries a challenger_id + a 32-byte
nonce; the response carries the responder_id + nonce + public_key + signature.
The receiver verifies BLAKE3(public_key)==responder_id and the signature.
See audit #8."
```

---

## Task 2: Sign/verify helpers + authority private-key caching

**Files:**
- Modify: `src/Network/pem_key.h`, `pem_key.c` (`pem_key_sign_nonce`, `pem_key_verify_nonce`), `src/Network/authority.h`, `authority.c` (cache the private key; `authority_sign_nonce`)
- Test: `test/test_peer_verify.cpp` or a new `test/test_pem_key.cpp`

**Why:** The responder signs the nonce with its private key; the challenger verifies the signature under the responder's public key. The authority has `node_key_path` (the private key PEM) but doesn't hold it in memory — cache it for efficient signing.

### Step 1: Add sign/verify helpers

In `src/Network/pem_key.h`:
```c
/* Sign a 32-byte nonce with the private key loaded from key_path. Returns 0
   on success and writes the signature to *out_sig (caller frees). Returns -1
   on failure. Uses Ed25519 if the key is Ed25519 (raw sign), else
   EVP_DigestSign. See audit #8. */
int pem_key_sign_nonce(const char* key_path, const uint8_t nonce[32],
                       uint8_t** out_sig, size_t* out_sig_len);

/* Verify a signature of a 32-byte nonce under a public key. Returns 0 if
   valid, -1 if invalid or on error. Uses Ed25519 if the key is Ed25519 (raw
   verify), else EVP_DigestVerify. See audit #8. */
int pem_key_verify_nonce(const uint8_t* public_key, size_t public_key_len,
                         const uint8_t nonce[32],
                         const uint8_t* signature, size_t signature_len);
```

In `src/Network/pem_key.c`, implement: `PEM_read_PrivateKey` (or `PEM_read_bio_PrivateKey`) to load the key, then `EVP_DigestSign` / `EVP_DigestVerify` (or the Ed25519 raw sign/verify if available). Free the `EVP_PKEY` on all paths. For the verify, reconstruct the `EVP_PKEY` from the raw public key (`EVP_PKEY_new_raw_public_key` for Ed25519, or `d2i_PUBKEY` for DER SPKI) — mirror the format logic from `peer_verify_extract_pubkey` / `pem_extract_public_key`.

### Step 2: Cache the private key on the authority

In `src/Network/authority.h`, add `EVP_PKEY* node_private_key;` (cached, loaded once at create). In `authority.c` `authority_create`, load it from `node_key_path` (if set); in `authority_destroy`, `EVP_PKEY_free` it. Add `authority_sign_nonce(authority, nonce, &sig, &sig_len)` that uses the cached key (or loads on demand if not cached).

### Step 3: Test

- Unit test `pem_key_sign_nonce` + `pem_key_verify_nonce`: generate a key pair (the `test_peer_verify` fixture generates a CA + cert; extract the key pair), sign a nonce, verify with the matching public key (passes) and a wrong public key (fails). Add to `test/test_peer_verify.cpp` or a new `test/test_pem_key.cpp`.
- Unit test the authority caching: `authority_sign_nonce` produces a verifiable signature.

### Step 4: Commit

```bash
git add src/Network/pem_key.h src/Network/pem_key.c src/Network/authority.h src/Network/authority.c test/
git commit -m "feat(pem_key): sign/verify nonce helpers + authority private-key caching

Add pem_key_sign_nonce (sign a 32-byte nonce with the private key from a PEM
path) and pem_key_verify_nonce (verify a signature under a public key), for
the relay signed-nonce challenge (audit #8 relay). Cache the EVP_PKEY on the
authority (loaded once from node_key_path) for efficient signing. See audit #8."
```

---

## Task 3: The challenge table + send/verify + timeout sweep

**Files:**
- Modify: `src/Network/network.h` (the `relay_challenge_t` table), `src/Network/network.c` (send-challenge, handle-challenge, handle-response, sweep, wire into the relay handler)
- Test: `test/test_network.cpp`

**Why:** The receiver tracks pending challenges (sender_id + nonce + deadline), sends a challenge on the first unverified relayed message, verifies the response, and expires unanswered challenges.

### Step 1: Add the challenge table

In `src/Network/network.h`, add to `network_t`:
```c
  relay_challenge_t* relay_challenges;   // pending relay signed-nonce challenges
  size_t            relay_challenge_count;
```
And the struct:
```c
typedef struct relay_challenge_t {
  node_id_t sender_id;
  uint8_t   nonce[32];
  uint64_t  deadline_ms;
  uint32_t  relay_endpoint_id;  // to route the challenge back via the relay
} relay_challenge_t;
```

### Step 2: Send a challenge for unverified relayed senders

In `src/Network/network.c`, the `NETWORK_RELAY_RECEIVED` handler (line 3993+), after `wire_extract_sender_id` + `connection_manager_lookup`/`add` (line 4012-4016): if the peer is not `relay_verified` (and not already challenged), send a `WIRE_RELAY_CHALLENGE` back via the relay (to `relay_payload->src_endpoint_id`) with a fresh nonce (use a CSPRNG or the existing random; read how the project generates randomness — `node_id_generate` uses `rand()`, but for a nonce, prefer a stronger source if available; if not, `rand()` + a monotonic counter is acceptable for a nonce). Record the challenge (sender_id + nonce + deadline + endpoint_id).

Build the `WIRE_RELAY_CHALLENGE`, CBOR-encode it, wrap it in a `wire_relay_send_t` = `{dest_endpoint_id = src_endpoint_id, src_endpoint_id = our_endpoint_id, payload = the encoded challenge}`, and send via `RELAY_CLIENT_SEND` to the relay client actor. Read the relay send path (`relay_client.c:715`) + how the network sends to the relay client (`actor_send(&network->relay->actor, &msg)` with `msg.type = RELAY_CLIENT_SEND`).

### Step 3: Handle the challenge (the responder signs)

In `src/Network/network.c`, the relay handler's switch (the `switch (type)` at line 4037), add `case WIRE_RELAY_CHALLENGE:` — decode the challenge, sign the nonce with the authority's private key (`authority_sign_nonce`), build a `WIRE_RELAY_CHALLENGE_RESPONSE` = `{responder_id = our local_id, nonce, public_key, signature}`, encode it, wrap in a `wire_relay_send_t` to the challenger's endpoint (the challenge's `challenger_id` — but we only have the `src_endpoint_id` from the relay envelope; the `challenger_id` in the challenge message is the node_id, not the endpoint. Hmm — to route the response back, we need the challenger's endpoint id. Options: include the challenger's endpoint id in the challenge message, OR look up the challenger's relay endpoint via the connection manager. Read how relayed messages route back — the `src_endpoint_id` in the relay envelope is the SENDER's endpoint, which the receiver stores on the peer (`existing->relay_endpoint_id`). So the challenger's endpoint is `network->relay->local_endpoint_id` for sending TO the challenger? No — the challenger is another peer. We need the challenger's endpoint id to route the response. Include it in the challenge message: add `challenger_endpoint_id` to `wire_relay_challenge_t`.)

Update Task 1's `wire_relay_challenge_t` to include `uint32_t challenger_endpoint_id;` (the relay endpoint id to route the response back to). The responder uses it to send the response via the relay.

### Step 4: Handle the response (the challenger verifies)

In the relay handler's switch, add `case WIRE_RELAY_CHALLENGE_RESPONSE:` — decode the response, find the pending challenge by `responder_id` + `nonce`, verify: `BLAKE3(public_key) == responder_id` (via `node_id_from_public_key`) AND `pem_key_verify_nonce(public_key, nonce, signature)` AND the nonce matches the pending challenge. If all pass, set the peer's `relay_verified = true` (find the peer via `connection_manager_lookup(&responder_id)`), remove the pending challenge. If any fail, drop (log).

### Step 5: Timeout sweep

Reuse the `NETWORK_REQUEST_TIMEOUT_TICK` (from tier-3) — add a sweep of the `relay_challenges` table in `network_handle_request_timeout_tick`: remove challenges whose `deadline_ms` has passed. (The peer stays `relay_verified = false`; a future message re-triggers a challenge.) OR add a dedicated sweep. Prefer reusing the existing tick (one sweep function for all pending state).

### Step 6: Test

- Unit test the challenge table: add a challenge, find it, remove it, sweep expired.
- Unit test the send/verify flow (if a full network + relay fixture is feasible): a relayed message from an unverified peer triggers a challenge; the response verifies. If the full fixture is too heavy, rely on the sign/verify unit tests (Task 2) + the challenge-table unit tests + code-review.

### Step 7: Commit

```bash
git add src/Network/network.h src/Network/network.c test/
git commit -m "feat(network): relay signed-nonce challenge — send, verify, sweep

When a relayed message arrives from an unverified sender (relay_verified
false), send a WIRE_RELAY_CHALLENGE (fresh nonce) back via the relay. The
responder signs the nonce with its private key and returns
WIRE_RELAY_CHALLENGE_RESPONSE (public_key + signature). Verify
BLAKE3(public_key)==responder_id and the signature; set relay_verified=true
on success. A timeout sweep (reusing the NETWORK_REQUEST_TIMEOUT_TICK)
expires unanswered challenges. Closes the relay-path impersonation vector
(audit #8 relay; the deferred signed-nonce challenge from tier-5)."
```

---

## Task 4: Whole-tier verification

- [ ] Build. `cmake --build cmake-build-verify -j$(nproc) --target testliboffs`.
- [ ] Full test suite. `cmake-build-verify/test/testliboffs` — all pass (modulo pre-existing SSL).
- [ ] Valgrind. `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestPeerVerify.*:TestWireValidation.*:QuicIntegration.*:TestNetwork.*:StreamNetwork.*:TestShutdown.*:NetworkSyncDispatchTest.*'` — `definitely lost: 0 bytes`, no new invalid reads. The challenge table + the sign/verify must not leak (the EVP_PKEY cache, the nonce, the signature, the challenge entries).
- [ ] De-wonk. No stubbed/disabled code.
- [ ] TODO check. No new TODOs.

---

## Self-Review

**1. Spec coverage.** The deferred signed-nonce challenge (audit #8 relay) → Tasks 1-3.
- Wire types → Task 1 ✓
- Sign/verify + key caching → Task 2 ✓
- Challenge table + send/verify + sweep → Task 3 ✓
- Verification → Task 4 ✓

**2. Interaction.** The challenge rides on the existing relay (inner messages). The timeout sweep reuses the tier-3 `NETWORK_REQUEST_TIMEOUT_TICK`. The `relay_verified` flag (tier-5) is set true on success. The direct path's cert pin (tier-5) is unchanged.

**3. Edge cases.**
- A peer that doesn't support the challenge (old version) won't respond → the challenge times out → the peer stays `relay_verified = false` (downgraded but still admitted, as today). No regression for old peers.
- A peer that responds with a wrong key/signature → verification fails → stays `relay_verified = false`.
- Re-challenge: if the peer sends another message before the first challenge completes, don't send a second challenge (check the pending table first).
- The `challenger_endpoint_id` in the challenge message is how the responder routes the response back — include it (Task 1 update).

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-15-tier5b-relay-signed-nonce.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, two-stage review between tasks.
2. **Inline Execution** — execute in this session with checkpoints.

Which approach?