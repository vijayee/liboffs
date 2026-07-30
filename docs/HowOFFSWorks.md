# liboffs and OFFS: From Brightnet Idea to Modern P2P Storage Daemon

## 1. What is OFFS?

OFFS, the Owner-Free File System, is a peer-to-peer storage system built around the idea of a brightnet. In a traditional darknet, participants hide the routes their traffic takes; in a brightnet, the stored data itself is anonymized while the network path remains ordinary and observable. OFFS achieves this by never storing complete files. Instead, it keeps only fixed-size blocks that look like random noise. A file is represented by a recipe, recorded separately, that tells the system how to recombine those blocks; the original content is reconstructed only when someone with the right identifier asks for it.

Because every block is content-addressed and shared across the network, the same random-looking block can appear in many unrelated files or representations. There is no discrete file-to-block mapping stored on any single node, so no individual participant can inspect its local cache and determine what it is actually holding. Possession of a block and knowledge of what that block means are deliberately separated.

Retrieval is driven by an OFF URL or ORI string: a compact identifier that names a representation and gives a client enough information to fetch the necessary blocks and reassemble the original data. Anyone who sees the traffic sees only the movement of undifferentiated random blocks; anyone who sees a node's disk sees only blocks whose purpose is unknown.

## 2. A brief history of the OFF System

The OFF System grew out of the hacktivism collective The Big Hack around 2003, with early design and prototyping by Cheater512, CaptainMorgan, Aqlo, and WhiteRaven. The first public proof of concept was a rudimentary PHP demo that circulated on CDs in 2004. It demonstrated the core idea, but it was not yet a practical network client.

The real mainline appeared when SpectralMorning re-implemented the system in C++. CaptainMorgan announced the public launch in 2006, promoting it as a copyless file system in which the network carries only random-looking data blocks. By April 2008, a beta test had reached more than one hundred nodes. Active development stopped when SpectralMorning halted work in late 2008, leaving the original project in maintenance mode with only minor bug-fix releases afterward.

Despite the slowdown, the ecosystem produced several alternative clients. BlocksNet, written in Ruby, was maintained from 2007 to 2011. OFFLoader offered a more minimal fork of the same ideas, and Monolith explored the block-recombination concept without any networking layer at all.

Today, liboffs/OFFS is a modern reinterpretation of the same brightnet idea by Prometheus-SCN. Where a darknet hides traffic routes, OFFS keeps the network open and instead anonymizes the data blocks themselves, continuing the original claim that no one can own mathematics or numbers.

## 3. Core data model: blocks, descriptors, and ORIs

OFFS stores data as fixed-size blocks rather than files. liboffs defines four block sizes so the system can choose the granularity that best fits a given payload:

- Mega: 1 MB
- Standard: 128 KB
- Mini: 64 KB
- Nano: 136 bytes

Every block is identified by its BLAKE3 hash. Because the hash is deterministic, identical blocks map to the same identifier, so the store keeps only one physical copy of any given block. This content-addressing model provides built-in deduplication across unrelated files: a block that appears in two different representations is stored once and referenced twice. The physical storage is organized into section files on disk, while the Index component records each hash-to-location mapping in a write-ahead log. On start-up the index replays that WAL to rebuild the hash-location tree; on every put the index is updated and logged before the promise resolves.

The OFF layer turns those raw blocks into retrievable representations. A file is described by an **ORI** (OFF Resource Identifier), a structure that records the descriptor hash, block type, tuple size, file offset, and final byte count needed to reconstruct the data. The string form people pass around is the **OFF URL**, which carries the same fields in a compact, URL-shaped encoding. Both forms are implemented in `src/OFFStreams/`.

The actual recipe for reassembly is a **tuple**: an ordered list of block hashes. When data is written, the `writeable_descriptor` stream splits the source into blocks, mixes each source block with randomizer blocks through XOR, and stores the resulting output blocks. Their hashes are collected into a tuple. When data is read, the `readable_descriptor` stream fetches the tuple's blocks from the cache or network and XORs them back together, yielding the original bytes. The raw blocks themselves remain indistinguishable from random noise; only someone who already holds the ORI or OFF URL knows which blocks to request and how to combine them.

Collections of files or directories are represented by **OFDs** (OFF File Directories). An OFD is a directory-like structure that maps names to either a file ORI or the hash of another OFD, so a single OFF URL can name an entire directory tree. The OFD itself is encoded and stored as a block, which means directories are content-addressed and deduplicated in the same way as ordinary data.

