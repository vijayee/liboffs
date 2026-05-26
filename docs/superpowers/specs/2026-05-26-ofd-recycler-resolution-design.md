# OFD-Aware Recycler Resolution

## Summary

When a recycler URL points to an OFD (off file descriptor) whose name matches the folder being
uploaded, the client fetches the OFD contents and uses them to build per-file recycler lists.
Files matched by name get their old ORI as the primary recycler, supplemented by donor ORIs
from the OFD tree when the old ORI doesn't cover the new file's block count.

## Motivation

Currently the client passes the same flat list of recycler URLs to every file in a directory
upload. If a user re-uploads a modified directory and provides the previous upload's OFD as a
recycler, the server has no way to know which recycler ORI corresponds to which file. By having
the client resolve the OFD and build per-file recycler lists, the server can efficiently reuse
blocks from each file's previous version.

## Architecture

Changes stay entirely in the client libraries. No server-side or wire protocol changes.

### C Client Library (`src/ClientLibs/c/`)

**New file: `offs_ofd_resolver.h` / `.c`**
- `offs_ofd_resolve()` — fetches raw OFD from a URL (HTTP GET `?ofd=raw`), parses CBOR,
  returns entry list with ORIs
- `offs_ofd_build_recyclers()` — takes OFD entries + local file list (names/sizes),
  returns per-file recycler URL arrays using the greedy donor algorithm
- `offs_ofd_free_entries()` — cleanup

**Modified: `offs_client.h` / `.c`**
- `offs_client_fetch_raw()` — generic HTTP GET returning raw bytes (needed for OFD fetching)

### Flutter Client (`examples/off_client/`)

**New file: `lib/services/recycler_resolver.dart`**
- `resolveOfdRecyclers(String ofdUrl)` — fetches raw OFD, parses entries, recursively
  resolves sub-OFDs, returns `Map<String, OfdEntry>` and donor pool
- `buildRecyclerList(matched, fileSize, blockSize, donorPool, fallback)` — greedy donor
  algorithm
- Uses `OffApi.fetchRawOfd()` for HTTP calls

**Modified: `lib/screens/import_screen.dart`**
- `_uploadDirectory()` calls resolver when recycler URLs contain matching OFDs
- Donor pool passed down through recursive calls for subdirectory use

**Modified: `lib/services/off_api.dart`**
- `fetchRawOfd(String url)` — HTTP GET with `?ofd=raw`

## Data Flow

```
_uploadDirectory("photos/", recyclerUrls)

1. SCAN: Check recyclerUrls for OFDs matching "photos"
   → "photos.ofd" matches → fetch raw OFD from server

2. PARSE: Raw OFD CBOR → entries:
   ├── "IMG_001.jpg" → ori_t (final_byte, block_type, tuple_size, ...)
   ├── "IMG_002.jpg" → ori_t
   └── "subdir/"      → dir hash → fetch sub-OFD → more file ORIs

3. BUILD DONOR POOL: All file ORIs from this OFD + recursively fetched sub-OFDs
   → sorted by total_blocks descending

4. FOR EACH local file:
   a. Match by filename → matched ORI
   b. blocks_needed = total_blocks(file_size, block_type, tuple_size)
      blocks_covered = total_blocks(matched.final_byte, block_type, tuple_size)
   c. If shortfall > 0, greedy-select donors by total_blocks
   d. Append original non-OFD recycler URLs
   e. Upload file with tailored recycler list

5. FOR EACH subdirectory:
   → recurse into _uploadDirectory with sub-OFD URLs and accumulated donor pool
```

Non-OFD recycler URLs pass through unchanged and are appended after matched/donor ORIs.
Files with no match in the OFD get the full donor pool + fallback recyclers.

## Block Calculation

An ORI contributes both data blocks and descriptor blocks to the recycler pool. The formula
(derived from `js-offs/src/descriptor.js`):

```
data_blocks       = ceil(final_byte / block_size)
cut_point         = floor(block_size / descriptor_pad) * descriptor_pad
desc_data_per_blk = cut_point - descriptor_pad
desc_blocks       = ceil(data_blocks * descriptor_pad * tuple_size / desc_data_per_blk)
total_blocks      = data_blocks + desc_blocks
```

Where:
- `descriptor_pad` = hash size (32 for SHA-256)
- `block_size` depends on `block_type` (standard=128000, mini=1000, nano=136)
- `tuple_size` = 3 (2 data + 1 parity shard per tuple)

## Donor Selection Algorithm

```
function buildRecyclerList(matched, fileSize, blockType, tupleSize, donorPool, fallback):
    result = []
    blocksNeeded = total_blocks(fileSize, blockType, tupleSize)
    blocksCovered = 0

    // 1. Matched ORI first
    if matched != null:
        result.append(matched.toUrl())
        blocksCovered += total_blocks(matched.final_byte, blockType, tupleSize)

    // 2. Greedy donors by total_blocks descending
    for donor in donorPool:
        if blocksCovered >= blocksNeeded: break
        if donor.hash == matched.hash: continue   // skip self
        result.append(donor.toUrl())
        blocksCovered += total_blocks(donor.final_byte, donor.block_type, tupleSize)

    // 3. Fallback recyclers
    result.appendAll(fallback)
    return result
```

Edge cases:
- **No match:** `matched` is null, starts with donor pool, then fallback
- **File unchanged or smaller:** `blocksCovered >= blocksNeeded` immediately, no donors consumed
- **Donor pool exhausted:** remaining shortfall left for server to fill with new blocks
- **Zero-byte file:** `blocksNeeded=0`, no donors consumed

## OFD Detection

A recycler URL is treated as an OFD if it matches one of:
- Contains `offsystem/directory` in the path
- Filename ends with `.ofd`

Matching is done by URL pattern, not by fetching and inspecting content type.

## Recursive Sub-OFD Resolution

For each directory entry in the fetched OFD:
1. Construct the sub-OFD URL from the directory hash
2. Fetch the raw sub-OFD
3. Extract all file ORIs into the donor pool
4. Recurse into sub-sub-directories

This builds a complete donor pool from the entire OFD tree, making ORIs available
cross-directory.

## Error Handling

| Failure | Behavior |
|---------|----------|
| OFD fetch fails (network error, 404) | Skip that OFD, warn, use fallback recyclers |
| OFD CBOR parse fails | Skip, warn, use fallback recyclers |
| Sub-OFD fetch fails | That subtree contributes no donors; partial pool still used |
| Donor pool exhausted before shortfall covered | Upload proceeds with whatever was collected |

All OFD-related failures are non-fatal. The upload always proceeds, just with fewer
recycler entries than optimal.

## Testing

### C Library
- `offs_ofd_build_recyclers()` unit tests:
  - Exact match (file unchanged, no donors consumed)
  - File grew (donors consumed, enough available)
  - File grew (donors exhausted, partial coverage)
  - No match in OFD (donor pool only)
  - Empty OFD (only fallback recyclers)
  - Descriptor block counting (verify `total_blocks = data_blocks + desc_blocks`)
- Recursive sub-OFD fetching with mock HTTP responses
- `offs_ofd_resolve()` with mock valid/invalid CBOR responses
- Memory leak checks (valgrind) on entry/URL cleanup paths

### Flutter Client
- `buildRecyclerList()` unit tests (same cases as C library)
- Integration test: upload directory → modify one file → re-upload with first OFD as
  recycler → verify server reports fewer new blocks than a fresh upload
