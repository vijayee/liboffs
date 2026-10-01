# GC Keep-Filter Sizing

The keep-list garbage collection (`block_gc_create`, `src/BlockCache/block_gc.c`)
seeds one elastic bloom filter (see `src/Bloom/elastic_bloom_filter.h`) that
holds every block a keep-list URL must spare. This document records the exact
sizing equation for that filter and why it is computable in advance.

## The equation

For one keep-list line whose OFF URL parses (url/ori carry the stream extent —
`off_url_to_string` embeds `stream_length`, `off_url_parse` recovers it), with
the cache's block-size class `B = representation_block_size_for_type(type)`
(mega 1000000 / standard 128000 / mini 64000 / nano 136) and the descriptor pad
`P = 32` (`REPRESENTATION_DESCRIPTOR_PAD` in
`src/OFFStreams/representation_actor.h`):

    data_blocks      = ceil(stream_length / B)
    entries_per_desc = B / P - 1          (standard: 128000/32 - 1 = 3999)
    descriptor_blocks= max(1, ceil(data_blocks / entries_per_desc))
    expected_blocks  = sum over lines of (data_blocks + descriptor_blocks)

The seeded filter budget is then `expected_blocks * 16` bits, floored at 1024,
with `BLOCK_GC_EBF_HASH_COUNT = 4` hashes.

## Why the arithmetic works

One descriptor pad is consumed per data block (every data hash stored in a
descriptor is a fixed 32-byte pad slice), and each descriptor block divides
into `B / P` pads — but the trailing pad of every descriptor block carries the
next-descriptor pointer instead of an entry, so `B / P - 1` entry pads fit per
descriptor block. The head descriptor block exists even for a zero-extent
stream (the walk always adds its own hash), hence the `max(1, ...)`.
`stream_offset` is deliberately ignored: an overcount only buys filter room,
and a false positive in the filter merely spares a block — both safe
directions. An over-count never loses blocks, and the filter still elastically
expands on saturation (`elastic_bloom_filter_add`) as the belt-and-braces for
anything the URL arithmetic cannot see.

Unparseable lines contribute 0 to the estimate (collect reports them as
`GC_LINE_MALFORMED_URL`; they keep nothing either way). If every line is
unparseable the sweep is refused at collect end anyway, but `block_gc_create`
still seeds via the fallback `line_count * BLOCK_GC_EXPECTED_PER_URL` so the
refused run has a working filter for its report.

## Where it lives

- Estimator: `block_gc_expected_blocks()` in `src/BlockCache/block_gc.c`
  (contract in `block_gc.h`).
- Descriptor geometry: `_rep_process_descriptor_block()` in
  `src/OFFStreams/representation_actor.c` and
  `representation_block_size_for_type()` (made public for shared use).
- Seeding site: `block_gc_create()`.

## Verification

`test/test_gc_collect.cpp::TestGcCollect.KeepFilterSizedExactlyFromUrls`
locks the arithmetic: a standard-class cache sized from URLs of extent
128000 (2 expected), 384001 (5), and 640000000 (5002), plus one junk line,
must total 5009; the junk line alone must total 0.