The implementation of these OFF structures lives in `src/OFFStreams/`, which handles ORI and OFF URL creation, OFD encoding, tuple management, and the readable/writeable descriptor streams that connect them to the block cache and network layers.

## 4. The three binaries: library, daemon, and CLI

OFFS ships as three cooperating artifacts instead of a single monolithic program: a C library, a long-running daemon, and a command-line client. This separation lets you embed the storage engine in your own application, run it as a background service, and manage it remotely without each tool duplicating code.

`liboffs` is the core C library. It owns the block cache, the OFF stream machinery, the scheduler and actor runtime, the peer-to-peer network layer, and the client API transport implementations. The block cache stores fixed-size blocks on disk and indexes them by BLAKE3 hash. OFFStreams provide the readable and writeable descriptors that turn those blocks into ORIs, OFDs, and tuples. The network layer implements QUIC gossip, relays, and peer discovery. ClientAPI exposes HTTP, Unix socket, TCP, WebSocket, and WebTransport endpoints. All other OFFS components are built directly on `liboffs`.

`offsd` is the daemon. It initializes the scheduler pool, loads configuration, starts the block, OFD, and tuple caches, opens the client transports, and then listens for peer connections and admin requests until it receives a shutdown signal. The daemon is meant to run as a system service and keep all local state across long sessions.

`offs` is the administration CLI. Modeled after Docker's daemon/client split, the binary itself performs no storage work. It opens a Unix socket to `offsd` and sends CBOR-encoded wire-protocol messages to start or stop the daemon, put and get files, inspect blocks, list peers, manage friends, and read or update configuration. The same CLI can target the local daemon or, when the client endpoint is reachable, a remote one.

```
┌───────────────────────────────────────────────┐
│                 APPLICATIONS                  │
│  offs CLI │ Flutter example │ future bindings │
└───────────────────────┬───────────────────────┘
                        │
┌───────────────────────┴───────────────────────┐
│                   ClientAPI                   │
│              HTTP / Unix / TCP /              │
│           WebSocket / WebTransport            │
└───────────────────────┬───────────────────────┘
                        │
┌───────────────────────┴───────────────────────┐
│                     offsd                     │
│                daemon built on                │
│                    liboffs                    │
└───────────────────────┬───────────────────────┘
                        │
┌───────────────────────┴───────────────────────┐
│                    liboffs                    │
│              BlockCache │ Network             │
│             OFFStreams │ ClientAPI            │
└───────────────────────────────────────────────┘
```

## 5. Inside `offsd`: from startup to shutdown

When `offsd` starts, it builds the runtime in a strict order so later layers can borrow the scheduler, caches, and identity from earlier ones. First the scheduler pool is created and started with the requested worker count (auto-detected from CPU cores if `--workers 0`). Then the timer actor is attached to that pool, giving every cache and network component a single coordinated clock. Configuration comes next, either from defaults, from the JSON config file, or from a pending config that was staged by `offs config reload`. With the config in hand, the daemon creates the block cache on disk, the OFD cache on top of it, and the tuple cache for in-flight reassembly metadata. If `--port` is non-zero, the HTTP server is created and registered later with the OFF, block, health, peer, and config routes. The authority/identity subsystem is loaded after the server exists but before the network starts, so TLS certificate paths and the local node identity are ready when peer connections are opened. The network layer is created around the authority, block cache, and timer. Only then are client transports wired up: the Unix socket listener on the `--unix` path, and on POSIX systems the auto-update actor. Route registration and `unix_transport_set_config_ctx` connect the admin endpoints to the running node.

Once startup succeeds, `offsd` opens its listeners. The HTTP server binds `--host` and `--port` (default 23402). The QUIC/P2P listener binds the same host on `--quic-port` (default 23401) for direct peer connections and NAT traversal. The Unix socket listens on the configured path for CBOR admin requests from `offs`. After printing its endpoints, the daemon enters a light main loop.

Shutdown is triggered by SIGINT or SIGTERM. The signal handler clears the node `running` flag; the main thread then tears everything down in reverse order: Unix transport, peer persistence and network stop, HTTP server, scheduler pool, and finally the caches, timer, authority, and config members. A config reload is handled as an in-process restart: the `config reload` RPC writes a pending config to `{data_dir}/pending_config.json` and sets a flag; the main loop shuts down, loads the pending override, and runs `_startup` again with the new config. This avoids the deadlock that would occur if a pool worker tried to restart while it was still using the shared scheduler pool.

