# Network-Layer Functionality Audit & Live Verification Plan

Audit date: 2026-10-23 · liboffs `a96865a` (exhale verify-only fix) · OFFS `31c9326`
Lab: **A** = Windows 11 @ 192.168.1.212 (installed `OFFS Daemon` service, MSI 0.1.256)
**mycroft** = Pop!_OS @ 192.168.1.198 (`systemctl` offsd, deb 0.1.1 / liboffs a96865a)

## 1. What touches the network (handler inventory)

Every network-facing code path, by family. 48 local handlers, 30 wire types,
11 local-only families, 3 transport stacks.

### 1.1 Wire RPC family (CBOR over QUIC streams, dispatched in network.c)

| Wire type | Request sender (production) | Response handler | Live-verifiable |
|---|---|---|---|
| `WIRE_SALUTATION` | on connect accept (quic_listener) | `~1103` flips `peer->connected=true` + registers | ✅ already verified both directions |
| `WIRE_PING` | send site @ network.c:1430 | ping_response + latency cache | ✅ via connected-peer tick |
| `WIRE_PING_RESPONSE` | (reply) | latency_cache/hebbian update | ✅ rides along |
| `WIRE_PING_CAPACITY` | :1197 (probe) + :1372 (`network_handle_ping_capacity_tick`) | capacity response → store_block accept probability | ✅ tick-driven |
| `WIRE_PING_BLOCK` | **NO production sender** — reply handler only | — | ❌ dead unless a caller sends it |
| `WIRE_FIND_NODE` | :6040 (`network_handle_local_closest_nodes` path) | find_node_response → ring/EABF population | 🟡 needs a seek trigger |
| `WIRE_FIND_BLOCK` | wanted-list routing (`find_block_state`) | find_block_response | ✅ (the GET path — verified) |
| `WIRE_STORE_BLOCK` | **not observed as a request sender** (put stores locally; cross-node blocks arrive via SEEK/fetch) | store_block_response | 🟡 acceptance logic only reachable in tests unless a store send is forced |
| `WIRE_SEEKING_BLOCKS` | **NO production sender** (response-only encoding observed) | seeking_blocks_response | ❌ dead request path |
| `WIRE_RANK_BLOCK` | :4349 (seek loop ranks a discovered block) | rank_block decode | 🟡 rides on SEEK |
| `WIRE_RECALL_BLOCK` | :4039 (recipe pull across nodes) | recall_accept/decline | 🟡 cross-node stream recall |
| `WIRE_RECALL_ACCEPT/DECLINE` | (replies) | recipe block delivery | 🟡 same trigger |
| `WIRE_RATE_LIMITED` | `network_send_rate_limited` | peer throttles sender | ✅ under load |
| `WIRE_CLOSEST_NODES` / `_PROGRESS` / `_RESPONSE` | :2504 / :2830 | ring lookup progress | 🟡 needs a closest-nodes query |
| `WIRE_MEASURE_NODES` | **NO production request sender** (response encoder exists) | measure_nodes_response | ❌ dead request path |
| `WIRE_GOSSIP` / `WIRE_GOSSIP_PULL` | gossip tick (:2288) | `NETWORK_GOSSIP_RECEIVED` / pull | ✅ tick-driven |
| `WIRE_RELAY_PUNCH` | nat path | hole-punch completion | 🟡 needs NAT/relay (relay box currently down) |
| `WIRE_RELAY_CHALLENGE` / `_RESPONSE` | relayed-sender verification | accept/drop relayed messages | 🟡 needs a relay path |

### 1.2 Local orchestration family (actor messages, never on the wire)

- `NETWORK_LOCAL_FIND_BLOCK` / `_CANCEL` / `NETWORK_FIND_BLOCK_RESULT` — the
  requester lifecycle (wanted list, stream-owner references, deadline sweep,
  destroy-time prune). **Now carries `verify_only` for exhale sheds** (a96865a).
- `NETWORK_LOCAL_STORE_BLOCK` / `NETWORK_STORE_BLOCK_RESULT` — respiration
  storing phase + stream-origin stores; `store_block_should_accept` decides.
