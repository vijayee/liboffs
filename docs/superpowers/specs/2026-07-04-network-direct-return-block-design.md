# Network Direct-Return Block — Design

## Problem

When a GET request needs a block that isn't in the local cache, the
`readable_off_stream` asks the network to find it. The network finds the
block on a peer, receives the block data, and then:

1. Calls `block_cache_put(block_cache, block, fib, NULL)` — with
   `reply_to = NULL` (`network.c:1734`).
2. Destroys the block (`network.c:1735`).
3. Sends `NETWORK_FIND_BLOCK_RESULT` with `found = 1` and the block's
   hash — but **not the block data** (`message.h:234-238`).
4. The stream re-issues `block_cache_get` to fetch the block it was just
   told was "found" (`readable_off_stream.c:271`).

If the cache is full, step 1 silently fails: `block_cache_put` hits the
`CACHE_PUT_FULL` path internally but, since `reply_to` is NULL, the
result is swallowed. The block is not stored. The stream re-fetches,
misses again, asks the network again — a stall/loop. The client sees
`GET_RESPONSE_START` followed by a hang or silent failure, with no
indication that cache space was the cause.

The same pattern exists in `readable_descriptor.c:274` (descriptor
block fetch) and `block_recipe.c:392` (recipe block fetch).

## Root cause

The `NETWORK_FIND_BLOCK_RESULT` payload carries only the hash and a
`found` flag. The comment in `message.h:237` says it outright:
`"1 = found (block now in cache)"`. The design assumes the block is in
the cache after `found = 1`, forcing the stream to re-fetch. But the
network already has the block data in hand at `network.c:1721-1731` —
it built the `block_t`, called `block_cache_put`, then destroyed the
block without passing it along.

## Goal

Pass the retrieved block data directly to the requesting stream in the
`NETWORK_FIND_BLOCK_RESULT` message, so the stream can use it without
re-fetching from the cache. The cache store becomes best-effort (for
future GETs), not a gate for the current GET.

## Non-goals

- **No GET pre-flight.** This design does not add a block-availability
  check before `GET_RESPONSE_START`. It fixes the "found but couldn't
  store" case; the genuinely-not-found case is separate.
- **No change to the local-cache-hit paths.** When the network finds the
  block in the local cache (lines 2675, 2730), the block is not in hand
  and the re-fetch works fine. Direct-return is only for the remote-receipt
  paths where the block data is already in memory.
- **No change to the PUT path.** The writable streams already handle
  `CACHE_PUT_ERROR`/`CACHE_PUT_FULL` (fixed in the prior PUT cache space
  errors work). This design only touches the GET/retrieval path.
- **No change to the network-announce-on-NEW path.** The
  `CACHE_PUT_NEW` → peer announce flow is handled by the writable
  streams during PUT, not by the new network-actor `CACHE_PUT_RESULT`
  handler.

## Architecture

Four coordinated changes in liboffs:

### 1. Payload struct (`src/Actor/message.h`)

Extend `network_find_block_result_payload_t` with a nullable `block`
field:

```c
typedef struct {
  buffer_t* hash;       /* same hash from the request */
  int       found;      /* 1 = found, 0 = not found */
  block_t*  block;      /* the retrieved block, when found via remote path.
                         * NULL when found=0, or when found via local-cache-hit
                         * (the consumer re-fetches from cache in that case). */
} network_find_block_result_payload_t;
```

The `found` flag stays the source of truth for "did the network find
it?" — `block` is the optional fast-path data.

### 2. Destroy function (`src/Network/network.c:88-95`)

`network_find_block_result_destroy` destroys the `block` field if
present. Callers that attach the block `refcounter_reference` it before
attaching; the destroy function calls `block_destroy` (which derefs).

### 3. Network actor — 3 remote-receipt paths (`src/Network/network.c`)

The 3 paths that receive block data from a peer and have local
requesters waiting (lines 1755, 1958, 2550):

1. Build the `block_t` from the peer response data (unchanged — they
   already do this via `block_create_existing_data_hash_by_type`).
2. Call `block_cache_put(bc, block, fib, &network->actor)` — `reply_to`
   now set to the network actor. Best-effort: the store may succeed
   (cache populated for future GETs) or fail (cache full — harmless to
   the current GET).
3. Attach `result->block = REFERENCE(block, block_t)` to the result
   payload.
4. Send `NETWORK_FIND_BLOCK_RESULT` with `found = 1`, `hash`, `block`.
5. `DESTROY(block, block)` — release the network's local reference; the
   payload's reference keeps the block alive until the consumer
   processes it.

The 2 local-cache-hit paths (lines 2675, 2730) are unchanged — they send
`found = 1` with `block = NULL`, and consumers fall back to
`block_cache_get` re-fetch.

