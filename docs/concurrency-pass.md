---
# liboffs-prometheus — Concurrency Pass (addendum to the main audit)

Companion to `liboffs-audit-report.md`. Dedicated thread-safety review of the actor core, refcounter, scheduler, and teardown paths. Read-only; `deps/` excluded. Date: 2026-07-14.

**Confidence:** ✅ verified (cross-checked against the code this session) · 🟡 high-confidence (traced with file:line + a concrete interleaving; not independently re-traced).

## Verdict

The actor core itself — the MPSC mailbox, the Chase-Lev work-stealing deque, and the scheduler idle/wakeup machinery — is carefully built and largely sound. The problems are in the layers **above** it: the refcounter's non-actor ownership-transfer path is genuinely racy, `sections` state is dispatched from two actor threads at once, and teardown paths repeatedly free state while MsQuic callback threads or pool workers can still reach it. Important correction to the main report: the lock order documented in `ARCHITECTURE.md` (Cache→Index→Section→LRU) is **fiction** — BlockCache uses zero locks (it is fully actor-based). So there is no ABBA deadlock — but there is also no lock discipline protecting the shared structures that multiple threads *do* touch; correctness rests entirely on actor-isolation assumptions that several call sites violate.

## Findings (most severe first)

### F1 — CRITICAL ✅ — Refcounter YIELD/CONSUME window race → use-after-free / leak
The non-actor path maintains the `yield`/`pending_deref`/`count` invariant with separate relaxed atomic loads + independent RMWs and no transaction: `refcounter.c:95-103` (reference), `:126-131` (dereference), `:155-161` (dereference_is_zero). `yield`/`pending_deref` are `uint8_t` (`refcounter.h:35-40`), so underflow wraps to `0xFF`.
- **Double-adopt underflow → UAF:** with `yield==1`, two threads both `refcounter_reference` the same object; both load `yield=1` (`:95`) before either `fetch_sub` (`:96`) → yield 1→0→`0xFF`, two acquisitions consume one escrow slot, `count` under-counts live holders → a later release frees the object while a holder remains. The poisoned `yield=0xFF` is sticky (every later deref leaks, every later ref applies phantom `count--`).
- **Stranded pending_deref → leak:** a `dereference` commits to `pending++` after loading `yield=1`, but an adopting `reference` on another thread consumes the yield and checks `pending` before that `++` lands → the release is never applied.
- **Live cross-thread caller:** `block_recipe.c:40-42` — `block_cache_put` references the block on the recipe thread (`block_cache.c:796`) and `actor_send`s it (`:806`); the block_cache actor then runs CACHE_PUT on another worker, doing `refcounter_reference` in `block_lru_cache_put` (`block_cache.c:164`, the double-adopt racer) and `block_destroy` (`:389`, the stranded-pending racer) — while the recipe thread's `CONSUME(block)` + `stream_notify`'s adopting reference (`streams.c:385`, after a malloc at `:363`) hold `yield=1` open. `_block_cache_resolve_pending_gets` (`block_cache.c:295`) also hands one block to multiple stream actors.
- **Why not a race (rejected):** the mailbox push/pop gives release/acquire only for the message hand-off, not for these operations on an object reachable via an earlier message. The mutex `#ifndef OFFS_ATOMIC` path would be safe, but `OFFS_ATOMIC` is unconditionally defined (`refcounter.h:18`), so the racy path is the one that ships.
- **Fix:** make the escrow transfer a single atomic transaction — pack `{count,yield,pending}` into one word manipulated by CAS, or take the yield via `compare_exchange` (expected=n, desired=n−1) so only one adopter wins a slot.

### F2 — CRITICAL 🟡 — `sections_dispatch` executes on two actor threads concurrently; all `sections` state is unsynchronized
Site A: `block_cache.c:367` (and sync CACHE_GET `:453-460`) — the block_cache actor calls `sections_dispatch` **synchronously**. Site B: `sections.c:460-503` — `sections_read/write/deallocate` `actor_send` to the **sections actor** (a different worker; queued by the async CACHE_GET path at `block_cache.c:450-451`). Both mutate `sections->lru` (`sections.c:201/208/251/258`), `sections->robin` (`:198/260`, whose comment claims "only called from sections_dispatch"), and `section_t` objects incl. file I/O. Interleaving: block_cache actor runs `round_robin_next`+`lru_put` (`:198-208`) while the sections actor runs `lru_get`+relink (`:251-258`) → list/hashmap corruption, double `section_create`, concurrent `section_dispatch` on one section → on-disk corruption.
- **Fix:** one owner — make CACHE_PUT also `actor_send` to the sections actor, or delete the sections actor and always dispatch synchronously from block_cache.

