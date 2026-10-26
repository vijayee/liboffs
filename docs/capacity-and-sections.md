# Capacity Model and the Section Layer

How OFFS capacity actually works — the units, the growth rules, and the
failure modes this module has already had. Read this before touching
`sections.c`, `block_cache.c` capacity logic, or anything that sizes a put.

## The three unit systems (the standing hazard)

The BlockCache stack uses three different unit systems for "size". They
have been confused at least once with production impact (the 2026-10-25
big-put stall: a 4.6GB capacity cap demanded ~59 million sections because
the ceiling math divided bytes by a tuple count).

| Field | Unit | Example |
|---|---|---|
| `block_size_e` (mega/standard/mini/nano) | **BYTES per block** (enum values are literally 1000000/128000/64000/136) | `(size_t)block_cache->type` used as byte size is correct |
| `sections_t.size` | **TUPLES (block slots) per section** | default 78 → a section holds 78 × 128,000 = **9.98MB**, not 78 bytes |
| `max_capacity_bytes` / `sections_t.max_bytes` | **BYTES** of total cache storage | the `max-capacity-bytes` config field |

Rule of thumb: to convert anything in the sections layer, go through
`_sections_bytes_for_type(sections->type)` and `sections->size` (tuples).
Never divide a byte count by `sections->size` alone.

## The OFF expansion factor

An OFF representation stores **`tuple_size`× the file's bytes** in the
cache: every `block_size` data block rides `tuple_size − 1` random blocks
that are also stored, plus descriptor blocks. At the defaults
(standard = 128KB, tuple_size = 3) a **909MB file needs ~2.73GB of cache**
(~7,105 tuples → 21,315 block stores).

`writeable_off_stream_estimate_required_bytes(stream_length, tuple_size,
descriptor_pad)` computes this — PUT_START's pre-flight runs
`block_cache_can_fit` against it and rejects with
"cache full: configure larger max_capacity_bytes" when the cap is short.
Size your cap for the expansion, not the file size.

## How the cap reaches the disk shelves

- `block_cache_set_max_capacity` (and `block_cache_create`) call
  `sections_set_max_bytes` → the section layer's growth ceiling.
- The ceiling = ceil(max_bytes / section byte capacity) + 1 section of
  headroom; `max_bytes == 0` = uncapped → legacy behavior (robin tops up
  to `max_tuple_size` only and never grows past history).
- **Growth is lazy, one net section per full-section event**: when a write
  rolls onto a full section, `sections_full` removes it, refills the
  robin to `max_tuple_size` (replacing the dead one with a fresh file),
  then opens ONE more toward the target. The write retry's budget scales
  with the live robin (see below), so a big put opens its ~90 sections
  across the put as they fill — no hundreds-of-files storms.
- Shrinking the cap does NOT remove sections; extra sections just wait
  for GC/shed pressure.

## The write retry budget

`sections_write`'s attempt budget is refreshed **every iteration**:
`robin size + full rotations so far + max_tuple_size`. A grown robin whose
older entries come from past failed puts contains many FULL sections; each
full attempt removes that section permanently, so the harmful pool shrinks
monotonically and the loop terminates once only fresh sections remain.
A budget computed once at loop start exhausts before reaching fresh
sections (observed: 2.37GB stored, abort with the robin at 171).

`section_create` failures (OOM/file open) log id + path and do not consume
the id slot.

## Operational notes

- **`offs cache size <bytes>` applies the cap LIVE over RPC** — no restart,
  no admin (it routes to `block_cache_resize`). Use it instead of editing
  `offs.json` + restarting; the config edit only matters for the next boot.
- The daemon's RPC pipe: the CLI hardcodes `/var/run/offs.sock`; the
  Windows pipe name keeps only the basename (`liboffs-offs.sock`) — a
  scratch/dev daemon cannot pick a different pipe the stock CLI reaches.
- The recurring hunt rule (see memory): every actor talks to another
  actor's state only through the actor API or atomics — the section layer
  is reached by callers only via the `sections_dispatch` sync seam (the
  block-cache actor) or messages.

## History

- a96865a..c583fca — the exhale/verify-only arc (see
  docs/network-functionality-audit.md).
- d148e4b — the section layer grows to the capacity ceiling (introduced).
- c4d75e8 — ceiling counted tuples, not bytes + the growth schedule.
- b0ebf5f — the write retry budget scales with the grown robin.
- a2f0df7 — section_create failures named + id-slot retry.
- efd5394 — the relay retry reuses its I/O thread (the ~1,045-thread spin).
- 41b16de / 6f0444a — the actor-boundary sweep (nothing outside the cache
  actor touches its LRU/index/sections directly).