- `NETWORK_LOCAL_FIND_NODE` / `NETWORK_LOCAL_CLOSEST_NODES` / `_RESULT`,
  `NETWORK_CLOSEST_NODES_PROGRESS` — ring lookup plumbing.
- Timer families: `NETWORK_REQUEST_TIMEOUT_TICK` (wanted-list sweep + relay
  challenge sweep + Hebbian decay gate), `NETWORK_GOSSIP_TICK`,
  `NETWORK_GOSSIP_EXPIRE`, `NETWORK_PING_CAPACITY_TICK`, `NETWORK_EABF_EXPIRE`,
  `PEER_BOOK_RECONNECT_TICK`, `PEER_BOOK_RECONNECT`.
- Respiration: `RESPIRATION_EXHALE_TRIGGER` (block_cache ≥0.8) →
  VERIFYING → (redundant→delete→STORING→accepted→CACHE_REMOVE) /
  (not-found→preserved) with watchdog.
- `RELAY_CLIENT_SEND/ADDR_REQUEST`, `NETWORK_RELAY_RECEIVED`,
  `NETWORK_ADDR_RESPONSE`, `NETWORK_NAT_TYPE_DETECTED` — relay/NAT lane.
- `NETWORK_QUIC_CONNECTED/_DISCONNECTED/_DATA`, `NETWORK_SHUTDOWN_CONNECTIONS`,
  `QUIC_LISTENER_*` — transport events.

### 1.3 Support layers under the handlers

connection_manager (EABF wells, Hebbian weights, decay + **prune only
!connected peers**), peer_book (persistence, reconnect ring), ring_set/ring
(walk, roulette, zero-id poison fix), wanted_list (requester refs, fanout and
not-found accounting, verify_only), rate_limit, latency_cache, message_log
(queryable send/receive ledger), topology metrics, store_block policy.

## 2. Verification matrix (what we run between this machine and mycroft)

### Phase P1 — Connectivity lifecycle (already exercised; re-run as a gate)

1. `offs status` both machines → peer_count 1, exactly one direct peer.
2. Service restart on A → reconnect within `PEER_BOOK_RECONNECT_TICK`;
   salutation flips `connected` (log `peer connected=true` + `/api/v1/peers`).
3. Stop offsd on mycroft 2 min → A must mark disconnected (no stale
   `connected=true`), Hebbian decay keeps the row (prune only for
   never-connected peers); restart → reconnect restores state.
4. Fresh config with 3 fake peers → they connect never → decay prunes them;
   the real peer survives 100+ decay ticks (unit: `DecayTickNeverPrunesLivePeers`).
5. Ring zero-id poison: no zero-id ring entries after a gossip exchange (fixed
   in f69bec1; spot-check log for `ring` inserts with id 0).

### Phase P2 — Wire RPC sweep

For each ✅ entry in §1.1: trigger it once with both nodes' logs captured
(`/api/v1/message-log` style probes or file logs) and assert the paired
response arrived and updated its consumer (latency cache, capacity
probability, EABF). For 🟡 entries the trigger is a data flow (P3). For ❌
entries: decision needed — wire the sender (e.g. periodic `MEASURE_NODES` for
latency-aware routing) or delete the dead handler.

### Phase P3 — Data flows

1. **GET cross-node both directions** — regression: penguin.mp4 (3,847,371 B,
   SHA256 e36fd07d…) mycroft→A and putA.bin (524,288 B, 7412f325…) A→mycroft,
   byte-for-byte re-verified today.
2. **PUT (stream write → local section store)** — PUT does NOT push while a
   node has room (store-path theory test: zero wire store sends at 524 KB);
   assert block count + sections on the origin only.
3. **SEEK /inhale** — PUT new content on A; mycroft's capacity<0.5 inhale
   must discover + fetch it (peer SEEK → rank → find). Watch `find:` logs.
4. **Cross-node recall (recipe pull)** — read a large OF whose blocks only
   exist on the peer: exercises `WIRE_RECALL_BLOCK`/ACCEPT/DECLINE delivery.
5. **CLI network-aware GET** (named-pipe local RPC + network legs).

### Phase P4 — Respiration (this is the fix just deployed: a96865a)

Both nodes run at small capacity → always inhaling (<0.5). To exercise
**exhale** we need capacity ≥0.8:

1. Temporarily set `cache_size` to a small value in both configs (e.g. just
   above current usage) → restart → capacity >0.8 → exhale fires.
2. **Verify-only check (the new behavior)**: pick a shed candidate known to
   exist ONLY on the local node → the VERIFYING find must NOT answer from the
   local index (bug fixed: it previously always said found → deleted last
   copies). Log expectation: the exhale search goes to the wire.
3. **Redundant path**: a block that also exists on mycroft → found → the
   respiration actor deletes the local copy → STORING → local store declines
   (≥0.8) → CACHE_REMOVE. **Assert capacity actually drops** — under the old
   bug the found-response re-downloaded the block and refilled the shed, so
   capacity never decreased. This is the regression this fix is judged on.
4. **Not-found path**: unique block → preserved, logged.
5. Then restore the original cache_size.

### Phase P5 — Fault injection

1. Peer goes down mid-GET → wanted-list deadline sweep delivers found=0 →
   stream error, no hang (watchdog + inline SHUTDOWN_COMPLETE already tested).
2. Replay a stale not-found (different message_id) → origin must ignore it
   (`message_id` fence on wanted entries).
3. Garbage CBOR frames from a hostile peer → session throttled via
   `WIRE_RATE_LIMITED`-adjacent handling; no crash (fuzz-ish check with the
   tcpmon script).
4. Self-connect attempt → rejected (loopback), no ring poison.
5. Quic receive pause/consumed contract — covered by test suite
   (`QUIC_STATUS_PENDING`; returning SUCCESS double-drained frames).

### Phase P6 — Relay / NAT (gated on the relay box — currently down)

Bootstrap-connect via the Azure relay node, `WIRE_RELAY_PUNCH` cross-NAT,
relay challenge/response verification, RELAY_RECEIVED addr learning.
Blocked; revisit when the relay node returns.

## 3. Dead/unclear paths needing a decision

| Path | Status | Options |
|---|---|---|
| `WIRE_PING_BLOCK` request | no sender anywhere | delete or wire into health checks |
| `WIRE_SEEKING_BLOCKS` request | only responses encoded | confirm intent; wire the SEEK broadcast or remove the handler |
| `WIRE_MEASURE_NODES` request | response path exists, request never sent | wire periodic measurement or delete |
| `PEER_SEND_SEEKING_BLOCKS` / `PEER_SEND_PING_BLOCK` | message types unused | prune |
| `PEER_UPDATE_HEBBIAN` | no production sender (weights only decay) | by design? (conn weight growth comes from direct sends) |
| `STORE_BLOCK` request | origin never stores to net while it has room | confirm the seek/fetch path is the intended distribution channel |

## 4. Tooling available for the verification runs

- Two-machine live lab, SSH (paramiko) to mycroft; MSI service locally.
- Log lines shipped for the hunt (`find:`, `find:rx`, `find:rxresp`,
  `quic:send`, `quic:send_complete`) — still present pending sweep.
- Crash minidump tooling (offsd-crash.dmp + parser stack in %TEMP%\diageo).
- `docs/GC_KEEP_FILTER_SIZING.md` documents sizing; the exhale test should
  account for the GC keep filter when filling to 0.8.

## 5. Verification status (live run of 2026-10-23)

**Executed live between A and mycroft (liboffs a96865a → c583fca, OFFS b453201 → a7956e2):**