## 6. Inside `liboffs`: the layers

`liboffs` is not a monolithic module. Its code is split into semantic layers under `src/`, each with a single responsibility and a narrow interface to the layers above and below it. The BlockCache layer owns the block store on disk; OFFStreams builds the owner-free file abstractions on top of it; Network moves blocks between peers; ClientAPI turns local operations into remote endpoints; and the Actor/Scheduler layer runs all of the above concurrently. Supporting infrastructure such as `Buffer`, `Streams`, and `RefCounter` is used across every layer. This layering keeps the storage engine, networking, and API surfaces independent, so you can replace or test each one without dragging the rest of the daemon into scope.

| Layer           | Directory                                    | Responsibility                                          |
|-----------------|----------------------------------------------|---------------------------------------------------------|
| BlockCache      | `src/BlockCache/`                            | Fixed-size block storage, LRU, index, sections          |
| OFFStreams      | `src/OFFStreams/`                            | ORI/OFD/tuple encoding and stream descriptors           |
| Network         | `src/Network/`                               | QUIC/P2P, gossip, relay, peer discovery                 |
| ClientAPI       | `src/ClientAPI/`                             | HTTP, Unix socket, TCP, WebSocket, WebTransport servers |
| Actor/Scheduler | `src/Actor/`, `src/Scheduler/`, `src/Timer/` | Async actor system and timing                           |

The BlockCache owns the physical store, keeping section files, maintaining the hash-to-location index with a write-ahead log, and caching hot blocks in memory with an LRU policy. OFFStreams turns user data into the OFF wire format: it splits a stream into fixed-size blocks, XORs each source block with randomizer blocks, records the output hashes in a tuple, and wraps the tuple in an ORI or OFD that can be encoded as an OFF URL. The Network layer implements direct QUIC peer connections, gossip for peer discovery and block availability, and relay-assisted NAT traversal; it also contains the timing wheel used for network timeouts. ClientAPI exposes the same operations through HTTP, Unix socket, TCP, WebSocket, and WebTransport servers so `offs`, the Flutter example, and future bindings share the daemon without duplicating protocol code. Finally, the Actor/Scheduler layer provides the asynchronous runtime: actors, message mailboxes, and a thread-pool scheduler that drives the timer actor and all concurrent work.

A block put or get therefore passes through the cache layer in a predictable pipeline:

```
PUT:                                 GET:
  data                                 hash
   │                                    │
   ▼                                    ▼
┌─────────────────┐              ┌─────────────────┐
│ split into      │              │ check LRU       │
│ fixed blocks    │              │ cache           │
└────────┬────────┘              └────────┬────────┘
         │                                │
         ▼                                ▼
┌─────────────────┐              ┌─────────────────┐
│ BLAKE3 hash     │              │ check index     │
└────────┬────────┘              └────────┬────────┘
         │                                │
         ▼                                ▼
┌─────────────────┐              ┌─────────────────┐
│ check index     │              │ read section    │
└────────┬────────┘              └────────┬────────┘
         │                                │
         ▼                                ▼
┌─────────────────┐              ┌─────────────────┐
│ write section   │              │ read block      │
└────────┬────────┘              └────────┬────────┘
         │                                │
         ▼                                ▼
┌─────────────────┐              ┌─────────────────┐
│ update index    │              │ add to LRU      │
└────────┬────────┘              └────────┬────────┘
         │                                │
         ▼                                ▼
┌─────────────────┐              ┌─────────────────┐
│ add to LRU      │              │ resolve promise │
└────────┬────────┘              └─────────────────┘
         │
         ▼
┌─────────────────┐
│ return promise  │
└─────────────────┘
```

Every put deduplicates on the index before writing to disk, and every get tries the in-memory LRU first before falling back to the section files. Because both paths return promises, callers see the same asynchronous interface. The network and client layers operate on the same promises, so a remote request follows the same path once it reaches the daemon.

## 7. The `offs` CLI in action

`offs` is the command-line companion to `offsd`. It follows Docker's daemon/client split: the binary performs no storage work itself. Instead it opens a Unix socket to a running `offsd` and exchanges CBOR-encoded wire-protocol messages. This keeps the administration tool small and lets it manage either the local daemon or any `offsd` whose Unix socket path is reachable.

