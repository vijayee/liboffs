# Ephemeral Blocks & Pin Counts — Design

Date: 2026-09-17
Status: Approved

## Purpose

Add two per-block metadata concepts to the liboffs block cache:

- **Ephemeral blocks** — blocks whose residency in the block cache may be temporary. Stored by clients (primarily libons via the C client) while they verify whether a representation is unique. Ephemeral blocks are excluded by default from any recipe that would make them part of a new representation. Ephemerality is a **claim count** (`ephemeral_count`), not a boolean: it tracks how many ephemeral representations currently reference the block, so a block shared by several ephemeral representations is only removed when the last claim is released.
- **Pin counts** — a per-block reference count. Permanent blocks with `pin_count > 0` are immune from automatic deletion (respiration) and protected from explicit deletion by default. Pins never affect ephemeral blocks' deletion.

The libons flow this serves: put a representation as ephemeral → verify uniqueness → either commit (mark all blocks permanent) or abort (delete all ephemeral blocks in the representation).

## Decisions (validated with user)

| Question | Decision |
|---|---|
| Persistence | `ephemeral_count` and `pin_count` persist in `index_entry_t` (WAL + snapshot) |
| Restart behavior | Ephemeral blocks stay ephemeral across restart until explicitly committed or deleted |
| Network visibility | Ephemeral blocks are local-only: never announced, never served to peers. On unmark (commit) they are announced to the network the same way a put operation would |
| Block-level API | The block cache API has the ability to manipulate a block's pin and ephemeral status directly |
| Recipe exclusion | Persistent elastic bloom filter registry of ephemeral ori/urls. Recycler errors + warns on a listed source when the new representation is non-ephemeral. An ignore flag is available; with it the caller **must explicitly choose** `commit` (mark source ephemerals permanent) or `propagate` (mark the new representation's blocks ephemeral). No default mode. When the new put is itself ephemeral, an ephemeral source is accepted without error — the outcome is inherently propagate-like, with claims acquired for referential integrity |
| Referential integrity | Ephemerality is a count (`ephemeral_count`, `uint16_t`): each ephemeral representation referencing a block holds one claim. `delete-ephemeral` releases the representation's claims and physically deletes only blocks whose count reaches 0, so recycling an ephemeral source with `propagate` can never be stranded by a later delete of the source |
| Overflow | `ephemeral_count` acquires reject with an error at `UINT16_MAX` (no silent saturation); `pin_count` saturates at `UINT32_MAX` with a warning |
| Pinned deletion | Pins protect **permanent** blocks only: explicit delete of a pinned permanent block warns and is rejected by default; a force/ignore flag bypasses (pin reset, block deleted). Ephemeral blocks are deleted at count 0 regardless of pin status |
| Transitive commit | mark-permanent commits shared recycled blocks too, without warning — B can commit A's blocks if it recycled them; all claims release at once so nothing is stranded |
| Failed ephemeral puts | Cleanup RELEASEs the put's created blocks (not recipe-received ones) and any claims it acquired on recycled blocks; crash-orphans are found via a list-ephemerals API and cleaned with the block-level ops |
| Ephemeral ceiling | None — no automatic bound on ephemeral bytes; operators maintain discipline via the list-ephemerals API plus block-level status/delete ops |
| Unpin | Yes — an unpin API decrements pin counts, clamped at zero |
| Pin on get/load | A `pin` argument on get/load pins all blocks of the representation after the read completes; pins persist until unpin |
| LRU interaction | Pinned entries do **not** affect the in-memory LRU cache; LRU and capacity behavior are identical regardless of pinning or ephemerality |
| Bloom filter concurrency | The elastic bloom filter is owned by a dedicated actor that manages concurrency and usage across APIs. Only recyclers check it; every path that unmarks an ephemeral ori must maintain (remove from) it. Deletion is native to the elastic bloom filter — no rebuild is needed |

## 1. Data model & persistence

### `index_entry_t`

```c
typedef struct {
  refcounter_t refcounter;
  fibonacci_hit_counter_t counter;
  buffer_t* hash;
  size_t section_id;
  size_t section_index;
  uint64_t ejection_date;
  uint16_t ephemeral_count; /* 0 = permanent; >0 = ephemeral, one claim per referencing ephemeral representation */
  uint32_t pin_count;       /* >0 = permanent blocks resist deletion; never affects ephemeral blocks */
} index_entry_t;
```

`ephemeral_count` is a claim count, not a boolean: an ephemeral block put by representation A has count 1 (A's claim); when a `propagate` recycle makes another ephemeral representation B reference it, the count rises to 2. The count only falls via release (delete-ephemeral) or clear (commit). This is what prevents referential integrity breaks — a block is physically deleted only when its last ephemeral claim is gone.

Width note: `uint16_t` (65,535 claims) fits in the struct's existing alignment padding after `ejection_date`, and CBOR/WAL encode the logical value compactly, so the wider type costs no extra RAM or disk for typical small counts.

### CBOR snapshot

`index_entry_to_cbor` grows from a fixed 5-element array to 7:
`[counter, hash, section_index, section_id, ejection_date, ephemeral_count, pin_count]`.

Decode (`cbor_to_index_entry`) accepts arrays with `>= 5` elements; missing trailing fields default to `ephemeral_count = 0`, `pin_count = 0`. Existing on-disk indexes load unchanged.

### WAL

One new record type `'m'` (metadata) carrying `{hash, ephemeral_count, pin_count}`. Every mutation path (acquire, release, clear, pin, unpin) writes the full metadata set, making replay idempotent and partial crashes harmless. Existing types `'a'/'i'/'e'/'r'` are untouched.

### In-memory access

`index_entry_t` is already the object referenced by both the LRU node and the snapshot tree — no join needed anywhere.

## 2. Ephemeral registry actor (elastic bloom filter)

New actor in `src/BlockCache/` (`ephemeral_registry.c` / `ephemeral_registry.h`) owning a persistent **elastic (scalable) bloom filter** keyed by **descriptor hash** — the canonical representation identity (URLs have multiple encodings; one URL maps to one descriptor hash).

**Why an actor:** the registry is touched from many paths (ephemeral puts insert; mark-permanent and delete-ephemeral remove; recyclers check). The actor mailbox gives single-threaded mutation, no locks, consistent with the block cache/index actor pattern.

**Messages:**
- `EPHEMERAL_REGISTRY_ADD` — insert a descriptor hash (sent by the ephemeral put path when the descriptor block is stored, and by the recycler's self-healing discovery, see below)
- `EPHEMERAL_REGISTRY_CHECK` (request/result) — used by the recycler recipe
- `EPHEMERAL_REGISTRY_REMOVE` — delete a descriptor hash from the filter. **Every path that unmarks or deletes an ephemeral ori must send this** — that is how the registry is maintained

**Deletion:** an elastic bloom filter supports deletion as part of its design, so no rebuild is ever needed — `EPHEMERAL_REGISTRY_REMOVE` deletes the key directly. (The insert set includes each ephemeral representation's descriptor block hash, because the descriptor block created by an ephemeral put is itself one of the blocks the put creates and therefore carries an ephemeral claim.)

**The filter is advisory in both directions; the index is exact:**
- *False positives:* on a `CHECK` hit, the recycler walks the actual descriptor and consults index entry counts. Confirmed ephemeral → error (when the new representation is non-ephemeral, see recycle check). FP hit with a clean walk → proceed normally.
- *False negatives:* when the new representation is non-ephemeral, the recycler also checks each fetched recipe block's `ephemeral_count` at fetch time. Discovering a claimed block — even when the filter missed the source — errors exactly as a `CHECK` hit would (subject to the same override modes).
- *Self-healing:* upon discovering at fetch time that a recycled source's blocks are ephemeral, the recycler sends `EPHEMERAL_REGISTRY_ADD` for the source's descriptor hash, repairing the stale filter so future checks hit. (In `commit` mode the source's blocks are cleared to permanent, so no add is performed; in default and `propagate` modes, and in the ephemeral-put case, the source remains ephemeral and is added.)

**Persistence (backup copy, no WAL):** filter file alongside the index (`<location>/ephemeral_registry.bf`), flushed on mutation (debounced, following the index's debounce pattern). Each flush uses **two-file rotation, mirroring the index's `current_file`/`last_file` pattern**: write to a temp file, fsync, rotate the previous file to `ephemeral_registry.bf.last`, and move the new file into place — the previous flush is retained as a backup copy. On load: prefer the current file; if missing or corrupt, fall back to `ephemeral_registry.bf.last`; if both are unusable, reconstruct once from the index (entries with `ephemeral_count > 0`). A stale backup is always safe: extra keys only produce false positives, which the confirm-walk resolves, and genuine misses are caught by the fetch-time exact check and self-heal the filter.

## 3. Block cache semantics

### Block-level actor messages

Following the existing `CACHE_GET/PUT/REMOVE` pattern with sync/async reply semantics:

- `CACHE_EPHEMERAL` — one message with an op field:
  - `ACQUIRE` — `ephemeral_count += 1`; **rejects with an error at `UINT16_MAX`** (no silent saturation). Used by the ephemeral put path (new blocks start at 1) and by `propagate` recycles (referenced blocks +1).
  - `RELEASE` — `ephemeral_count -= 1`; when the count reaches 0 the block is **deleted** (regardless of pin status). Used by delete-ephemeral.
  - `CLEAR` — `ephemeral_count = 0` (commit: block becomes permanent; the new put is announced to the network, see below).
- `CACHE_PIN` — `pin_count += 1` (saturating at `UINT32_MAX` with a warning)
- `CACHE_UNPIN` — `pin_count -= 1`, clamped at 0
- `CACHE_EPHEMERAL_LIST` — enumerate every ephemeral block in the cache (hash, `ephemeral_count`, `pin_count`). The maintenance surface for operators: combined with the block-level ops above (`CACHE_EPHEMERAL` RELEASE/CLEAR, `CACHE_REMOVE`), it lets an operator find and clean up orphaned ephemeral data (e.g. blocks claimed by puts that failed before cleanup could run).

Each mutation writes an `'m'` WAL record and updates the referenced `index_entry_t` in place.

### LRU and capacity: unchanged

`block_lru_cache_put/get/delete` and the `CACHE_PUT_FULL` capacity rejection have **zero awareness** of `pin_count` or `ephemeral_count`. The LRU is a RAM cache of loaded blocks; evicting from it never deletes data, so pins have no business there.

### Deletion immunity

- **Respiration exhale** (`index_entries_by_ejection_date` victim selection): permanent entries with `pin_count > 0` are excluded from victim lists. Ephemeral entries (`ephemeral_count > 0`) are also excluded — exhale's "not found elsewhere" path re-stores blocks to the network, and ephemeral blocks are local-only and must never be pushed.
- **Explicit delete (`CACHE_REMOVE`)**:
  - A **permanent pinned** block (`ephemeral_count == 0`, `pin_count > 0`) returns a new `CACHE_REMOVE_PINNED` result code and a warning. A `force` flag on the remove payload bypasses: warning logged, pin count reset to 0, block deleted. This warn/ignore behavior is surfaced at every API level.
  - An **ephemeral claimed** block (`ephemeral_count > 0`) returns a new `CACHE_REMOVE_EPHEMERAL_CLAIMED` result code and a warning — deleting it outright could strand other ephemeral representations that reference it. The `force` flag bypasses with the same warning semantics; the proper non-forced paths are delete-ephemeral (RELEASE) or mark-permanent (CLEAR).
  - Pins never protect ephemeral blocks: an ephemeral block reaching count 0 via RELEASE is deleted regardless of `pin_count`.

### Network visibility while ephemeral

- `NETWORK_LOCAL_STORE_BLOCK` announcement (sent on `CACHE_PUT_NEW`) is suppressed for ephemeral puts.
- find-block responses skip blocks whose index entry has `ephemeral_count > 0`.
- **On commit** (`CACHE_EPHEMERAL` CLEAR), the block is announced to the network exactly as a put would, using the same store-block push path respiration uses for re-stores.

## 4. API surface & flows

### Put path (ephemeral)

The existing — currently unconsumed — `temporary` flag becomes the `ephemeral` argument end-to-end: HTTP header, CBOR wire (index 7), C client `offs_client_put` options.

When set:
- `writeable_off_stream._create_tuple` acquires ephemeral claims (**ACQUIRE**, count starts at 1) on **only the blocks the put creates** (new_blocks_recipe randoms, each tuple's off_block, and the descriptor block written by `writeable_descriptor`) at `block_cache_put` time, via a new field on `cache_put_payload_t`. Blocks fetched by recycler recipes are **never** claimed by this path (see `propagate` below for how recycled blocks acquire claims).
- On completion, the descriptor hash is inserted into the ephemeral registry (`EPHEMERAL_REGISTRY_ADD`).
- **Failure cleanup:** the put tracks the hashes of every block it created (the ephemeral-marked ones — not blocks received from its recipes). If the put fails mid-stream (error, disconnect, overflow), the cleanup path sends `CACHE_EPHEMERAL` RELEASE for each created block — their only claim is the failed put's, so they reach 0 and are deleted — and RELEASEs any claims it acquired on recycled source blocks. If cleanup itself cannot run (crash), orphaned blocks are findable via `CACHE_EPHEMERAL_LIST` and clearable with the block-level ops.

### Recycle check

`recycler_recipe` calls `EPHEMERAL_REGISTRY_CHECK` on each source ori's descriptor hash before using the source. The exclusion rule's purpose is to keep unverified data out of **permanent** representations, so enforcement depends on what the new put is:

**Non-ephemeral put:** every recipe block the recycler fetches is checked at fetch time for `ephemeral_count > 0` — the exact enforcement that makes a stale or lost filter unable to silently admit an ephemeral source. On a confirmed-ephemeral source (via `CHECK` + confirm-walk, or via fetch-time discovery, which also sends `EPHEMERAL_REGISTRY_ADD` to heal the filter):
- Default: hard error + warning to the caller (recycle source is ephemeral/unverified).
- With the recycle-ephemeral override, the caller must **explicitly choose one mode** (there is no default):
  - `commit` — CLEAR the source's ephemeral blocks (they become permanent and are announced to the network); the new put proceeds as non-ephemeral.
  - `propagate` — the new put's newly-created blocks acquire ephemeral claims (ACQUIRE, count 1), and **every ephemeral block referenced from the recycled source acquires an additional claim (ACQUIRE, +1)** so the source's later deletion cannot remove blocks still used by this representation. If a referenced block is at `UINT16_MAX` claims, the put fails with the overflow error.

**Ephemeral put:** an ephemeral source is **accepted without error and without an override** — the outcome is inherently propagate-like, and no permanent contamination can occur because the new representation is itself temporary. The mechanics are the same as `propagate`: every ephemeral block referenced from the source acquires an additional claim (ACQUIRE, +1) for referential integrity, and the source's descriptor hash is heal-added to the registry (it remains ephemeral). Overflow at `UINT16_MAX` claims fails the put as above.

### Representation-level APIs (by ori/URL)

Each resolves the URL → descriptor, walks the descriptor to enumerate its block hashes, then issues block-level messages:

- **mark-permanent** — `CACHE_EPHEMERAL` CLEAR on every ephemeral block in the representation's block set. This is **transitive and intentional**: a representation that recycled ephemeral blocks (via `propagate` or as an ephemeral put) commits those shared blocks too — B can commit A's blocks if it recycled them. All claims are released at once; other ephemeral representations referencing these blocks are unaffected — the blocks are now permanent and announced, so they can never be stranded. Announce each newly-committed block to the network; send `EPHEMERAL_REGISTRY_REMOVE` for the representation's descriptor hash.
- **delete-ephemeral** — `CACHE_EPHEMERAL` RELEASE on every ephemeral block in the representation: each count is decremented and the block is physically deleted **only when its count reaches 0** (blocks still claimed by other ephemeral representations survive, regardless of pin status); sends `EPHEMERAL_REGISTRY_REMOVE` for the representation's descriptor hash.
- **pin-all** — `CACHE_PIN` on every block in the representation (+1 each).
- **unpin-all** — `CACHE_UNPIN` on every block in the representation (clamped at 0).

Exposure: HTTP routes in `off_routes.c` alongside the existing OFF patterns; `client_api_wire.c` ops (WS/TCP/Unix/WT transports inherit); C client functions in `offs_client.h` (libons' interface); JS client. The **list-ephemerals** maintenance API is exposed the same way, so operators (and libons) can enumerate ephemeral blocks and change or delete their status.

### Get/load + pin

A `pin` argument on get/load walks the representation after the read completes and issues pin-all. Pins persist until unpin is called.

### Configuration

New fields in `config.h` (documented in `docs/CONFIG_FIELDS.md`):
- `ephemeral_registry_capacity` — initial elastic filter capacity
- `ephemeral_registry_error_ratio` — target false-positive ratio
- `ephemeral_registry_growth` — filter growth factor

`recycle_ephemeral_mode` is **not** a config field — it is chosen explicitly per call.

## 5. Error handling summary

| Situation | Behavior |
|---|---|
| Recycle an ephemeral source (default) | Hard error + warning |
| Recycle an ephemeral source with override `commit` | Source ephemeral blocks become permanent + announced; new put normal |
| Recycle an ephemeral source with override `propagate` | New put's new blocks acquire claims; referenced source blocks +1 claim |
| `ACQUIRE` at `UINT16_MAX` claims | Error — the acquiring put fails; no silent saturation |
| Delete a pinned **permanent** block (default) | `CACHE_REMOVE_PINNED` result + warning; block kept |
| Delete a pinned **permanent** block with force | Warning logged; pin reset; block deleted |
| Delete a **claimed ephemeral** block via `CACHE_REMOVE` (default) | `CACHE_REMOVE_EPHEMERAL_CLAIMED` result + warning; block kept (release/clear are the proper paths) |
| Delete a claimed ephemeral block with force | Warning logged; block deleted regardless of remaining claims |
| `RELEASE` bringing an ephemeral block to 0 | Block deleted regardless of pin status |
| Ephemeral put fails mid-stream | Cleanup RELEASEs the put's created blocks (deleted at 0) and its acquired claims; crash-orphans findable via list-ephemerals |
| Bloom filter file missing/corrupt | Load falls back to `ephemeral_registry.bf.last`; if also unusable, one-time reconstruction from index; log notice |
| Recycler fetches a claimed block despite a filter miss | If the new put is non-ephemeral: fetch-time check errors exactly as a `CHECK` hit; source descriptor hash self-heals into the filter via `ADD`. If the new put is ephemeral: no error — treated as propagate (claims acquired, source heal-added) |

## 6. Testing

- Index: CBOR round-trip with old 5-element arrays (defaults applied) and new 7-element arrays; WAL `'m'` record replay incl. partial-write recovery; ephemeral count increment/decrement/clear; acquire-overflow rejection; pin saturation and clamp.
- Registry actor: add/check/remove lifecycle (native elastic deletion, no rebuild); false-positive resolution via descriptor walk and false-negative enforcement via fetch-time check (non-ephemeral puts); self-healing `ADD` on fetch-time discovery; persistence reload incl. `.last` backup fallback and rotation on flush; one-time reconstruction from index when both files are unusable.
- Block cache: `CACHE_EPHEMERAL` ACQUIRE/RELEASE/CLEAR; `CACHE_EPHEMERAL_LIST` enumeration; `CACHE_REMOVE_PINNED`, `CACHE_REMOVE_EPHEMERAL_CLAIMED` and their force paths; respiration victim selection excluding pinned permanent and ephemeral entries.
- **Referential integrity (integration):** ephemeral A → recycle into ephemeral B (no override needed — accepted as propagate) → `delete-ephemeral(A)` leaves B's referenced blocks alive and B readable; `delete-ephemeral(B)` then removes them; ephemeral A → recycle into B → commit A leaves referenced blocks permanent, `delete-ephemeral(B)` removes only B's own blocks; ephemeral put recycling an ephemeral source does not error even when the registry misses the source.
- **Transitive commit:** ephemeral B (having recycled ephemeral A) → mark-permanent(B) commits A's shared blocks too; A's later `delete-ephemeral` touches only its descriptor/own blocks; B remains fully readable and permanent.
- **Failed puts:** ephemeral put failing mid-stream (error, disconnect, overflow) cleans up its created blocks and acquired claims; nothing orphaned in the normal case; simulated crash leaves orphans recoverable via list-ephemerals + block-level ops.
- Flows (integration): ephemeral put → mark-permanent (blocks announced); ephemeral put → delete-ephemeral; non-ephemeral recycle error and both override modes; get/load with pin; LRU/capacity behavior identical with and without pins.
- All suites valgrind-clean per project convention.