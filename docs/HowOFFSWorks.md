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
┌─────────────────────────────────────────────┐
│                 APPLICATIONS                  │
│  offs CLI │ Flutter example │ future bindings│
└──────────────────┬────────────────────────────┘
                   │
        ┌──────────┴──────────┐
        │      ClientAPI        │
        │  HTTP / Unix / TCP /  │
        │ WebSocket / WebTransport │
        └──────────┬──────────────┘
                   │
        ┌──────────┴──────────┐
        │       offsd         │
        │  daemon built on    │
        │      liboffs        │
        └──────────┬──────────┘
                   │
        ┌──────────┴──────────┐
        │       liboffs         │
        │  BlockCache │ Network │
        │ OFFStreams │ ClientAPI│
        └───────────────────────┘
```

## 5. Inside `offsd`: from startup to shutdown

## 6. Inside `liboffs`: the layers

## 7. The `offs` CLI in action

## 8. Client libraries and bindings

## 9. Security and trust model

## 10. Current status and where it fits