The command surface is a single dispatch table. The available commands are `start`, `stop`, `restart`, `put`, `get`, `block`, `peer`, `config`, `friend`, `health`, `status`, `version`, and `help`. Most commands need an active daemon connection, but a few are handled client-side: `start`, `stop`, `restart`, and `version` never open a socket, and `config help` and `put --help` are pure client-side help.

The CLI is deliberately flexible about two global flags. Before it looks at the subcommand, `offs` scans the entire `argv` and lifts `--socket <path>` and `--lang <code>` out wherever they appear, consuming each value. The remaining arguments are compacted in place so the subcommand handler sees a clean `argv`. That means `offs --socket /tmp/offs.sock health` and `offs health --socket /tmp/offs.sock` both connect to the same socket. Language detection defaults from `OFFS_LANG`, then `LANG`, then falls back to English, and `--lang` overrides it.

Once the global flags are removed, the first remaining argument selects the handler from `cli_command_table()`. The matching handler lives in `OFFS/src/offs/commands/` (`put.c`, `get.c`, `peer.c`, `start_stop.c`, and the rest). Socket-requiring handlers receive a connected `cli_client_t` that encodes the request as CBOR and reads the daemon's response.

A typical foreground session looks like this:

```bash
# Start the daemon in the foreground
./offsd --foreground --cache-dir /tmp/offs-cache --data-dir /tmp/offs-data

# In another terminal
./offs health
./offs put ./README.md
./offs get <ori-string>
./offs peer list
```

## 8. Client libraries and bindings

liboffs exposes its operations through two main surfaces: a C client library in `src/ClientLibs/c/offs_client.h` for native applications, and plain HTTP endpoints that higher-level clients such as the Flutter example can call directly. Both surfaces talk to the same `offsd` daemon through ClientAPI.

The C API is the primary binding surface for native code. `offs_client_connect_ex(url, api_key, config)` opens a connection and applies retry/timeouts. For `wts://` (secure WebTransport) connections, `ca_path` and `allow_secure` in `offs_client_config_t` control TLS certificate validation: when `allow_secure` is true, a `ca_path` must be set or the connection is rejected. The client understands `unix://`, `tcp://`, `ws://`, `wss://`, `wt://`, and `wts://` transport URLs. Once connected, `offs_client_put_ex()` performs a buffered upload with recycler URLs and temporary-storage options, while the streaming variants `offs_client_put_stream_start_ex`, `offs_client_put_stream_data`, and `offs_client_put_stream_end` feed data incrementally. Retrieval is done with `offs_client_get(ori_string, ...)` which delivers chunks through a data callback and signals completion through an end callback. Lower-level block cache operations are available through `offs_client_block_put`, `offs_client_block_get`, and `offs_client_block_delete`, and `offs_client_health` returns a JSON health response. All results are delivered asynchronously through callback functions, so the caller must wait for the callback before disconnecting.

```c
static volatile int put_done = 0;

static void on_put_response(void* ctx, const char* ori_string) {
  printf("stored as: %s\n", ori_string);
  put_done = 1;
}

offs_client_config_t config = offs_client_config_default();
config.connect_timeout_ms = 5000;

offs_client_t* client = offs_client_connect_ex(
    "ws://localhost:23402", "secret-api-key", &config);

const uint8_t payload[] = "hello, OFFS";
offs_client_put_ex(client,
                   &(offs_put_options_t){
                     .content_type = "text/plain",
                     .file_name = "greeting.txt",
                     .stream_length = sizeof(payload) - 1,
                   },
                   payload, sizeof(payload) - 1,
                   on_put_response, NULL);

while (!put_done) {
  platform_sleep_ms(10);
}
offs_client_disconnect(client);
```

The Flutter example in `examples/off_client/lib/services/off_api.dart` takes a different approach. Its `OffApi` class speaks plain HTTP to the daemon, so it can run on any platform that supports Dart's `http` package. `uploadFile` streams a file to `PUT /offsystem` without loading the whole file into memory, setting headers such as `type`, `file-name`, `stream-length`, and optional `server-address`, `recycler`, and `temporary`. `downloadFile` fetches data from an OFF URL with a simple `GET`. The class also wraps `/health`, `/peer/info`, `/peer/connect`, and `/friends` for health checks, peer introspection, outbound peer connections, and friend-list management. API-key authentication is carried on admin endpoints via an `Authorization: Bearer` header.

