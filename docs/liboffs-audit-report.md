# liboffs-prometheus — Correctness & Security Audit

**Scope:** the repo's own code (`src/`, `OFFS/src/`, `tools/`); `deps/` (BLAKE3, libcbor, msquic, googletest, etc.) excluded.
**Driver:** the 5 concerns in `docs/Investigate.md`.
**Date:** 2026-07-14. **Method:** 8 parallel specialist review lanes + direct verification of load-bearing claims.

**Confidence legend:**
- ✅ **verified** — traced in the current code this session (file:line quoted).
- 🟡 **high-confidence** — reported by a review lane with file:line evidence; every lane claim that was independently spot-checked (7/7) proved accurate, but these specific ones were not personally re-traced.
- ⚠️ **suspected** — depends on `deps/`-internal behavior that scope excluded; needs a liboffs-side confirmation.

---

## Bottom line

**It is not "rock solid" for untrusted / public-internet use — but that's not the same as "badly built."** Real hardening has landed since the May `PRODUCTION_BLOCKERS.md` audit (a working bcrypt auth framework, a real Windows platform port, WAL, the actor model, a mostly-sound multi-hop routing core). The problems are concentrated where the code meets **untrusted input and partial failure**:

- **Multiple remotely-triggerable memory-safety bugs** on the QUIC/relay receive path (a heap over-read, a per-message payload leak, an unbounded frame-length allocation, a connection-tracking use-after-free).
- **Multi-hop RPC hangs the caller forever** on the *normal* "block not found" case and on any mid-hop failure — GET has no timeout and no not-found return path past hop 1.
- **NAT hole-punching is unimplemented** — the "relay" only proxies, and the direct-upgrade machinery is dead code. Same-LAN peer discovery doesn't exist.
- **The OFFS CLI lies about success** — `offs get` truncates silently and exits 0; `start`/`stop`/`restart` are fire-and-forget with exit codes that don't reflect reality.

`PRODUCTION_BLOCKERS.md`'s own verdict — *"suitable for trusted-LAN or research use; not ready for public-internet deployment"* — still holds. This audit confirms it and adds concrete memory-safety defects that the May doc did not list. The "rock solid" belief is understandable (a lot of the May blockers really were fixed) but does not survive the untrusted-input paths.

---

## What's genuinely good (so the framing is fair)

