# liboffs and OFFS: From Brightnet Idea to Modern P2P Storage Daemon

## 1. What is OFFS?

OFFS, the Owner-Free File System, is a peer-to-peer storage system built around the idea of a **brightnet**. In a darknet, participants hide traffic routes; in a brightnet, the stored data itself is anonymized while the network path remains ordinary. OFFS never stores complete files. It keeps only fixed-size blocks that look like random noise, plus separate recipes that describe how to recombine them. A file is reconstructed only when someone with the right identifier asks for it.

Because every block is content-addressed and shared across the network, the same random-looking block can appear in many unrelated files. No discrete file-to-block mapping is stored on any single node, so a participant cannot inspect its local cache and know what it is holding. Possession and meaning are deliberately separated. Retrieval is driven by an **OFF URL** or **ORI**: a compact identifier that names a representation and tells the client which blocks to fetch and how to XOR them back together.

## 2. A brief history of the OFF System

The OFF System grew out of the hacktivist collective The Big Hack around 2003. A PHP demo circulated on CDs in 2004, but the practical mainline arrived when SpectralMorning re-implemented the system in C++. CaptainMorgan announced the public launch in 2006 as a *copyless* file system, and by April 2008 a beta had reached more than one hundred nodes. Active development stopped when SpectralMorning halted work in late 2008, leaving the project in maintenance mode. Alternative clients such as BlocksNet, OFFLoader, and Monolith kept the idea alive in different forms.

Today, **liboffs/OFFS** is a modern reinterpretation by Prometheus-SCN. Where a darknet hides routes, OFFS keeps the network open and anonymizes the data blocks themselves, continuing the original claim that no one can own mathematics or numbers.

## 3. Core data model: blocks, descriptors, and ORIs

OFFS stores data as fixed-size blocks rather than files. liboffs defines four block sizes:

- Mega: 1 MB
- Standard: 128 KB
- Mini: 64 KB
- Nano: 136 bytes

Every block is identified by its BLAKE3 hash. Identical blocks map to the same identifier, so the store keeps one physical copy and references it many times. Blocks live in section files on disk, while the Index maps hashes to locations and replays a write-ahead log on startup.

The OFF layer turns raw blocks into retrievable representations. A file is described by an **ORI** (OFF Resource Identifier), a structure that records the descriptor hash, block type, tuple size, file offset, and final byte count. The string form people pass around is the **OFF URL**. The actual recipe is a **tuple**: an ordered list of block hashes. When data is written, the `writeable_descriptor` stream splits the source into blocks, XORs each source block with randomizer blocks, and stores the output blocks; their hashes become a tuple. When data is read, the `readable_descriptor` stream fetches those blocks and XORs them back together. Only someone who already holds the ORI or OFF URL knows which blocks to request.

Collections of files or directories are represented by **OFDs** (OFF File Directories). An OFD maps names to either a file ORI or the hash of another OFD, so a single OFF URL can name an entire directory tree. OFDs are themselves encoded and stored as blocks, making directories content-addressed and deduplicated just like ordinary data.

All of these structures live in `src/OFFStreams/`.

## 4. The three binaries: library, daemon, and CLI

OFFS ships as three cooperating artifacts: a C library, a long-running daemon, and a command-line client. This separation lets you embed the storage engine, run it as a background service, or manage it remotely without duplicating code.

- **`liboffs`** is the core C library. It owns the block cache, OFF stream machinery, scheduler/actor runtime, peer-to-peer network layer, and ClientAPI transports.
- **`offsd`** is the daemon. It initializes the scheduler pool, loads configuration, starts the caches, opens the client transports, and listens for peer connections and admin requests until shutdown.
- **`offs`** is the administration CLI. Modeled after Docker's daemon/client split, it performs no storage work itself; it opens a Unix socket to `offsd` and sends CBOR-encoded messages to put, get, inspect, and manage the node.

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

When `offsd` starts, it builds the runtime in a strict order so later layers can borrow the scheduler, caches, and identity from earlier ones:

1. Scheduler pool and timer actor
2. Configuration (defaults, JSON file, or pending reload)
3. Block cache, OFD cache, and tuple cache
4. HTTP server (if `--port` is non-zero)
5. Authority / identity subsystem
6. Network layer
7. Unix transport listener
8. POSIX auto-update actor

Once startup succeeds, `offsd` opens its listeners. The HTTP server binds `--host` and `--port` (default 23402). The QUIC/P2P listener binds the same host on `--quic-port` (default 23401) for direct peer connections and NAT traversal. The Unix socket listens on the configured path for CBOR admin requests from `offs`.

Shutdown is triggered by SIGINT or SIGTERM. The main thread tears everything down in reverse order, saving peers before stopping the network. A config reload is handled as an in-process restart: the `config reload` RPC writes a pending config to `{data_dir}/pending_config.json` and sets a flag; the main loop shuts down, loads the override, and runs startup again with the new config. This avoids the deadlock that would occur if a pool worker tried to restart while still using the shared scheduler pool.

## 6. Inside `liboffs`: the layers

`liboffs` is split into semantic layers under `src/`, each with a single responsibility. Supporting infrastructure such as `Buffer`, `Streams`, and `RefCounter` is used across every layer.