- **P1 ✔** restart → reconnect → salutation flip verified three times (once as part of each MSI upgrade); `offs peer list` tracks the direct peer; `status.peer_count` remains the metrics-quirk (use `/api/v1/peers` or `offs peer list`).
- **P3 ✔ (data flows)** — byte-exact cross-peer GET verified on the final builds in BOTH directions: A→mycroft 262,144 B (SHA `d84a82e6…`) and mycroft→A 131,072 B (SHA `da8db8e8…`), plus re-verified `verify_putA2` and `verify_dirTest` after the shed had started. NOTE: the Windows CLI's stdout is O_TEXT — `offs get <url> > file` via cmd/PS redirect text-mangles binary; use `--output <path>`.
- **P3 one incident** (P5-class): a GET issued during a daemon restart raced the connection teardown — `conn_state_send` returned success into a dying QUIC connection and the find was lost (the requester then waited out the wanted deadline). Recorded as an open P5 item: sends should fail fast when the stream is closing.
- **P4 ✔ (respiration) — and it found four real bugs, all fixed + deployed:**
  1. `verify_only` exhale verification (a96865a) — the shed's verify answered from the LOCAL cache (always found) and re-downloaded results (refill).
  2. Exhale never evaluated at startup (e75c443) — `block_cache_update_capacity` ran inside `block_cache_create` before `network_create` wired the respiration actor; nothing re-evaluated until the next put (add: re-run after wiring).
  3. offsd passed `NULL` for the cache's authority (OFFS b453201) — `block_cache_update_capacity` early-returned forever in the production daemon; respiration AND authority phase/capacity were dead outside tests.
  4. Verify flood volume (ac44e09) — 18k candidates × 128 KB verify-downloads ≈ 2.3 GB; responses outran the 30 s watchdog (≈18 solved/cycle). Fix: optional 11th CBOR element carries `verify_only` end-to-end; suppliers answer metadata-only FOUND and skip the block serve.
  5. Watchdog reset didn't re-trigger (512c534) — after a failed cycle the node sat IDLE above the threshold with nothing to re-arm; added `CACHE_CAPACITY_REFRESH`.
  6. Batch cap (581392e) — one cycle carried EVERY candidate (rate-limited wire finds at ~50/s made mass expiry inevitable); each cycle is now 40 candidates (under the ~60-token FIND_BLOCK burst budget), progressed by the deletes' own re-evaluations.
  7. **Loop-back self-verification (dcea34f)** — the origin never marked itself visited, so a peer forwarded the verify find back to the origin, whose supplier answered found=1 against its OWN copy: the shed deleted last copies verified against themselves (40 live deletions before the catch; all shared test files re-verified intact afterwards). Fixed: origin enters itself in the visited bloom; a supplier that sees a verify-only find addressed to itself answers nothing.
  8. **Data-less store-then-delete (b0a319e, a445dcd)** — the local store path announced blocks WITHOUT data; the supplier accepted by capacity rules and stored nothing; the shed deleted each block the moment the acceptance arrived (the −40). Now the exhale's stores carry the bytes (read back from the LRU or sections, hash-verified); the block store skips entirely when bytes aren't readable; the store-carry buffer ownership follows the find-serve pattern. One ownership bug in the first cut caused `c0000374` heap corruption on every boot (the carry path runs at every first shed) — fixed in a445dcd.
  9. **IDLE-before-refresh (c583fca)** — the completion path asked the cache to re-evaluate before publishing state=IDLE; the trigger's IDLE check lost the race and a fast batched cycle could stall the shed forever. Disarm + IDLE publish now precede the refresh.
- **P4 live result**: A sheds 18,287 → 18,193 blocks (−512 KB verified on `current_bytes`) while mycroft grows 236 → 290 (+54 blocks RECEIVED) — store-then-delete transfers real bytes before deletion. The shed runs autonomously toward the 50% target at ~40 blocks / ~2 min.

**Open items surfaced by this run (user decisions or next steps):**
- The rate limiter is built but **never enforced** on any send/receive path (no caller of `rate_limit_table_find` in network.c, no `WIRE_RATE_LIMITED` sender) — either wire it (P5) or delete the dead table + handler.
- Preserved (not-found) candidates keep their ejection_date, so failed candidates re-verify at the head of every batch until stored; acceptable while stores succeed, but a peer that declines everything pins the shed on the same 40 forever. An ejection-date pushback on preserved candidates would fix the pathological case.
- journald rate-limits mycroft's log during flood-style operation (log gaps look like missing traffic — do not chase).
- The Windows daemon's `stop` remains wedge-prone with the relay down (watchdog force-exit covers it; the SCM stop returns before the daemon finishes).
- Hunt diagnostics (`find:`/`find:rx`/`find:rxresp`/`quic:send*`) still in tree — sweep-commit pending user's call.

## 6. Tracking

Harmony epic **"cross-node network-verification matrix"** is the intended
tracker — the CLI here needs `HP_API_KEY` in env/.env to bootstrap; provide
it and the phases above become tickets P1…P6.