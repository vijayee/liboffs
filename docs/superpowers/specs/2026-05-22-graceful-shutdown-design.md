# Graceful Shutdown — Design

Date: 2026-05-22

## Motivation

`offs_node_stop()` currently sets `running=0` and tears down components immediately:
no connection draining, no peer notification, no in-flight operation completion.
This loses data for in-progress PUT operations and causes peers to waste resources
on timeout-based dead-node detection.

## Scope

Full graceful shutdown with configurable deadline. When `offs_node_stop()` is called:

1. Stop accepting new work (HTTP connections, P2P connections, API requests)
2. Notify peers of departure (QUIC CONNECTION_CLOSE frame, non-silent)
3. Drain in-flight HTTP requests (wait for completion up to deadline)
4. Drain actor work queue (wait for idle up to deadline)
5. Close P2P connections
6. Flush section bitmaps and persist peer list
7. Stop scheduler and destroy components

Each phase checks the deadline. If expired, skip remaining drain phases and
proceed to force-stop.

## Configuration

Add to `config_t` in `src/Util/config.h`:

```c
uint32_t shutdown_timeout_ms;  // default 30000, 0 = no timeout (block forever)
```

Set default in `config_defaults()` in `src/Util/config.c`.

## Phases

### Phase 1: Stop Accepting

Set `node->draining = 1`. This flag gates three entry points:

- **HTTP server:** accept loop checks draining flag, stops `accept()` and closes
  the listen socket immediately. New connections hitting the listen socket get
  `ECONNREFUSED` from the kernel backlog.
- **P2P listener:** `quic_listener` accept callback checks draining flag, rejects
  new connections.
- **Client API:** `offs_client_put/get/delete` entry points check draining flag,
  return `OFFS_ERROR_DRAINING`.

### Phase 2: Notify Peers

Close each active QUIC connection with `QUIC_CONNECTION_SHUTDOWN_FLAG_NONE` (not
SILENT). MsQuic sends a `CONNECTION_CLOSE` frame to the peer, which signals
intentional departure. Peers detect this faster than timeout-based dead-node
detection. No new wire protocol message needed.

### Phase 3: Drain HTTP

`http_server_drain()` waits for in-flight HTTP request count to reach zero.
Track active requests with an atomic counter incremented on request start and
decremented on completion (connection close). Poll with 50ms sleep until zero
or deadline exceeded.

### Phase 4: Drain Actors

Call existing `scheduler_pool_wait_for_idle()` — it already blocks until all
worker queues are drained. Wrap with deadline polling: check deadline every
100ms. If deadline exceeded before idle, proceed to force stop.

Call `scheduler_pool_drain_pending_derefs()` to run deferred destructors.

### Phase 5: Close P2P

Close all remaining QUIC connections via connection manager. Stop relay client.
No draining here — Phase 4 completed actor work, so no new messages will be
generated.

### Phase 6: Flush & Persist

- Flush section bitmaps to disk (existing `section_flush()` or equivalent)
- Save peer list (existing `save_peers_to_file()`)

### Phase 7: Stop Scheduler

Call `scheduler_pool_stop()` to join all worker threads, then proceed with
the existing destroy sequence in `offs_node_destroy()`.

## Deadline Handling

```c
static uint64_t deadline_ms(node_t *node) {
    if (node->config.shutdown_timeout_ms == 0) return UINT64_MAX;
    return platform_time_now_ms() + node->config.shutdown_timeout_ms;
}

static bool deadline_exceeded(uint64_t deadline) {
    return platform_time_now_ms() > deadline;
}
```

When `shutdown_timeout_ms == 0`, all drain phases block indefinitely
(useful for tests and embedded use). When set, each phase polls the deadline
and aborts if exceeded.

After deadline exceeded in any phase, skip directly to Phase 7 (force stop).

## Files

| File | Change |
|------|--------|
| `src/Util/config.h` | Add `shutdown_timeout_ms` field |
| `src/Util/config.c` | Set default 30000 |
| `src/Node/node.c` | Rewrite `offs_node_stop()` as phased drain, add `draining` flag |
| `src/Node/node.h` | Add `OFFS_ERROR_DRAINING` error code |
| `src/ClientAPI/HTTP/http_server.h` | Add `http_server_drain()` declaration |
| `src/ClientAPI/HTTP/http_server.c` | Add draining flag, drain function, active request counter |

## Testing

- Unit: deadline calculation, deadline exceeded check
- Unit: draining flag rejects PUT/GET with OFFS_ERROR_DRAINING
- Unit: http_server_drain waits for in-flight requests
- Integration: full shutdown sequence with in-flight PUT completes without data loss
- Integration: deadline exceeded skips drain phases
- Integration: zero timeout blocks until idle (backward compatible)
- Pre-existing: all existing tests continue to pass