| Layer | Directory | Responsibility |
|-----------------|----------------------------------------------|---------------------------------------------------------|
| BlockCache | `src/BlockCache/` | Fixed-size block storage, LRU, index, sections |
| OFFStreams | `src/OFFStreams/` | ORI/OFD/tuple encoding and stream descriptors |
| Network | `src/Network/` | QUIC/P2P, gossip, relay, peer discovery, timing wheel |
| ClientAPI | `src/ClientAPI/` | HTTP, Unix socket, TCP, WebSocket, WebTransport servers |
| Actor/Scheduler | `src/Actor/`, `src/Scheduler/`, `src/Timer/` | Async actor system and timing |

A block put or get passes through the cache layer in a predictable pipeline:

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

Every put deduplicates on the index before writing to disk, and every get tries the in-memory LRU before falling back to the section files. Because both paths return promises, callers see the same asynchronous interface whether the request is local or remote.

## 7. The `offs` CLI in action

`offs` is the command-line companion to `offsd`. It performs no storage work itself; it opens a Unix socket to a running `offsd` and exchanges CBOR-encoded messages. The available commands are `start`, `stop`, `restart`, `put`, `get`, `block`, `peer`, `config`, `friend`, `health`, `status`, `version`, and `help`.

Most commands need an active daemon connection, but a few are handled client-side: `start`, `stop`, `restart`, and `version` never open a socket, and some `--help` variants are pure client-side help. Global flags `--socket <path>` and `--lang <code>` are accepted anywhere in the command line, so `offs --socket /tmp/offs.sock health` and `offs health --socket /tmp/offs.sock` both connect to the same socket. Language detection defaults from `OFFS_LANG`, then `LANG`, then falls back to English.

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

liboffs exposes its operations through two surfaces: a C client library for native applications, and plain HTTP endpoints for higher-level clients such as the Flutter example. Both talk to the same `offsd` daemon through ClientAPI.

The C API in `src/ClientLibs/c/offs_client.h` is the primary binding surface. `offs_client_connect_ex(url, api_key, config)` opens a connection and applies retry/timeouts; it understands `unix://`, `tcp://`, `ws://`, `wss://`, `wt://`, and `wts://` URLs. For `wts://` connections, `ca_path` and `allow_secure` control TLS certificate validation. Buffered upload is done with `offs_client_put_ex()`, and streaming upload uses `offs_client_put_stream_start_ex`, `offs_client_put_stream_data`, and `offs_client_put_stream_end`. Retrieval uses `offs_client_get()`, block operations use `offs_client_block_put/get/delete`, and `offs_client_health` returns a JSON health response. All results arrive asynchronously through callbacks.

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

The Flutter example in `examples/off_client/lib/services/off_api.dart` takes the HTTP route. Its `OffApi` class streams a file to `PUT /offsystem`, fetches data with `GET` on an OFF URL, and wraps `/health`, `/peer/info`, `/peer/connect`, and `/friends` for health checks and peer management. API-key authentication is carried on admin endpoints via an `Authorization: Bearer` header.

Other languages can wrap the C API with FFI, cgo, or similar interfaces, while mobile or web clients can follow the Flutter pattern and use the HTTP endpoints directly.

## 9. Security and trust model

OFFS uses a layered security model: transport-level credentials for peer and admin traffic, content-level anonymization through the OFF block transform, and no central authority that knows what any node is storing.

Peer and some client transports are TLS-capable. The daemon loads `--ca-cert`, `--node-cert`, and `--node-key` into the `authority_t` identity subsystem. When `allow_secure` is true, the node requires a configured CA and validates peer certificates on QUIC, WebTransport, and relay connections. When `allow_secure` is false (the current default), connections are encrypted but not authenticated against a CA, which is convenient for trusted-LAN or research deployments but not for untrusted networks. The HTTP admin endpoint started by `offsd` is currently plain HTTP and relies on API-key bearer auth rather than TLS client auth.

When an API key hash is configured, `off_routes_register` installs a single global auth middleware that runs before every HTTP request, including OFF storage retrieval, block management, peer management, and config endpoints. Block and peer routes are only registered when auth is enabled, and config routes additionally allow local-loopback requests to bypass the token check.

The brightnet property itself is a security primitive. No single node ever stores a complete file; it keeps only fixed-size blocks that are XOR-mixed and content-addressed by BLAKE3 hash. The same random-looking block can appear in many unrelated representations, and without the ORI or OFF URL there is no way to know which blocks belong to which content. Possession and meaning are deliberately separated.

Connectivity uses relay-assisted NAT traversal when direct QUIC peer connections are not possible, plus mDNS for same-LAN discovery.

## 10. Current status and where it fits

liboffs/OFFS is a working research and developer preview of the brightnet idea, not a production-ready public storage network. The block cache, OFF stream machinery, actor/scheduler runtime, and multi-transport ClientAPI are implemented and testable; the daemon can start, store blocks, serve admin requests, and communicate with peers. Active development is concentrated in the areas listed in `docs/Investigate.md` and the current tiered plans: multi-hop RPC behavior and memory-safety hardening, concurrency teardown and scheduler ordering, relay/NAT hole punching and same-LAN discovery, and CLI/daemon streaming consistency across the Unix socket, C client library, and HTTP endpoints.

What ties these efforts together is the original brightnet promise: an open network where the data itself is anonymized. OFFS does not try to hide routes or timestamps; it makes every stored block look like random noise and lets the same block serve many unrelated files. The current implementation is a step toward that goal, and the ongoing work is aimed at making the network layer reliable and safe enough that the brightnet property can hold in practice as well as in theory.
