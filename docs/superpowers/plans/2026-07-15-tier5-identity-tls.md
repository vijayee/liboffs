# Tier-5 Identity & Auth-over-TLS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bind node identity to the TLS credential (audit #8) and enable TLS certificate validation (audit #11) so the bcrypt api-key auth isn't undermined by MITM, and a node can't impersonate another by lifting its public key from gossip.

**Architecture:** Four focused fixes. Task 1 wires cert validation everywhere a CA is configured (the WT server currently skips it entirely; relay server/client and offs_client fall back to `NO_CERTIFICATE_VALIDATION`). Task 2 adds an explicit `allow_insecure` opt-in and fails closed when no CA is configured unless the operator explicitly opts in (the audit's "fail closed" recommendation, with a migration path for trusted-LAN). Task 3 pins the salutation `public_key` to the peer's TLS leaf-cert public key on the direct QUIC path, closing the impersonation. Task 4 applies the salutation self-consistency check on the relay path (which currently admits peers via `wire_extract_sender_id` with no identity check).

**Tech Stack:** C11, MsQuic (`QUIC_CREDENTIAL_FLAG_*`, `QUIC_PARAM_CONN_PEER_CERTIFICATE`), OpenSSL (`X509` / `EVP_PKEY` for leaf-cert key extraction), GoogleTest, CMake, valgrind.

**Scope (in):** `docs/liboffs-audit-report.md` findings #8, #11.

**Scope (out — deferred):** the signed-nonce challenge for the relay path (the audit's stronger fix for #8 relay — proof-of-possession without a direct TLS connection); #15-17/#19/#20/#24 (CLI lies); #18 (NAT/relay feature); MEDIUM/LOW audit findings.

**Behavior change (Task 2):** fail closed without a CA unless `allow_insecure` is set. This is the audit's recommendation. The `allow_insecure` flag is the migration path for trusted-LAN/research use. The test fixtures that run without a CA must set `allow_insecure = true` (or configure a CA via `ca_generate`, as `test_peer_verify` already does).

**Build / test commands:**
```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs --gtest_filter='<filter>'
test -d cmake-build-vg || cmake -S . -B cmake-build-vg -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-gdwarf-4 -O0" -DCMAKE_CXX_FLAGS="-gdwarf-4 -O0"
cmake --build cmake-build-vg -j$(nproc) --target testliboffs
valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='<filter>'
```

**Style reminder:** `_t` suffix, `type_action()`, no single-letter names, no TODO/FIXME, no `Co-Authored-By`, de-wonk before done, valgrind for leaks.

---

## File Structure

| File | Responsibility | Touched by task |
| --- | --- | --- |
| `src/Network/quic_listener.c` | WT/quic cert config: validate when CA present; fail closed without CA unless `allow_insecure`; outbound client credential. Extract peer leaf-cert public key for Task 3. | 1, 2, 3 |
| `src/Network/Relay/relay_server.c` | Relay server cert config: validate when CA present; fail closed without CA unless `allow_insecure`. | 1, 2 |
| `src/Network/relay_client.c` | Relay client cert config: validate the relay server's cert against the CA (client credential); fail closed without CA unless `allow_insecure`. | 1, 2 |
| `src/ClientAPI/WT/wt_transport.c` | WT server cert config: validate client certs against the CA (wire `peer_verify`); fail closed without CA unless `allow_insecure`. | 1, 2 |
| `src/ClientLibs/c/offs_client.c` | WT client cert config: validate the WT server's cert against the CA; fail closed without CA unless `allow_insecure`. | 1, 2 |
| `src/Network/authority.h`, `authority.c` | Add `bool allow_insecure` field (default false). | 2 |
| `src/Configuration/*` | Load `allow_insecure` from config (if a config field exists; else default false). | 2 |
| `src/Network/peer_verify.c`, `peer_verify.h` | Add `peer_verify_extract_pubkey` (extract the leaf-cert public key from a DER cert buffer). | 3 |
| `src/Network/network.c` | `network_handle_salutation`: pin `salut->public_key` to the TLS leaf-cert public key (extracted from the QUIC connection). Relay path: apply the salutation self-consistency check. | 3, 4 |
| `test/*` | Test fixtures: set `allow_insecure = true` where they run without a CA; new tests for cert validation + salutation pinning. | 1-4 |

---

## Task 1: Wire cert validation when a CA is configured

**Files:**
- Modify: `src/Network/quic_listener.c`, `src/Network/Relay/relay_server.c`, `src/Network/relay_client.c`, `src/ClientAPI/WT/wt_transport.c`, `src/ClientLibs/c/offs_client.c`
- Test: existing `TestPeerVerify.*`, `QuicIntegration.*`, `TestWsTransport.*`, `TestOffsWtClient.*`

**Why:** `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION` is set in 8 sites. The quic_listener already validates when `peer_verify` (CA) is set, but the WT server (`wt_transport.c:622`) skips validation entirely, and the relay server/client + offs_client fall back to `NO_CERTIFICATE_VALIDATION` even when a CA could be used. Fix: when a CA is available, use `QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE` (+ `QUIC_CREDENTIAL_FLAG_CLIENT` for outbound); only fall back to `NO_CERTIFICATE_VALIDATION` when NO CA is available, with a logged warning.

### Step 1: Read the cert-config blocks

Read each site:
- `quic_listener.c:610-645` — the existing pattern (SET_CA when peer_verify, else NO_CERT). This is the reference.
- `relay_server.c:640-655` — the relay server cert config.
- `relay_client.c:525-545` — the relay client cert config.
- `wt_transport.c:605-625` — the WT server cert config (no CA validation).
- `offs_client.c:1245-1260` — the WT client cert config.

For each, determine: does it have access to a CA? (The relay/WT may need a CA passed in via their create/config — check the signatures.) If a CA is available, wire `SET_CA_CERTIFICATE_FILE`. If not, leave the fallback (Task 2 will fail-close it).

### Step 2: Wire cert validation where a CA is available

For each site that has access to a CA (or can be given one):
- **Server paths** (quic_listener, relay_server, wt_transport): if CA is set, use `SET_CA_CERTIFICATE_FILE` to validate client certs. The quic_listener already does this — mirror it in relay_server and wt_transport.
- **Client paths** (relay_client, offs_client, quic_listener outbound): if CA is set, use `SET_CA_CERTIFICATE_FILE` + `QUIC_CREDENTIAL_FLAG_CLIENT` to validate the server's cert.
- Where a CA is NOT available, keep the `NO_CERTIFICATE_VALIDATION` fallback BUT add a `log_warn` ("...: no CA configured; TLS encrypts but does not authenticate — MITM possible. Set allow_insecure=1 to acknowledge and proceed.") — this sets up Task 2's fail-close.

### Step 3: Test

Run the existing suites. The tests that run without a CA will hit the new `log_warn` (but still proceed via the fallback — Task 2 changes this to fail-close). The tests that generate a CA (`test_peer_verify`) will exercise the validation path. Add a test if feasible: a WT/relay server with a CA rejects a client with an untrusted cert. If the test infrastructure doesn't support a full CA + untrusted-cert setup, rely on the existing `TestPeerVerify.*` + code-review.

### Step 4: Commit

```bash
git add <touched files>
git commit -m "fix(tls): wire cert validation when a CA is configured

NO_CERTIFICATE_VALIDATION was set unconditionally on the WT server and as a
fallback on the relay server/client + offs_client, even when a CA was
available -> MITM (audit #11). When a CA is configured, use
SET_CA_CERTIFICATE_FILE (+ CLIENT for outbound) to validate the peer's cert;
only fall back to NO_CERTIFICATE_VALIDATION when no CA is available, with a
logged warning. The quic_listener already did this; mirror it in the relay/
WT paths. See audit #11."
```

---

## Task 2: Fail closed without a CA unless `allow_insecure` is set

**Files:**
- Modify: `src/Network/authority.h` (add `bool allow_insecure`), `authority.c` (init false), `src/Configuration/*` (load from config if a field exists)
- Modify: `src/Network/quic_listener.c`, `src/Network/Relay/relay_server.c`, `src/Network/relay_client.c`, `src/ClientAPI/WT/wt_transport.c`, `src/ClientLibs/c/offs_client.c` — fail closed (return error / refuse to start) when no CA AND `!allow_insecure`
- Test: update fixtures that run without a CA to set `allow_insecure = true`

**Why:** The audit's "fail closed when no CA is configured" — TLS encrypts but does not authenticate without a CA, undermining the bcrypt auth. The `allow_insecure` flag is the explicit opt-in for trusted-LAN/research use.

### Step 1: Add `allow_insecure` to authority + config

In `src/Network/authority.h`, add to the `authority_t` struct:
```c
  bool allow_insecure;  /* if true, allow NO_CERTIFICATE_VALIDATION when no CA
                           is configured (trusted-LAN/research; logs a warning).
                           Default false — fail closed without a CA. See #11. */
```

In `authority.c` (`authority_create`), init `allow_insecure = false`. In the config loader, load it from a config field (e.g., `network.allow_insecure`) if one exists; else default false. Read the config loader to find where `ca_cert_data` is loaded and add `allow_insecure` nearby.

### Step 2: Fail closed in each cert-config site

In each site (quic_listener, relay_server, relay_client, wt_transport, offs_client), change the no-CA fallback:
```c
  if (ca_available) {
    // ... SET_CA_CERTIFICATE_FILE (+ CLIENT for outbound) ...
  } else if (allow_insecure) {
    log_warn("...: no CA configured and allow_insecure is set — TLS will not "
             "authenticate the peer (MITM possible). Configure a CA for production.");
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  } else {
    log_error("...: no CA configured and allow_insecure is not set — refusing to "
              "start. Configure a CA, or set allow_insecure=1 for trusted-LAN use.");
    return NULL;  // or the appropriate error return for the site
  }
```

For the relay/WT paths that don't have direct access to `authority->allow_insecure`, thread it through their create/config functions (add an `allow_insecure` param or a config struct field). Read each create function to find the right place.

### Step 3: Update test fixtures

The tests that run without a CA (quic_integration, ws_transport, offs_client, etc.) must set `allow_insecure = true` in their authority/config, OR generate a CA via `ca_generate` (as `test_peer_verify` does). Read each test fixture and update. Prefer generating a CA where feasible (tests the validation path); fall back to `allow_insecure = true` where the CA setup is too heavy.

### Step 4: Test + valgrind

```bash
cmake --build cmake-build-verify -j$(nproc) --target testliboffs
cmake-build-verify/test/testliboffs
```
Expected: all tests pass (the fixtures either generate a CA or set `allow_insecure`). No NEW failures. Valgrind on the affected suites.

### Step 5: Commit

```bash
git add <touched files>
git commit -m "fix(tls): fail closed without a CA unless allow_insecure is set

TLS without a CA validates nothing — MITM undermines the bcrypt api-key auth
(audit #11). Add an allow_insecure flag (default false) to authority_t and
the config; when no CA is configured and allow_insecure is false, refuse to
start the P2P/relay/WT server and client (fail closed). When allow_insecure
is true, proceed with NO_CERTIFICATE_VALIDATION and a logged warning
(trusted-LAN/research migration path). Test fixtures that run without a CA
now set allow_insecure or generate a CA. See audit #11."
```

---

## Task 3: Pin the salutation public_key to the TLS leaf-cert public key

**Files:**
- Modify: `src/Network/peer_verify.c`, `peer_verify.h` (add `peer_verify_extract_pubkey`), `src/Network/quic_listener.c` (extract the peer cert from the QUIC connection), `src/Network/network.c` (`network_handle_salutation`: compare `salut->public_key` to the cert key)
- Test: `test/test_peer_verify.cpp` (extract pubkey from a known cert) + `test/test_quic_integration.cpp` (salutation pinning end-to-end if feasible)

**Why:** `network_handle_salutation` verifies `BLAKE3(public_key) == sender_id` but nothing ties `public_key` to the TLS credential MsQuic authenticated. Any CA-admitted node can lift a victim's public key from gossip and present `sender_id = BLAKE3(Kv)` to be accepted as the victim → routing-table poisoning, FindBlock/StoreBlock interception. Fix: after the QUIC handshake, extract the peer's leaf-cert public key; in `network_handle_salutation`, compare `salut->public_key` to the extracted cert key. Reject on mismatch. (Requires Task 1/2's cert validation — otherwise the cert key isn't trustworthy.)

### Step 1: Add `peer_verify_extract_pubkey`

In `src/Network/peer_verify.h`:
```c
/* Extract the leaf-cert public key (raw, e.g. DER-encoded SubjectPublicKeyInfo
   or the raw key bytes) from a DER-encoded certificate. Returns 0 on success
   and writes the key to *out_key / *out_len (caller frees out_key). Returns
   -1 on failure. Used to pin the salutation public_key to the TLS credential.
   See audit #8. */
int peer_verify_extract_pubkey(const uint8_t* cert_der, size_t cert_len,
                               uint8_t** out_key, size_t* out_len);
```

In `src/Network/peer_verify.c`, implement with OpenSSL (`d2i_X509` → `X509_get_pubkey` → `i2d_PUBKEY` for the DER-encoded key, OR extract the raw key bytes depending on what `salut->public_key` is). Read `node_id_from_public_key` to see what format `salut->public_key` expects (raw bytes? DER SPKI? a specific key type?). The extracted cert key must be in the same format for the comparison.

### Step 2: Extract the peer cert from the QUIC connection

In `src/Network/quic_listener.c`, after the QUIC handshake completes (the CONNECTED event in `quic_connection_callback`), extract the peer's cert via `QUIC_PARAM_CONN_PEER_CERTIFICATE`:
```c
  uint8_t* cert_buf = NULL;
  uint32_t cert_len = 0;
  listener->msquic->GetParam(connection, QUIC_PARAM_CONN_PEER_CERTIFICATE, &cert_len, NULL);
  if (cert_len > 0) {
    cert_buf = get_clear_memory(cert_len);
    listener->msquic->GetParam(connection, QUIC_PARAM_CONN_PEER_CERTIFICATE, &cert_len, cert_buf);
  }
```
Store the cert (or its extracted pubkey) on the `pending_quic_t` so `network_handle_salutation` can access it. Read the `pending_quic_t` struct and the CONNECTED handler to find where to store it.

### Step 3: Compare in `network_handle_salutation`

In `src/Network/network.c`, `network_handle_salutation`, after the existing `BLAKE3(public_key) == sender_id` check (line 514), add:
```c
  // Pin the salutation public_key to the TLS leaf-cert public key. The
  // BLAKE3(public_key)==sender_id check above only verifies self-consistency;
  // without this pin, any CA-admitted node can lift a victim's public_key
  // from gossip and impersonate them. See audit #8.
  if (pending->peer_cert_der != NULL && pending->peer_cert_len > 0) {
    uint8_t* cert_pubkey = NULL;
    size_t cert_pubkey_len = 0;
    if (peer_verify_extract_pubkey(pending->peer_cert_der, pending->peer_cert_len,
                                   &cert_pubkey, &cert_pubkey_len) != 0) {
      log_error("salutation: failed to extract peer cert public key");
      wire_salutation_destroy(salut);
      free(pending);
      return;
    }
    bool pubkey_matches = (cert_pubkey_len == salut->public_key_len &&
                           memcmp(cert_pubkey, salut->public_key, cert_pubkey_len) == 0);
    free(cert_pubkey);
    if (!pubkey_matches) {
      log_error("salutation: public_key does not match the TLS leaf-cert key (impersonation attempt?)");
      wire_salutation_destroy(salut);
      free(pending);
      return;
    }
  }
  // If no peer cert was extracted (e.g., cert validation was skipped —
  // allow_insecure mode), the pin is a no-op; the BLAKE3 check above is the
  // only guard. This is the documented insecure-mode behavior.
```

### Step 4: Test

- Unit test `peer_verify_extract_pubkey` with a known cert (the `test_peer_verify` fixture generates a CA + cert; extract the pubkey and assert it matches the cert's key).
- Integration test (if feasible): a direct QUIC connection where the salutation's `public_key` doesn't match the cert key → rejected. If the full QUIC + cert + salutation fixture is too heavy, rely on the unit test + code-review.

### Step 5: Commit

```bash
git add <touched files>
git commit -m "fix(identity): pin salutation public_key to the TLS leaf-cert key

network_handle_salutation verified BLAKE3(public_key)==sender_id but nothing
tied public_key to the TLS credential MsQuic authenticated — any CA-admitted
node could lift a victim's public_key from gossip and present sender_id=
BLAKE3(Kv) to be accepted as the victim -> routing-table poisoning,
FindBlock/StoreBlock interception (audit #8). Extract the peer's leaf-cert
public key from the QUIC handshake (QUIC_PARAM_CONN_PEER_CERTIFICATE) and
compare it to salut->public_key; reject on mismatch. The pin is a no-op in
allow_insecure mode (no cert extracted); the BLAKE3 check is the only guard
there. See audit #8."
```

---

## Task 4: Apply the salutation check on the relay path

**Files:**
- Modify: `src/Network/network.c` (the RELAY_RECEIVED handler — apply the salutation self-consistency check)
- Test: `test/test_network.cpp` or `test/test_quic_integration.cpp`

**Why:** The relay receive path admits peers via `wire_extract_sender_id` **without** the BLAKE3 salutation check that direct QUIC connections require. A relayed peer can claim any `sender_id`. Fix: apply the salutation self-consistency check (BLAKE3(pubkey)==sender_id) on the relay path. (The stronger signed-nonce challenge — proof-of-possession without a direct TLS connection — is deferred; this is the minimum: require the relayed message to carry a public_key that hashes to the sender_id.)

### Step 1: Read the relay receive path

Read `src/Network/network.c` the RELAY_RECEIVED handler (~line 3600+). Find where `wire_extract_sender_id` is called and where the sender is added to the connection manager / ring set. The relayed message envelope carries a `sender_id` but NOT a `public_key` (per the audit). So the self-consistency check requires either: (a) the relayed message to carry a `public_key` (a wire-format addition to the relay envelope), OR (b) a separate salutation message on the relay path (like the direct path's first message). Read the relay wire format (`wire_relay_received_t` / the relay envelope) to determine the cleanest approach.

### Step 2: Apply the check

If the relay envelope already carries a `public_key` (check the struct), add the `BLAKE3(pubkey)==sender_id` check where the sender is admitted. If it doesn't, the minimum fix is to require the sender to be already authenticated via a direct QUIC connection (the relay is a backup path) — check `connection_manager_lookup` for the sender_id; if not found, require a salutation. If neither is feasible without a wire-format change, document the gap and scope this task as: add the check IF a public_key is present, else require a prior direct authentication. The signed-nonce challenge is the deferred follow-up.

### Step 3: Test + commit

```bash
git add <touched files>
git commit -m "fix(identity): apply salutation check on the relay path

The relay receive path admitted peers via wire_extract_sender_id without the
BLAKE3(pubkey)==sender_id check that direct QUIC connections require — a
relayed peer could claim any sender_id (audit #8). Apply the self-consistency
check on the relay path (require the relayed message's public_key to hash to
the sender_id, OR require a prior direct authentication). The stronger
signed-nonce challenge (proof-of-possession without a direct TLS connection)
is deferred. See audit #8."
```

---

## Task 5: Whole-tier verification

- [ ] Step 1: Build. `cmake --build cmake-build-verify -j$(nproc) --target testliboffs` — expect clean.
- [ ] Step 2: Full test suite. `cmake-build-verify/test/testliboffs` — expect all pass (the fixtures either generate a CA or set `allow_insecure`). No NEW failures beyond the pre-existing SSL cert-loading tests.
- [ ] Step 3: Valgrind sweep. `valgrind --leak-check=full --tool=memcheck --error-exitcode=1 cmake-build-vg/test/testliboffs --gtest_filter='TestPeerVerify.*:QuicIntegration.*:TestWsTransport.*:TestOffsWtClient.*:TestShutdown.*:TestNetwork.*'` — expect `definitely lost: 0 bytes`, 0 invalid reads. Run 2×.
- [ ] Step 4: De-wonk audit.
- [ ] Step 5: TODO check.
- [ ] Step 6: Leak check (covered by step 3).
- [ ] Step 7: Final commit if needed.

---

## Self-Review

**1. Spec coverage.** Tier-5 scope = audit #8, #11.
- #11 (cert validation) → Task 1 (wire when CA present) + Task 2 (fail closed without CA) ✓
- #8 direct (pin salutation to cert key) → Task 3 ✓
- #8 relay (salutation check on relay path) → Task 4 ✓
- Verification → Task 5 ✓

**2. Behavior change.** Task 2 fails closed without a CA. The `allow_insecure` flag is the migration path. Test fixtures must set it (or generate a CA). This is the audit's recommendation; the flag makes it opt-in rather than a hard break.

**3. Interaction.** Task 3 (salutation pin) requires Task 1/2 (cert validation) — otherwise the cert key isn't trustworthy. The pin is a no-op in `allow_insecure` mode (no cert extracted). Task 4 (relay) is independent of Tasks 1-3 (the relay path doesn't have a direct TLS cert to pin to — the self-consistency check is the minimum; the signed-nonce is deferred).

**4. Deferred.** The signed-nonce challenge for the relay path (proof-of-possession without a direct TLS connection) — the audit's stronger fix for #8 relay. This tier applies the self-consistency check as the minimum.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-15-tier5-identity-tls.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, two-stage review between tasks. Best here because the 4 tasks span 6 files + the authority struct + the test fixtures.
2. **Inline Execution** — execute in this session with checkpoints.

Which approach?