### F3 — CRITICAL 🟡 — `network_shutdown_connections` (main thread) races the network actor on `conn_mgr` → UAF
Site A: `network.c:221-243` (called from `offs_node_stop` phase 5, `node.c:144`; workers join only in phase 7, `node.c:158`) iterates `conn_mgr.peers[i]`, nulls `peer->quic_connection/quic_stream` (`:233-234`), calls `ConnectionShutdown`. Site B: that shutdown → `SHUTDOWN_COMPLETE` → `actor_send(NETWORK_QUIC_DISCONNECTED)` (`quic_listener.c:390-408`) → network actor runs `connection_manager_remove` (`network.c:3421`) → frees the `peer_connection_t` + `memmove` + `peer_count--` (`connection_manager.c:123-135`). Main thread reads `peers[i]` while a worker frees that peer and compacts the array → UAF + skipped/duplicated elements. `connection_manager` has no internal locks (verified).
- **Why not a race (rejected):** phase 4 `wait_for_idle` drains actors first, but the racing messages are generated by phase 5 itself, after that drain, while workers are still live.
- **Fix:** route shutdown through the network actor (send NETWORK_SHUTDOWN and wait), or join workers before phase 5.

### F4 — HIGH 🟡 — `actor_send` vs `actor_destroy` TOCTOU → enqueue into a destroyed mailbox
Check `actor.c:89` (`if DESTROY return`) and act `:96-98` (malloc + `message_queue_push`) are not atomic; destroyer sets DESTROY (`:58`) then `message_queue_destroy` frees the sentinel and NULLs head (`message_queue.c:105-110`). A sender past the check then `push`es: `atomic_exchange(&head)`→NULL → store through NULL/freed sentinel. Concrete non-worker sender: `_timer_completion_callback` on the pd-loop thread (`timer_actor.c:109-124`) racing `timer_actor_destroy` (`:311`, tracked timers destroyed only afterward at `:324`). Every MsQuic-callback `actor_send` during teardown has this shape.
- **Fix:** teardown-aware mailbox (CAS in a poisoned sentinel; push detects and frees the message), or enforce quiescence (destroy timers + join the loop thread before `actor_destroy`).

### F5 — HIGH 🟡 — Worker re-queues a destroyed actor after clearing RUNNING → dangling pointer in the deque
`scheduler.c:140-144`: `actor_run` returns `has_more` → `fetch_and(~RUNNING)` (`:141`) → `deque_push(actor)` (`:144`). Destroyer (`actor.c:81-83`) spins only until RUNNING clears, then frees the enclosing struct (`network.c:301+307`, `wt_transport.c:335-370`, `update_actor.c:205`). Between `:141` and `:144` the worker pushes a freeable pointer; the next pop reads `actor->flags` (`scheduler.c:119`) on freed memory and may dispatch through a freed function pointer.
- **Fix:** destroyer must wait until the actor is out of all scheduler queues (a SCHEDULED/queued flag handshake), or defer the free via `scheduler_pool_defer_cleanup`.

### F6 — HIGH 🟡 — Relay server teardown mutates the client table with no lock while MsQuic callbacks are live
`relay_server_destroy` (`relay_server.c:549-556`) iterates `server->clients`, `StreamClose`s and `_relay_remove_client` (frees `client->framer`, `:86-97`) **without `clients_lock`**. `relay_server_stop` (`:703-711`) never shuts down/awaits existing client connections, so `_relay_stream_callback` RECEIVE (`:308-321`, reads `client->framer`) and `_relay_connection_callback` SHUTDOWN_COMPLETE (`:441-459`) still fire on MsQuic threads. Destroy frees `client->framer` (`:91`) while a callback runs `stream_framer_feed` (`:312`) → UAF; or double `StreamClose` (`:552` vs `:448`).
- **Fix:** shut down all client connections and await SHUTDOWN_COMPLETE under `clients_lock` before freeing.

### F7 — HIGH 🟡 — `wt_transport_destroy` frees connection objects before stopping the listener/closing QUIC (sibling of the known quic_listener UAF)
`wt_transport_stop` (`wt_transport.c:651-655`) only stops the pd loop; `wt_transport_destroy` frees every `wt_connection_t` (`:335-370`, no `conn_lock`) and only then stops the listener (`:373-376`); client connections are never shut down before `RegistrationClose` (`:380-382`). MsQuic callbacks for still-open connections deref freed structs → UAF; `wt_connection_destroy`'s `vec_remove` under `conn_lock` (`wt_connection.c:753-755`) races the unlocked iteration. **The tcp/ws/unix transports share this teardown shape (identical `destroy_lock` pattern by grep) but were not individually traced.**
- **Fix:** ListenerStop → ConnectionShutdown each → await SHUTDOWN_COMPLETEs → then free.

