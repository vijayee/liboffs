# liboffs / OFFS Summary Article Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write and commit a conference-deck-style Markdown article at `docs/HowOFFSWorks.md` that explains the liboffs/OFFS stack, its CLI tools, client libraries, and architecture, using the OFF System history as an introduction hook.

**Architecture:** A single documentation file with inline ASCII diagrams, CLI snippets, and API examples. No code changes outside the new article and the existing design spec.

**Tech Stack:** Markdown; source material drawn from existing docs, READMEs, and headers.

---

## File Structure

- **Create:** `docs/HowOFFSWorks.md` — the article itself.
- **Read-only reference material:**
  - `docs/ARCHITECTURE.md`
  - `OFFS/README.md`
  - `OFFS/src/offsd/main.c`
  - `OFFS/src/offs/main.c`
  - `OFFS/src/offs/cli_util.c`
  - `src/ClientLibs/c/offs_client.h`
  - `examples/off_client/lib/services/off_api.dart`
- **Already exists:** `docs/superpowers/specs/2026-07-30-liboffs-article-design.md` — the approved spec.

---

### Task 1: Create article skeleton with headings

**Files:**
- Create: `docs/HowOFFSWorks.md`

- [ ] **Step 1: Create the file and write the heading outline**

```markdown
# liboffs and OFFS: From Brightnet Idea to Modern P2P Storage Daemon

## 1. What is OFFS?

## 2. A brief history of the OFF System

## 3. Core data model: blocks, descriptors, and ORIs

## 4. The three binaries: library, daemon, and CLI

## 5. Inside `offsd`: from startup to shutdown

## 6. Inside `liboffs`: the layers

## 7. The `offs` CLI in action

## 8. Client libraries and bindings

## 9. Security and trust model

## 10. Current status and where it fits
```

- [ ] **Step 2: Commit the skeleton**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add HowOFFSWorks.md skeleton"
```

---

### Task 2: Write the introduction and history sections

**Files:**
- Modify: `docs/HowOFFSWorks.md`

- [ ] **Step 1: Write Section 1 — What is OFFS?**

Explain the brightnet / owner-free pitch in ~250 words: data is never stored as whole files, only as random-looking blocks; blocks can be reused across many files; retrieval uses an OFF URL; no discrete file-to-block mapping means no single node knows what it stores.

- [ ] **Step 2: Write Section 2 — Brief history**

Cover the original OFF System from The Big Hack (2003), public launch (2006), beta 100-node milestone (2008), SpectralMorning C++ mainline, and modern re-interpretation in liboffs/OFFS. Keep it to ~200 words.

- [ ] **Step 3: Commit the sections**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add OFFS intro and history sections"
```

---

### Task 3: Write the core data model section

**Files:**
- Modify: `docs/HowOFFSWorks.md`
- Read-only reference: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Document the block model**

Include:
- Fixed-size blocks: Mega (1 MB), Standard (128 KB), Mini (64 KB), Nano (136 bytes).
- Content addressing via BLAKE3 hash.
- Deduplication.
- Section files and the index WAL.

- [ ] **Step 2: Document the OFF data model**

Include:
- ORI string as the public retrieval key.
- OFD descriptor that records the tuple of blocks.
- Tuple encoding: source block XOR randomizer blocks => output block.
- Mention `src/OFFStreams/` as the implementation home.