Because the C API exposes connection, put, get, block, and health operations through stable C types and callbacks, it is the natural binding surface for other languages. Python, Go, Rust, and similar environments can wrap `offs_client_t` with FFI, cgo, or foreign-function interfaces, while mobile or web clients can follow the Flutter pattern and use the HTTP endpoints directly.

## 9. Security and trust model

OFFS uses a layered security model: transport-level credentials for peer and admin traffic, content-level anonymization through the OFF block transform, and no central authority that knows what any node is storing.

Peer and client transports are built on TLS-capable transports. The daemon loads `--ca-cert`, `--node-cert`, and `--node-key` into the `authority_t` identity subsystem. When `allow_secure` is true, the node requires a configured CA and validates peer certificates on QUIC, WebTransport, and relay connections, so encrypted links are also authenticated links. When `allow_secure` is false (the current default), connections are still encrypted but not authenticated against a CA, which is convenient for trusted-LAN or research deployments but leaves the node open to MITM on untrusted networks. The CLI and daemon let an operator opt into the stricter mode explicitly; the default is intentionally conservative for experimentation, not for public-internet deployment.

Admin endpoints on ClientAPI carry an API key via an `Authorization: Bearer` header. The HTTP server registers middleware around the OFF, block, peer, config, and friend routes, so management requests must present the configured key while ordinary storage retrieval does not require one. This separates peer-to-peer block exchange from node administration.

The brightnet property itself is a security primitive. No single node ever stores a complete file; it keeps only fixed-size blocks that are XOR-mixed with randomizer blocks and content-addressed by BLAKE3 hash. The same random-looking block can appear in many unrelated representations, and without the ORI or OFF URL there is no way to know which blocks belong to which content. Possession and meaning are deliberately separated: a node can prove it holds a block without revealing what that block is for.

Connectivity uses relay-assisted NAT traversal when direct QUIC peer connections are not possible. The relay server runs on a publicly reachable host; peers behind NAT can exchange rendezvous information through the relay and then attempt UDP hole punching for direct paths. mDNS is also used for same-LAN discovery, so nodes on the same local network can find each other without going through an external relay.

## 10. Current status and where it fits

liboffs/OFFS is a working research and developer preview of the brightnet idea, not a production-ready public storage network. The block cache, OFF stream machinery, actor/scheduler runtime, and multi-transport ClientAPI are implemented and testable; the daemon can start, store blocks, serve admin requests, and communicate with peers. Active development is concentrated in a few areas that are visible in `docs/Investigate.md` and the current tiered plans.

Multi-hop RPC behavior and memory-safety hardening are a primary focus. The multi-hop FindBlock/ClosestNodes path has known issues with colliding message IDs, missing request timeouts, and first-response-wins failure semantics, and the tiered plans call for monotonic IDs, a pending-request timeout sweep, proper not-found relaying, and fanout counting so a dead-end neighbor does not abort a still-live search. Memory-safety work is similarly staged: the tier-1 plan targets heap over-reads, payload leaks, and refcounter/actor teardown races that could be triggered by untrusted input, while the tier-2 plan addresses larger concurrency teardown ordering in the block-cache sections, QUIC connection shutdown, relay server, and WebTransport client paths.

Concurrency teardown and scheduler work runs alongside that. The scheduler, message queues, and actor lifecycle have already seen fixes for send-vs-destroy races and re-queue-vs-destroy use-after-free conditions, and further work is planned around sections dispatch, network shutdown sequencing, and relay/transport quiesce before freeing connection state.

NAT traversal and same-LAN peer discovery are partially in place. mDNS-based local discovery and relay-assisted hole punching have landed, and remaining work focuses on making hole punching more reliable and deciding how internal IP addresses should be included in connection information without leaking topology.

CLI/daemon streaming consistency is the fourth active area. The `offs` CLI and `offsd` daemon coordinate over a Unix socket CBOR protocol, and the current focus is on making put/get streaming behavior consistent across the CLI, C client library, and HTTP endpoints.

What ties these efforts together is the original brightnet promise: an open network where the data itself is anonymized. OFFS does not try to hide routes or timestamps; it makes every stored block look like random noise and lets the same block serve many unrelated files. The current implementation is a step toward that goal, and the ongoing work is aimed at making the network layer reliable and safe enough that the brightnet property can hold in practice as well as in theory.