### F8 — MEDIUM 🟡 — Timer completions carry a raw `actor_t* target` that can dangle
`timer_actor.c:114-124` enqueues a completion holding a raw `target`; `:232-245` later `actor_send`s it. `timer_actor_cancel` can't purge completions already in the mailbox; if the target is freed first, `actor_send` reads freed `flags`. Latent today (in-tree targets are long-lived and torn down safely), but nothing in the type system prevents a short-lived target. **Fix:** refcount the target or route completions by ID through a registry.

### F9 — MEDIUM 🟡 [SUSPECTED] — `offs_node_stop` phase 6 reads rings/hebbian while workers can still mutate them
`node.c:148-155` → `authority_save_peers` reads `network->rings`/`hebbian` (`authority.c:188+`) while workers (joined only at phase 7) can still run a late RELAY_RECEIVED dispatch that mutates rings (`network.c:3614-3620`). Plain lock-free structures; narrow trigger. **Fix:** move `authority_save_peers` after `scheduler_pool_stop`.

### F10 — MEDIUM 🟡 [self-documented] — Backpressure mute-vs-destroy can leave a sender muted forever
`actor_send`'s DESTROY re-check (`actor.c:116`) + CAS append (`:119-121`) vs `actor_destroy`'s single `backpressure_release` (`:60-62`): a node appended after the release is never drained → sender keeps `ACTOR_FLAG_MUTED`, is endlessly re-queued but never run (`scheduler.c:125-129`) → livelock + busy re-queue. The comment at `actor.c:113-115` acknowledges the window. **Fix:** per-actor lock or a second release sweep after detach.

### F11 — LOW 🟡 [SUSPECTED] — `msquic_singleton` lazy lock-init race
`_ensure_msquic_lock_initialized` (`msquic_singleton.c:18-22`) is check-then-act on `g_msquic_lock`; two first-callers can create different mutexes and both enter the critical section → double `MsQuicOpen2`/refcount corruption. First open is single-threaded in practice. **Fix:** `_Atomic` pointer + CAS (as `pool.c:42-49` does), or static init.

## Verified clean (with the check that cleared each)
- **Lock-order ABBA:** documented order names locks that don't exist; BlockCache has zero mutexes. Real lock graph (`registry_lock→inject.lock`, `inject.lock→idle_lock`, all others leaf-like) has no reverse edges → no ABBA. Caution: `clients_lock` is held across `StreamSend` (`relay_server.c:208-247`) — safe only because MsQuic calls don't block on app callbacks.
- **Chase-Lev deque:** matches canonical Le et al. ordering (release fence before `bottom` publish `deque.c:59-60`; seq_cst fence + top re-read in pop `:72-73`; seq_cst CAS `:79/:101`); grown arrays kept alive (`:26-29`) so thieves can't UAF; monotonic indices prevent `top` ABA.
- **Condvar predicate loops:** `wait_for_idle` re-checks under `idle_lock` in a loop incl. after the unlocked reinject scan (`scheduler.c:370-413`); worker inject wait re-checks under lock (`:159-163`). No naked waits.
- **Deferred-deref drain:** the "no actors running" comment (`scheduler.c:416`) is overstated but the mechanism is refcount-guarded (`defer_cleanup` +1 at `:279`; double-drain serialized by `deref_lock` swap `:292-295`). Residual hazard routes through F1, not the drain.
- **POSIX recursive re-lock:** no same-thread re-acquisition of any platform mutex found.

## Dead code / side notes (not concurrency bugs)
- `message_queue_push_single` (`message_queue.c:46-58`, non-atomic, single-producer): **zero callers** — remove before someone uses it cross-thread.
- Peer-actor dispatch is currently dead (all `PEER_*`/`CONN_STATE_*` messages go to the network actor); if anyone starts sending to `peer->actor`, peer fields become a two-actor data race.
- Non-concurrency: `pool_free`'s flush (`pool.c:154-158`) clobbers `list_head->next` in `_pool_push_global` (`:64`), orphaning the rest of the local chain → leaks all but the first item on every flush.

## Coverage
**Traced fully:** refcounter, actor/message_queue/pool, scheduler, deque, timer_actor, relay_server, connection_manager, conn_state, node, msquic_singleton, block; load-bearing regions of block_cache/sections/streams/block_recipe/wt_transport/peer_connection/quic_listener; network.c create/shutdown/destroy + dispatch (~600 of 4004 lines). **Skimmed:** authority, respiration_actor, ring_set/hebbian/eabf/wanted_list internals, wt_connection (destroy/lock sites). **Not reached:** wire.c, relay_client, nat_detect (beyond the known bug), tcp/ws/unix/http connection internals (share F7's teardown shape, unverified), OFFStreams beyond block_recipe/ofd spot checks, ClientLibs, index.c internals.