- ✅ **Refcounter is correct.** Atomic on all three compilers (`__atomic_*` on GCC/Clang, C11 `atomic_*` on MSVC — the plain field types are fine); textbook ordering (RELAXED increment, ACQ_REL decrement). One subtle smell only: the non-actor YIELD/CONSUME path (`refcounter.c:95-103`) maintains a 3-variable invariant with separate relaxed ops and no transaction — a theoretical double-decrement race if that ownership-transfer path is ever exercised concurrently.
- ✅ **Auth framework is real and enforced** (updates stale blocker #2): bcrypt api-key with an HTTP middleware (`auth_middleware.c:82`), TCP/Unix connection checks (`tcp_connection.c:599`, `unix_connection.c:615`), and block-route gating (`block_routes.c:308`).
- ✅ **Windows port is real** (updates stale blocker #3): actual Win32 File API + named-pipe backend, not the stubs the May doc described.
- 🟡 **Core BlockCache/OFFStreams memory looked clean** as far as the (interrupted) memory lane reached — yield/reference/consume transfers balanced, block-recipe cleanup thorough.
- ✅ **Multi-hop routing core is sound** — TTL underflow is guarded (`find_block.c:159`, `closest_nodes.c:165`, `store_block.c:111`), a top-level 2 MB frame cap and top-level array type-guard exist, and the ClosestNodes protocol handles not-found/return correctly (it's FindBlock that doesn't — see C1).
- ✅ **Streaming wire framing is correct** after the 2026-07-01 refactor (4-byte length prefix via `cbor_serialize_alloc` verified against libcbor's contract).

---

## Findings, ranked by severity

### CRITICAL — remotely triggerable, memory-safety or permanent hang

**1. Heap over-read via attacker-declared length** · `Network/wire.c:1179` (and identical at `wire.c:659`) · concern #1/#3 · ✅ verified
`wire_find_block_response_decode` sizes and copies the block buffer to the CBOR bytestring length (`:1171-1174`), then **overwrites** `block_data_len` with an independent, unvalidated integer from array index 10 (`msg->block_data_len = (size_t)cbor_get_int(data_len)`). The sink `buffer_create_from_pointer_copy(block_data, block_data_len)` (`network.c:1719`) then reads past the short heap buffer → heap disclosure (cached and served to other peers) or crash. Any peer answering a FindBlock triggers it; the intermediate-relay re-encode over-reads again while forwarding. `wire_recall_accept_decode:659` has the same bug. **Fix:** drop the index-10 override; use the bytestring length as the single source of truth (as `wire_store_block_decode:1312` already does).

**2. Every inbound message leaks its payload** · `Network/network.c` synchronous dispatch (~`3098-3398` QUIC_DATA, ~`3629` RELAY_RECEIVED) · concern #2 (the primary leak) · 🟡 cross-confirmed by two independent lanes; allocator premise ✅ verified
Handlers run synchronously against a stack-local `dispatch_msg`; after the switch the code only `cbor_decref`s and never calls `dispatch_msg.payload_destroy`. In the *actor* path the framework frees the payload (`actor.c:91`), but this synchronous path bypasses the actor. Because `get_clear_memory` is `calloc` (`Util/allocator.c:18`, ✅ read), this is a true heap leak on **every** inbound message — and block-carrying types (`find_block_response`/`store_block`/`recall_accept`) additionally leak their 128 KB–2 MB nested block buffer. **This is the dominant answer to concern #2.** **Fix:** call `payload_destroy` after each synchronous handler; ensure `WIRE_FIND_BLOCK_RESPONSE` uses `wire_find_block_response_destroy` (not default `free`) so the nested buffer is freed too.

**3. Unbounded frame-length allocation + integer overflow** · `Network/stream_framer.c:61-89` (feed `:39-59`) · concern #3 · 🟡
`stream_framer_next` reads a big-endian `uint32_t` length (up to 4 GiB) with **no maximum** and buffers until it arrives; a peer can advertise a huge length and slow-drip bytes to pin arbitrarily large allocations (and, combined with finding #2's allocator, drive an `abort()` on OOM = remote crash). On 32-bit, `total_message_size = 4 + length` wraps near `UINT32_MAX`, bypassing the completeness guard; the `new_capacity *= 2` loop can overflow to 0. This framer sits on the **entire** network receive surface. **Fix:** hard-cap frame size (blocks are ≤128 KB; a 1–2 MB cap is safe) and reject oversize; compute sizes with overflow checks.

**4. Connection-tracking double-add → use-after-free** · `Network/quic_listener.c:712` + `:354` · concern #2/#3 · 🟡
Outbound connections are added to the tracking array twice — once in `quic_listener_connect` (`:712`), once on the CONNECTED event (`:354`). On close, `_conn_track_remove` deletes only the first match; the DISCONNECTED handler frees the HQUIC (`network.c:3434`); the stale second slot then gets `ConnectionShutdown` on a freed handle at destroy. Reachable via the offsd config-reload cycle that destroys/recreates the network. **Fix:** let CONNECTED be the single add site, or make add/remove idempotent/duplicate-safe.

**5. Multi-hop FindBlock "not found" never reaches the origin → GET hangs forever** · `Network/network.c:1569`, `:1773-1801` · concern #1 · 🟡 cross-confirmed (two lanes); adjacent dead-upgrade path ✅ verified
The `found==1` branch relays upstream by locating self in `response->path` (`:1688-1712`); the `found==0` branch has **no** upstream relay and the terminal node zeroes the path (`:1569`). So on an A→B→C chain where the block is absent, C's NOT_FOUND dies at B (B has no local wanted-list entry — only the origin registers one, `:2747`). A never learns the search failed; its `wanted_list` entry and the requesting stream actor persist for the process lifetime. There is **no wanted_list expiry and no request timeout** anywhere. The sister protocol ClosestNodes does this correctly (`:964-965`), which proves it's an omission, not intent. **Fix:** preserve the path on terminal not-found; add the same self-in-path upstream relay to the `found==0` branch; deliver to the local wanted_list only at the origin.

**6. ClosestNodes correlation: colliding message IDs + no timeout** · `Network/network.c:1204` (also `2373`, `2753`, `3476`) · concern #1/#3 · 🟡
`message_id = time(NULL) * 1000` is **second-granularity scaled to look like milliseconds** — two queries in the same wall-clock second get identical IDs. `closest_pending` is keyed only on `message_id`; the remove returns the first match, so colliding queries cross-deliver and orphan an entry. And there is **no timeout sweep**: a lost response or dead hop means the `reply_to` actor is never signaled → caller hangs forever. **Fix:** monotonic per-node atomic counter for IDs; per-pending deadline swept by an existing tick that delivers a timeout result.

**7. Stack payload handed to async actor → use-after-return** · `Network/nat_detect.c:251-271` · concern #3 · 🟡 (also flagged by the NAT lane)
Two `wire_addr_request_t` live on `nat_detect_start`'s stack; `msg.payload` points at them with `payload_destroy = NULL`. `actor_send` shallow-copies the struct (keeping the pointer) and enqueues for another thread; by the time `relay_client.c:707` dereferences it, the stack frame is gone → UB read of a reclaimed frame (usually "works" because the page is still mapped, which is why testing misses it). **Fix:** heap-allocate each payload with a freeing `payload_destroy`.

**8. Node identity not bound to the TLS credential → impersonation** · `Network/peer_verify.c` + `network.c:435-445` (relay path `:3604`) · concern #3/security · 🟡
The `node_id = BLAKE3(pubkey)` binding is checked, but nothing ties `salutation.public_key` to the key MsQuic authenticated in the handshake, and there's no proof-of-possession. Any CA-admitted node can lift a victim's public key from a gossiped (cleartext) `peer_info` blob and present `sender_id = BLAKE3(Kv)` to be accepted **as the victim** → routing-table poisoning, interception of that node's FindBlock/StoreBlock. Relayed peers are worse: the relay receive path admits peers via `wire_extract_sender_id` **without** the BLAKE3 salutation check that direct QUIC connections require. **Fix:** pin `salutation.public_key` to the verified leaf-cert key, or use a signed-nonce challenge before trusting `sender_id`; apply the same check on the relay path.

### HIGH

**9. No timeout on pending finds** · `Network/wanted_list.c` (no expiry), forwarding return values ignored (`network.c:1633`, `:1701`, `:2857`) · concern #1 · 🟡 cross-confirmed
`conn_state_send` returns −1 when a peer's stream is gone, but every relay call site ignores it. A mid-request hop death or dropped datagram silently evaporates the response; the origin's entry and stream never resolve. **Fix:** check `conn_state_send`; fail the pending request when the next hop is unreachable; add the sweep from #6.

**10. First-response-wins aborts a still-live search** · `Network/network.c:1748` vs `:1781` · concern #1 · 🟡
FindBlock fans out to up to 3 next hops but the origin acts on the *first* response. A dead-end neighbor's NOT_FOUND removes the wanted entry and fails the stream before a live branch's found=1 arrives (which then finds no entry → block cached but GET already failed). **Fix:** treat found as the only terminal success; treat not-found as terminal only after all branches report or a timeout.

**11. TLS certificate validation still disabled** · `ClientAPI/WT/wt_transport.c:587`, `ClientLibs/c/offs_client.c:1255`, `quic_listener.c` fallback · concern #4/security · ✅ verified
`QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION` is still set on the WebTransport server path and the WT client, and `quic_listener` falls back to it when no CA is configured. TLS encrypts but does not authenticate on these paths → MITM. Critically, this undermines the (real) bcrypt auth: the api-key travels over a transport whose peer isn't authenticated. **Fix:** fail closed when no CA is configured; load a client-flagged credential for outbound; wire `peer_verify.c` into WT.

**12. Inbound rate limiting is never enforced** · `Network/rate_limit.c:132` (`rate_limit_check` has zero callers) · concern #3 · 🟡
The token-bucket limiter exists but is never called on the receive path; a peer can flood FindBlock/StoreBlock unthrottled. **Fix:** call `rate_limit_check` keyed on the authenticated sender at the top of the QUIC/relay handlers; send `wire_rate_limited` and drop on reject.

**13. CBOR decoders lack type guards on nested items** · `Network/wire.c` (`_node_id_decode:29`; hash reads `231/474/575/1029/1142/1273/2082`; `visited`/`path` loops) · concern #3 · 🟡
Typed getters are called on nested items with no preceding `cbor_isa_*` check. The top-level array is guarded, but nested items are attacker-controlled — a type-confused item makes libcbor read the wrong union member → wild-pointer deref → remote crash. Half the decoders already guard correctly, so it's an inconsistency. **Fix:** add `cbor_isa_*` before every typed extraction, uniformly.

**14. Per-peer tables grow unbounded on wire-controlled keys** · `Network/hebbian.c:40`, `eabf.c:89`, `rate_limit.c:56` · concern #2/#3 · 🟡
No pruning, and the keys are node_ids taken off the wire — one malicious peer can mint fake node_ids (≤6 per message, unbounded across messages) and force unbounded inserts (each EABF entry ≈2 KB + O(n) scan). **Fix:** bound each table (LRU/weight eviction); call `*_remove` on connection teardown.

**15. `offs get` silently truncates and exits 0** · `OFFS/src/offs/commands/get.c:77-103` · concern #5 · ✅ verified
The frame loop exits on GET_END, an ERROR frame, **or** a NULL frame (disconnect / the 30 s recv budget expiring) — and all paths fall through to `return 0`. No `saw_end` flag; `fwrite` (`:83`) return unchecked. So a mid-stream daemon death or slow fetch yields a truncated file and a success exit code; a server ERROR is printed to stderr but still exits 0 (knowingly shipped — the streaming plan lists it as out-of-scope). **Fix:** track GET_END receipt; return 1 on NULL-without-END and on ERROR; check `fwrite`.

**16. `offs start` always reports success** · `OFFS/src/offs/commands/start_stop.c:48-65` · concern #5 · 🟡
The parent forks offsd and unconditionally prints `"Daemon started (PID: N)"` / returns 0 — no wait, no probe. If `execvp` fails (offsd not on PATH) or offsd can't bind, you get "started" + exit 0 + no daemon. And because offsd double-forks, the printed PID belongs to the intermediate process that already `_exit(0)`'d. **Fix:** briefly poll `cli_client_connect` after fork; report the true result; read the real PID from the daemon's PID file.

**17. No double-start guard** · `start_stop.c:18-66` + `offsd/main.c:945` · concern #5 · 🟡
`cmd_start` never checks `_is_daemon_running()`; `_write_pid_file` takes no lock. `offs start; offs start` races two daemons for the port/socket/PID file (last-writer-wins). The `L10N_DAEMON_ALREADY_RUNNING` string exists but is used nowhere. **Fix:** check before spawning (or `O_EXCL` PID lock); wire up the existing string.

**18. NAT hole-punching is unimplemented; the relay only proxies** · `Network/Relay/relay_server.c:204`, `peer_connection.c:30`, `nat_detect.c` · concern #4 · ✅ core claims verified
The relay's only data path is look-up-dest-and-forward (`relay_server.c:204`) — no punch coordination, no candidate swap, no direct handoff; every non-public connection rides the relay forever (a bandwidth sink). The relay→direct upgrade state machine is **dead code**: `CONN_STATE_TRY_DIRECT`/`DIRECT_CONNECTED` have zero producers (✅ verified — only enum defs + `case` labels exist), peers are pinned to `CONN_STATE_RELAY` at create. NAT detection **never runs**: `nat_detect_start` has no caller (✅ verified), can't complete (the relay client swallows `ADDR_RESPONSE` keeping only the endpoint id, `relay_client.c:128`), and its output is never consumed. A node **advertises no address of its own**: `peer_info_from_node()` was specced but never implemented (✅ verified — exists only in the design doc), so `peer_handlers.c:39` sends `addresses = NULL`. Also: relay forwarding is unauthenticated/unmetered with guessable endpoint ids (`relay_server.c:364`) → amplification vector. **This is a feature to build, not a regression to fix** — hole-punching was never actually specified. See the design recommendation below.

### MEDIUM

- **19. `offs restart` can leave no daemon running** (`start_stop.c:103` — fixed 1 s sleep < graceful drain; new bind fails; still exit 0). 🟡 · #5
- **20. Fire-and-forget commands lie** — `friend add/remove` print "OK"/exit 0 even when the daemon is unreachable (NULL response); `peer connect`, `block delete`, `block get`, `health`, `status` treat any/ERROR response as success (`friend.c:33`, `peer.c:81`, `block.c:106`, `health.c:44`). 🟡 · #3/#5
- **21. StoreBlock decline responses dropped** (`network.c:2125` — no `else` for `accepted==0`; `replicas_remaining` never aggregated) → PUT reports success with 0 replicas actually placed. 🟡 · #1
- **22. Multi-hop reach silently capped at 3** (`network.c:2755/2375/1210` set initial `ttl = FORWARD_FANOUT` = 3, conflating branch-width with hop-limit; `MAX_PATH` = 6). 🟡 · #1
- **23. FindNode ignores `target_id`** (`network.c:1381`) and returns no result to the caller — effectively a ring-population probe, not a query. 🟡 · #1
- **24. `offs stop`/status use process-name identity** (`start_stop.c:68-97`) — ignores the PID file (stopping one daemon kills both; `pkill` lacks `-x` so matches `offsd-helper`); Windows `sc query` detects *installed* not *running*. 🟡 · #5
- **25. Service install/uninstall stubbed on macOS + Windows** (`service_macos.c:29`, `service_windows.c:114` return ok without creating a plist/service) → the updater "installs" nothing. 🟡 · #5
- **26. No Prometheus metrics format** (`OFFS/src/offs-metrics/` — cJSON `/status` + CBOR `/report`, no `# HELP`/`# TYPE`/`/metrics` exposition anywhere despite the "prometheus" name and port 9090). ✅ verified · ops
- **27. latency_cache purges itself on first sweep** (`latency_cache.c:50` — `timestamp_ms` set to 0 on insert, eviction computes `now − 0`). 🟡 · #3
- **28. Predictable node identity** (`node_id.c:60` — `rand()`-based). 🟡 · security
- **29. Respiration exhale has no watchdog** (`respiration_actor.c:89` — a lost result pins state non-IDLE; the node can never shed blocks again). 🟡 · #3

### LOW

- **30. Order-rigid global flags** — `offs status --socket /x` silently connects to the **default** socket (`main.c:20`). 🟡 · #5
- **31. Malformed metrics ack** — `metrics_server.c:135` writes 14 of a 15-byte string → returns `{"status":"ok"` (missing `}`). ✅ verified · ops
- **32. `put.c` flags silently dropped** at end-of-argv or when unknown (`put.c:37`). 🟡 · #5
- **33. Assorted** (from the network lane, 🟡): `peer_info.c` loop-bound decrement drops a valid address + duplicate-key leak; `topology_report.c:230` off-by-one key checks; `timing_wheel_init` no `%0` guard; `hebbian_config.h:7` `rpc_multipliers[20]` indexed by WIRE types up to 33 (OOB read); `wanted_list.c:16` bloom never rebuilt despite its comment.

---

## Answers to the 5 Investigate.md concerns

1. **Multi-hop RPC completeness** — Routing core is sound, but *failure and return-path handling is incomplete*: not-found can't propagate past hop 1 (#5), no pending-request timeout (#6, #9), first-response-wins (#10), TTL capped at 3 (#22), StoreBlock replica accounting dead (#21). A normal "block absent" GET or any peer churn hangs the caller.
2. **Memory leaks** — The core engine looked clean, but the **network receive path leaks a payload (often a whole block) on every inbound message** (#2) — that's the big one. Plus unbounded per-peer tables (#14) and hung requests that never free their state (#5, #9).
3. **Bugs** — A heap over-read (#1), an unbounded-allocation/overflow (#3), a use-after-free (#4), a use-after-return (#7), missing rate-limiting (#12), CBOR type-confusion (#13), and a pile of CLI silent-failure bugs (#15–17, #19, #20, #24, #30).
4. **Relay NAT efficacy + same-LAN** — The relay does **not** punch holes; it proxies forever, and the direct-upgrade path is dead code (#18). Same-LAN doesn't work at all. **Design answer below.**
5. **OFFS CLI ↔ offsd lifecycle + streaming** — Functional on the happy path but riddled with silent-failure/exit-code-lie bugs (#15–17, #19, #20, #24, #25, #30); the streaming *framing* is correct, the *command-level* EOF/error/exit handling is not.

---

## Design recommendation — same-LAN connectivity & internal IPs (concern #4, Part B)

**Should you include internal/private IPs in the connection info? — Yes, but typed as ICE-style candidates, friend-gated for privacy, and paired with mDNS. Do not naively broadcast them.**

Today two nodes behind the same NAT cannot auto-connect (neither advertises a LAN address; there's no mDNS; hairpinning would be the only path and is often broken), so same-LAN traffic falls back to bouncing through an internet relay. Adopt the ICE candidate model — the raw materials already exist (the relay is TURN-ish, `ADDR_RESPONSE` is STUN-ish); what's missing is candidate *exchange* and connectivity checks:

1. **Type the candidates** — split `peer_addr_type_e` (`peer_info.h:16`, currently just `DIRECT`/`RELAY`) into `HOST` (private/LAN), `SRFLX` (server-reflexive, learned from the relay), `RELAY`. The CBOR carrier already serializes `{type,host,port,relay_id}`, so this is an enum + priority addition, not a wire-format change.
2. **Populate local candidates** — implement the missing `peer_info_from_node()` and call it from `peer_handlers.c:39`: enumerate up interfaces for RFC1918/link-local → `HOST`; insert the relay-learned reflexive addr → `SRFLX` (finally *consuming* the value `relay_client.c:129` currently logs and drops); insert the relay endpoint → `RELAY`.
3. **Try candidates in priority order** at the three connect sites that currently pick "first DIRECT" (`peer_handlers.c:122`, `network.c:2879`, `:2920`), preferring a `HOST` candidate that shares the local subnet (the same-LAN fast path). Reuse the unused `conn_path_t.reflexive_addr/port` (`conn_state.h:35`) — **watch the byte-order trap**: the relay emits reflexive addr in host order (`ntohl`, `relay_server.c:161`) while `conn_path_t.addr` is network order.
4. **Privacy** — only include `HOST` candidates in `peer_info` shared with **authenticated friends**, never in DHT gossip or a public `/peer/info`; broadcasting internal IPs to arbitrary peers is a reconnaissance leak.
5. **Prefer mDNS for same-LAN** — a small responder on 224.0.0.251 / ff02::fb advertising `node_id` + LAN IP + QUIC port keeps private addresses on-link (they never traverse the WAN even for discovery) and sidesteps broken hairpinning. On seeing an on-link matching `node_id`, short-circuit to a direct LAN dial.
6. **For cross-NAT**, close the loop: add relay `PEER_CONNECT`/`PUNCH` signaling to swap `SRFLX` candidates + trigger a synchronized simultaneous-open, and feed `nat_detect`'s verdict into `conn_state` so symmetric→relay-only and cone→punch actually take effect (all currently dead — #18).

---

## Coverage & caveats (read this before trusting completeness)

- **Concurrency has now been audited** (see `concurrency-pass.md`). Headline: the actor core (mailbox, Chase-Lev deque, scheduler) is sound, but there are **three confirmed Critical thread-safety bugs** — a refcounter YIELD/CONSUME window race (double-free/leak; this is the race flagged as ✅ in finding-legend terms, `refcounter.c:95-131`), `sections_dispatch` running on two actor threads at once, and a shutdown-vs-disconnect UAF on `conn_mgr` — plus four High teardown races (mailbox-enqueue-after-destroy, worker re-queue of a freed actor, and relay/WT transport destroy-before-quiesce). One correction to this report and to `ARCHITECTURE.md`: the documented Cache→Index→Section→LRU lock order is **fiction** — BlockCache holds no locks (it is fully actor-based), so no ABBA deadlock exists, but nothing enforces the actor-isolation the shared structures rely on.
- **The core-engine memory lane was also interrupted** — the network payload leak (#2) was found and the core looked clean, but core leak coverage is not exhaustive.
- **`deps/` was out of scope.** A few OFFS findings (⚠️) depend on liboffs internals not audited: the daemon-side `GET_DATA` chunk size (drives the 16 MB recv-cap truncation risk, `client.c:19`) and `platform_socket_send` SIGPIPE flags (a broken pipe during a large `put` could kill the CLI with exit 141).
- **Process note:** two review lanes recursively spawned their own sub-agents, which burned the session token budget mid-run. Their findings are folded in and attributed; this affected cost, not validity — every claim spot-checked (7/7) was accurate.

---

## Suggested fix order

1. **Memory-safety criticals first** (#1 over-read, #2 payload leak, #3 frame cap, #4 double-add UAF, #7 stack UAF) — remote and exploitable.
2. **Multi-hop hangs** (#5, #6, #9, #10) — they make GET unreliable even on a healthy network.
3. **Identity & auth-over-TLS** (#8, #11) — required before *any* untrusted deployment; the bcrypt auth is undermined until TLS validates peers.
4. **CLI exit-code lies** (#15–17, #19, #20, #24) — cheap, high user-facing value, prevents scripts acting on corrupt/absent data.
5. **NAT/relay** (#18 + design above) — a feature build; schedule it only if internet-P2P (not trusted-LAN) is a goal.