- [ ] **Step 3: Commit the data model section**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add core data model section"
```

---

### Task 4: Write the three-binaries overview with an architecture diagram

**Files:**
- Modify: `docs/HowOFFSWorks.md`

- [ ] **Step 1: Describe the three binaries**

- `liboffs` — C library: block cache, streams, network, scheduler.
- `offsd` — long-running daemon built on `liboffs`.
- `offs` — CLI client that talks to `offsd` over Unix socket (CBOR/RPC), modeled after Docker's daemon/client split.

- [ ] **Step 2: Insert an ASCII architecture diagram**

```
┌─────────────────────────────────────────────┐
│                 APPLICATIONS                  │
│  offs CLI │ Flutter example │ future bindings│
└──────────────────┬────────────────────────────┘
                   │
        ┌──────────┴──────────┐
        │   ClientAPI           │
        │  HTTP / Unix / TCP /  │
        │  WebSocket / WebTransport│
        └──────────┬──────────────┘
                   │
        ┌──────────┴──────────┐
        │       offsd         │
        │  daemon built on    │
        │      liboffs        │
        └──────────┬──────────┘
                   │
        ┌──────────┴──────────┐
        │      liboffs          │
        │  BlockCache │ Network │
        │  OFFStreams │ ClientAPI│
        └───────────────────────┘
```

- [ ] **Step 3: Commit the binaries overview**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add three-binaries overview and diagram"
```

---

### Task 5: Write the `offsd` section

**Files:**
- Modify: `docs/HowOFFSWorks.md`
- Read-only reference: `OFFS/src/offsd/main.c`

- [ ] **Step 1: Document startup sequence**

List subsystem creation order:
1. Scheduler pool
2. Timer actor
3. Configuration
4. Block cache
5. OFD cache
6. Tuple cache
7. HTTP server (if enabled)
8. Authority / identity
9. Network
10. Unix transport
11. Update actor (POSIX)

- [ ] **Step 2: Document listeners and lifecycle**

- HTTP listener on `--port` (default 23402)
- QUIC/P2P listener on `--quic-port` (default 23401)
- Unix socket listener on `--unix` path
- Graceful shutdown in reverse order on SIGINT/SIGTERM
- Config reload path via pending config and in-process restart

- [ ] **Step 3: Commit the `offsd` section**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add offsd startup and lifecycle section"
```

---

### Task 6: Write the `liboffs` layers section

**Files:**
- Modify: `docs/HowOFFSWorks.md`
- Read-only reference: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Describe each layer in a table**

| Layer | Directory | Responsibility |
|-------|-----------|----------------|
| BlockCache | `src/BlockCache/` | Fixed-size block storage, LRU, index, sections |
| OFFStreams | `src/OFFStreams/` | ORI/OFD/tuple encoding and stream descriptors |
| Network | `src/Network/` | QUIC/P2P, gossip, relay, peer discovery |
| ClientAPI | `src/ClientAPI/` | HTTP, Unix socket, TCP, WebSocket, WebTransport servers |
| Actor/Scheduler | `src/Actor/`, `src/Scheduler/`, `src/Timer/` | Async actor system and timing |

- [ ] **Step 2: Add the block-cache data-flow ASCII diagram**

Re-use or adapt the put/get flow from `docs/ARCHITECTURE.md`:

```
PUT:  data -> split -> BLAKE3 -> check index -> write section -> update index -> LRU -> promise
GET:  hash -> check LRU -> index -> section -> read block -> LRU -> resolve promise
```

- [ ] **Step 3: Commit the liboffs layers section**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add liboffs layers section"
```

---

### Task 7: Write the `offs` CLI section

**Files:**
- Modify: `docs/HowOFFSWorks.md`
- Read-only references: `OFFS/src/offs/main.c`, `OFFS/src/offs/cli_util.c`, `OFFS/README.md`

- [ ] **Step 1: Document the command list and architecture**

Commands: `start`, `stop`, `restart`, `put`, `get`, `block`, `peer`, `config`, `friend`, `health`, `status`, `version`, `help`.

CLI lifts `--socket` and `--lang` from anywhere in argv, then dispatches via `cli_command_table()` to handlers in `OFFS/src/offs/commands/`.

- [ ] **Step 2: Include CLI usage snippet**

```bash
# Start the daemon in the foreground
./offsd --foreground --cache-dir /tmp/offs-cache --data-dir /tmp/offs-data

# In another terminal
./offs health
./offs put ./README.md
./offs get <ori-string>
./offs peer list
```