### 4. Network actor — new `CACHE_PUT_RESULT` handler

The network actor's dispatch function gains a `CACHE_PUT_RESULT` case.
Since the `block_cache_put` is purely best-effort caching for future
GETs (not a gate for the current GET), the handler is minimal:

```c
case CACHE_PUT_RESULT: {
  cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
  if (result->result == CACHE_PUT_ERROR || result->result == CACHE_PUT_FULL) {
    /* Best-effort cache store failed. The block was already delivered
     * directly to the requesting stream via NETWORK_FIND_BLOCK_RESULT,
     * so this failure does not affect the current GET. Log and continue. */
  }
  /* CACHE_PUT_NEW / CACHE_PUT_EXISTS: no action needed — the block is
   * in the cache for future GETs. Network announce for CACHE_PUT_NEW
   * is handled by the writable_off_stream path during PUT, not here. */
  break;
}
```

### 5. Stream consumers — 3 handlers

`readable_off_stream.c:264`, `readable_descriptor.c:274`,
`block_recipe.c:392` — the `NETWORK_FIND_BLOCK_RESULT` handler in each:

- When `found = 1` and `block != NULL`: use the block directly (XOR
  accumulate / feed to descriptor / feed to recipe) — the same logic
  each uses for `CACHE_GET_RESULT` success.
- When `found = 1` and `block == NULL` (local paths): fall back to the
  existing `block_cache_get` re-fetch.
- When `found = 0`: deactivate as before.

The `block != NULL` check is the sole discriminator between remote
direct-return and local re-fetch. No consumer needs to know which
network path produced the result.

## Data flow (remote-receipt path, after the change)

```
stream: block_cache_get(hash) → cache miss
stream: network_local_find_block(hash) → network
network: peer responds with block_data
network: build block_t from block_data
network: block_cache_put(bc, block, fib, &network->actor)  [best-effort]
network: NETWORK_FIND_BLOCK_RESULT{found=1, hash, block=REFERENCE(block)} → stream
network: DESTROY(block)  [releases local ref; payload ref keeps block alive]
stream: receive result, block != NULL
stream: XOR-accumulate result->block->data
stream: blocks_received++; if complete → _finish_decode_and_render

(later, asynchronously)
network: CACHE_PUT_RESULT{CACHE_PUT_FULL} → log, continue  [cache full, harmless]
```

Compare to the current flow where the stream re-issues `block_cache_get`
after `found=1`, which fails when the cache couldn't store the block.

## Error handling

- **`block_cache_put` fails (CACHE_PUT_FULL / CACHE_PUT_ERROR):** the
  network actor's `CACHE_PUT_RESULT` handler logs and continues. The
  current GET is unaffected — the block was delivered directly.
- **Network can't find the block (`found = 0`):** unchanged — the
  stream deactivates with `"Block not found on network"` (or the
  descriptor/recipe equivalent). This is the genuinely-not-found case,
  not addressed by this design.
- **`block` is NULL when `found = 1` (local paths):** the consumer
  re-fetches via `block_cache_get`. If the block was evicted between
  the network's cache check and the re-fetch, the `CACHE_GET_RESULT`
  handler will see `block == NULL` and go to the network again — the
  existing retry behavior, unchanged.

## Testing

### Unit tests

- `network_find_block_result_destroy` — destroys the payload with a
  `block` set (verify no leak, no double-free). Test with `block=NULL`
  and `block != NULL`.

### Integration tests

- **Remote direct-return:** mock a peer response with block data,
  verify the stream's `NETWORK_FIND_BLOCK_RESULT` handler receives
  `block != NULL` and XOR-accumulates it without calling
  `block_cache_get`.
- **Local re-fetch fallback:** mock a local-cache-hit result
  (`found=1`, `block=NULL`), verify the stream re-issues
  `block_cache_get`.
- **Cache-full no longer stalls GET:** configure
  `max_capacity_bytes` small, GET a block that requires network
  fetch. The best-effort `block_cache_put` fails (CACHE_PUT_FULL), but
  the GET succeeds because the block was delivered directly.
- **No regression:** existing GET round-trip tests pass.

### Valgrind

- Run valgrind on the new/modified tests to verify no leaks (the
  `block` reference in the payload is freed by the destroy function;
  the network's local `DESTROY(block)` releases its reference).

## Out of scope

- No GET pre-flight (block-availability check before
  `GET_RESPONSE_START`).
- No change to the local-cache-hit paths (2675, 2730) — they keep the
  re-fetch.
- No change to the PUT path (writable streams already handle
  cache-full).
- No change to the `CACHE_PUT_NEW` network-announce flow (handled by
  writable streams during PUT).
- No change to the `found = 0` path (genuinely-not-found is separate).