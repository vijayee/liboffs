# Design: liboffs / OFFS Summary Article

## Goal
Produce a slide-ready Markdown article in `docs/` that explains how the liboffs/OFFS codebase works for a technical audience, using the OFF System history as a framing hook.

## Audience and tone
Conference-deck article form: high-level concepts, architecture diagrams, CLI demo flow, and client library coverage, ~2000–2500 words. Technical but approachable; ASCII diagrams inline so they survive as presentation source.

## Structure

1. **What is OFFS?** — The 2-minute pitch: brightnet / owner-free storage, random data blocks, URL-based retrieval.
2. **Historical nod** — Original Big Hack / OFF System (2003–2008), brightnet vs. darknet, the XOR-block idea.
3. **Core data model** — Fixed-size blocks (mega/standard/mini/nano), XOR tuple encoding, BLAKE3 content addressing, ORI strings, OFD descriptors, deduplication.
4. **The three binaries** — `liboffs` (library), `offsd` (daemon), `offs` (CLI); Docker-style daemon/client split.
5. **Inside `offsd`: startup to shutdown** — Scheduler pool, block cache, OFD cache, tuple cache, authority/identity, HTTP + Unix + QUIC listeners, config reload, signal handling.
6. **Inside `liboffs`: layers** — BlockCache, OFFStreams, Network (QUIC/P2P + gossip + relay), ClientAPI transports (HTTP/Unix/TCP/WS/WT), actor/scheduler/timer infrastructure.
7. **`offs` CLI in action** — put/get/block/peer/config/friend/health/status, Unix socket CBOR wire, language detection.
8. **Client libraries and bindings** — C client API (`offs_client_*`), HTTP endpoints consumed by the Flutter example, FFI path to other languages.
9. **Security and trust model** — TLS/mTLS, API key auth, peer verification, relay/NAT traversal, no single node holds a whole file.
10. **Status and where it fits** — Current focus areas (network RPC, memory safety, concurrency), production blockers, how the architecture maps to the original brightnet promise.

## Source material
- `docs/ARCHITECTURE.md` for liboffs internals and block-cache flow.
- `OFFS/README.md` for daemon/CLI architecture and command list.
- `OFFS/src/offsd/main.c` for startup/shutdown and subsystem wiring.
- `OFFS/src/offs/main.c` and `cli_util.c` for CLI dispatch and command table.
- `src/ClientLibs/c/offs_client.h` for the public C API.
- `examples/off_client/lib/services/off_api.dart` for a concrete HTTP client example.
- `https://www.off.systems/` for the current project’s elevator pitch.
- `https://en.wikipedia.org/wiki/OFFSystem` for the historical background.

## Output location
`docs/HowOFFSWorks.md`

## Acceptance criteria
- Article covers all 10 sections above with code/CLI snippets.
- Includes at least two ASCII architecture diagrams.
- Uses the OFF System history as an introduction hook but keeps the focus on the current stack.
- No TODO/FIXME comments or uncommitted placeholders.
- Committed to the repo with a conventional commit message.