- [ ] **Step 3: Commit the CLI section**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add offs CLI section"
```

---

### Task 8: Write the client libraries and bindings section

**Files:**
- Modify: `docs/HowOFFSWorks.md`
- Read-only references: `src/ClientLibs/c/offs_client.h`, `examples/off_client/lib/services/off_api.dart`

- [ ] **Step 1: Document the C client API**

List key functions:
- `offs_client_connect_ex(url, api_key, config)`
- `offs_client_put_ex()` and streaming variants
- `offs_client_get()`
- `offs_client_block_put/get/delete()`
- `offs_client_health()`

Include a minimal C snippet showing connect + put.

- [ ] **Step 2: Document the HTTP/Flutter example**

Describe how the Flutter `OffApi` class uses plain HTTP endpoints:
- `PUT /offsystem` for streaming upload
- `GET <off-url>` for download
- `/health`, `/peer/info`, `/peer/connect`, `/friends`

- [ ] **Step 3: Note the FFI path to other languages**

The C API is the primary binding surface; other languages can bind via FFI/cgo/etc.

- [ ] **Step 4: Commit the client libraries section**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add client libraries and bindings section"
```

---

### Task 9: Write the security and status sections

**Files:**
- Modify: `docs/HowOFFSWorks.md`
- Read-only reference: `docs/PRODUCTION_BLOCKERS.md` if relevant

- [ ] **Step 1: Document the security model**

- TLS/mTLS cert paths (`--ca-cert`, `--node-cert`, `--node-key`) loaded into the authority.
- API key auth on admin endpoints.
- No single node stores a whole file; blocks are random-looking and multi-used.
- Relay-assisted NAT traversal for peer connectivity.

- [ ] **Step 2: Document current status**

Reference the project's active focus areas from `docs/Investigate.md` and recent plans:
- Multi-hop RPC behavior and memory-safety hardening.
- Concurrency teardown and scheduler work.
- Relay/NAT hole punching and same-LAN peer discovery.
- CLI/daemon streaming consistency.

Keep it factual and avoid uncommitted promises.

- [ ] **Step 3: Commit the final sections**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: add security model and current status sections"
```

---

### Task 10: Final review and cleanup

**Files:**
- Modify: `docs/HowOFFSWorks.md`

- [ ] **Step 1: Scan for red flags**

Search the article for:
- `TODO`, `FIXME`, `TBD`, `XXX`
- Incomplete sentences or placeholder phrases
- Broken links or missing references

Fix any issues inline.

- [ ] **Step 2: Verify word count and diagram count**

Expected: ~2000–2500 words, at least 2 ASCII diagrams.

- [ ] **Step 3: Run a markdown render check**

```bash
# Quick syntax sanity check via git diff preview
git diff -- docs/HowOFFSWorks.md
```

- [ ] **Step 4: Final commit if any changes**

```bash
git add docs/HowOFFSWorks.md
git commit -m "docs: final polish on HowOFFSWorks article"
```

---

## Spec coverage check

| Spec section | Implementing task |
|--------------|-------------------|
| What is OFFS? | Task 2 |
| Historical nod | Task 2 |
| Core data model | Task 3 |
| The three binaries | Task 4 |
| Inside `offsd` | Task 5 |
| Inside `liboffs` | Task 6 |
| `offs` CLI | Task 7 |
| Client libraries and bindings | Task 8 |
| Security and trust model | Task 9 |
| Status and where it fits | Task 9 |
| ≥2 ASCII diagrams | Tasks 4 and 6 |
| No TODO/FIXME | Task 10 |
| Committed to repo | Every task |

## Placeholder scan

- No `TBD`, `TODO`, `FIXME`, or `implement later` in tasks.
- No "write tests" without test code (this plan produces a doc; no test code needed).
- All file paths are exact.
- All source references are concrete.
