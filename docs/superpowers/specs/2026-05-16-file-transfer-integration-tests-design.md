# File Transfer Integration Tests

## Overview

Fully distributed integration tests that exercise the OFF stream file transfer pipeline between separate node processes communicating via QUIC (direct and relay). Tests verify that blocks propagate through the network and that late-joining nodes can fetch files for the first time via the readable_off_stream's OFF_STREAM_AWAITING_NETWORK path.

## Architecture

A single test binary (`test_file_transfer_integration`) runs in three modes selected by command-line flag:

- **coordinator** (default) — runs gtest, spawns relay and node processes, orchestrates test scenarios
- **relay** — runs as relay server on a given port
- **node** — runs as a full network node with OFF stream support and a TCP control socket

```
┌──────────────┐       ┌─────────────┐
│  Coordinator │──────▶│  Relay Srv  │
│  (gtest)     │       │  :14000     │
└──────┬───────┘       └─────────────┘
       │ fork/exec
       │
┌──────▼───────┐  QUIC/relay  ┌──────────────┐
│   Node A     │◀────────────▶│   Node B     │
│  (store)     │              │  (fetch)      │
└──────────────┘              └──────────────┘
       ▲                            ▲
       │    TCP control socket      │
       └────────────────────────────┘
                   coordinator
```

The coordinator spawns node processes by re-executing the same binary with `--mode=node` flags. The existing `relay_server` binary (built from `src/Network/Relay/relay_server_main.c`) is spawned as a separate process for the relay — no modifications to the relay server needed.

### Mode Flags

- No flag or `--mode=coordinator` — gtest coordinator
- `--mode=node --port PORT --relay-host HOST --relay-port PORT --control-port PORT --cache-dir PATH` — network node with control socket

## File Transfer Flow

The test data flow mirrors the HTTP PUT/GET path but without HTTP, exercising the full OFF stream pipeline:

### Upload (Node A)

1. Create `new_blocks_recipe_t` (generates random blocks for XOR encoding)
2. Create `writeable_off_stream_t` with the recipe, block_cache, and tuple_cache
3. Create `writeable_descriptor_t` to collect tuples
4. Subscribe: `ws` data_event → descriptor write; `ws` close_event → descriptor close
5. Subscribe: `desc` data_event → capture descriptor_hash
6. Write upload data into the writeable_off_stream
7. Call `writeable_off_stream_finalize()` to trigger final BLAKE3 hash and block processing
8. Descriptor close builds linked descriptor blocks and emits descriptor_hash
9. Construct ORI: `{descriptor_hash, file_hash, block_type, tuple_size, final_byte}`

Result: blocks are stored in block_cache and announced via NETWORK_LOCAL_STORE_BLOCK to the network actor, which gossips them to connected peers.

### Download (Node B, possibly late-joining)

1. Create `ori_t` from the upload result (descriptor_hash, file_hash, block_type, tuple_size, final_byte)
2. Create `readable_descriptor_t` and `readable_off_stream_t`
3. Pipe: descriptor data_event → readable_off_stream write; descriptor close_event → stream close
4. Subscribe: readable_off_stream data_event → accumulate decoded data
5. Call `readable_descriptor_push()` to start the pipeline
6. On block cache miss: readable_off_stream enters OFF_STREAM_AWAITING_NETWORK, sends NETWORK_LOCAL_FIND_BLOCK
7. Network routes FindBlock to a peer that has the block, returns NETWORK_FIND_BLOCK_RESULT
8. Readable_off_stream re-issues block_cache_get, XOR decodes, renders original data
9. Verify checksum of decoded data matches the original

## Control Protocol

Each node process opens a TCP control socket on a designated port. The coordinator connects to this socket and sends line-based text commands. The node responds with line-based text responses.

### Commands (coordinator → node)

| Command | Parameters | Description |
|---------|-----------|-------------|
| `STORE_FILE` | `<size> <block_type> <tuple_size>` | Generate random data, upload via writeable_off_stream + writeable_descriptor. `block_type` is the integer enum value (0=nano, 1=mini, 2=standard, 3=mega). |
| `FETCH_FILE` | `<descriptor_hash> <file_hash> <final_byte> <block_type> <tuple_size>` | Download via readable_descriptor + readable_off_stream. `block_type` is the integer enum value. |
| `PEER_ADD` | `<host:port>` | Add a peer address for direct connection |
| `CONNECT_RELAY` | `<host:port>` | Connect to a relay server |
| `WAIT_FOR_PEER` | `<count>` | Block until `<count>` peers are connected |
| `STATUS` | | Report connected peers, cache stats, relay status, NAT type |
| `SHUTDOWN` | | Gracefully terminate the node process |

### Responses (node → coordinator)

| Response | Meaning |
|----------|---------|
| `OK <detail>` | Command succeeded |
| `HASH <descriptor_hash> <file_hash> <final_byte>` | Response to STORE_FILE |
| `DATA <hex_checksum> <size>` | Response to FETCH_FILE (BLAKE3 checksum of decoded data) |
| `STATUS peers=<N> blocks=<M> relay=<connected\|disconnected> nat=<type>` | Response to STATUS |
| `ERROR <msg>` | Command failed |

## Test Fixture

```cpp
class FileTransferIntegrationTest : public ::testing::Test {
protected:
    struct Process {
        pid_t pid;
        uint16_t control_port;
        int control_fd;
        std::string cache_dir;
    };

    Process relay;
    std::vector<Process> nodes;
    static std::atomic<uint16_t> next_base_port;

    void SetUp() override;
    void TearDown() override;

    Process start_relay(uint16_t port);
    Process start_node(uint16_t node_port, uint16_t control_port,
                       uint16_t relay_port, const std::string& cache_dir);
    Process start_node_no_relay(uint16_t node_port, uint16_t control_port,
                                const std::string& cache_dir);
    std::string send_command(int control_fd, const std::string& cmd);
    void wait_for_ready(int control_fd, int timeout_ms = 5000);
    void shutdown(Process& proc);
};
```

Port allocation: atomic counter starting at 15000, incrementing by 100 per test to avoid conflicts.

Startup sequence:
1. Create temp directories for each node's block cache
2. Start relay server process
3. Start node processes (with or without relay connection)
4. Connect to each node's control socket
5. Send STATUS until ready

Shutdown sequence (reverse order, best-effort):
1. Send SHUTDOWN to each node
2. Wait for processes to exit (with timeout)
3. Kill relay process
4. Clean up temp directories

## Test Scenarios

### Category A: Direct Peer-to-Peer (No Relay)

**A1. DirectSmallFileTransfer**
- Start 2 nodes with QUIC listeners (no relay)
- Node A: `PEER_ADD` Node B's address
- Node A: `STORE_FILE 100000 standard 3` (1 block, ~100KB)
- Node B: `FETCH_FILE <descriptor_hash> <file_hash> <final_byte> standard 3`
- Verify Node B's data checksum matches Node A's original

**A2. DirectLargeFileTransfer**
- Same as A1 but with ~640KB data (5 blocks for standard block size)
- Tests multi-block file through writeable_descriptor linked-list structure

**A3. DirectLateJoin**
- Start Node A with QUIC listener
- Node A: `STORE_FILE 128000 standard 3`
- Wait for upload to complete and blocks to be announced
- Start Node B, `PEER_ADD` Node A's address
- Wait for QUIC handshake and block gossip to propagate
- Node B: `FETCH_FILE <descriptor_hash> <file_hash> <final_byte> standard 3`
- Tests that Node B's readable_off_stream can find blocks via NETWORK_FIND_BLOCK

### Category B: Relay-Mediated

**B1. RelaySmallFileTransfer**
- Start relay server
- Start 2 nodes, both connected to relay
- Node A: `STORE_FILE 100000 standard 3`
- Node B: `FETCH_FILE <descriptor_hash> <file_hash> <final_byte> standard 3`
- All communication routed through relay server

**B2. RelayLargeFileTransfer**
- Same as B1 but with ~640KB data (5 blocks)

**B3. RelayLateJoin**
- Start relay server and Node A
- Node A: `STORE_FILE 128000 standard 3`
- Start Node B, connect to relay
- Wait for relay handshake and gossip
- Node B: `FETCH_FILE` via relay
- Tests that late-joining node can fetch blocks through relay

**B4. NATDetectionOpen**
- Start 2 relay servers (needed for NAT detection)
- Start node, `CONNECT_RELAY` to both
- Send STATUS, verify NAT type is reported

### Category C: Multi-Node Distribution

**C1. ThreeNodePropagation**
- Start 3 nodes (A, B, C) all connected to relay
- Node A: `STORE_FILE 256000 standard 3` (2 blocks)
- Wait for gossip to propagate blocks to Node B
- Node C: `FETCH_FILE` — should be able to fetch blocks from either A or B
- Tests that blocks propagate through the network beyond the original uploader

**C2. ThreeNodeLateJoin**
- Start relay and 2 nodes (A, B)
- Node A: `STORE_FILE 256000 standard 3`
- Wait for blocks to propagate to Node B
- Start Node C, connect to relay
- Node C: `FETCH_FILE` — fetches from B (or A) via relay
- Tests that a node joining after distribution can still retrieve all blocks

**C3. ConcurrentDownloads**
- Start relay and 3 nodes (A, B, C)
- Node A: `STORE_FILE 256000 standard 3`
- Nodes B and C simultaneously: `FETCH_FILE`
- Both should complete successfully with correct data

## File Structure

New files:

```
test/
├── test_file_transfer_integration.cpp   # Coordinator: gtest main, fixture, test cases
├── test_node_main.c                     # Node mode: full node + control socket server
└── test_control_protocol.h              # Control protocol command/response constants
```

The existing `src/Network/Relay/relay_server_main.c` is reused as the relay process.

### test_file_transfer_integration.cpp

Contains:
- `main()` — checks for `--mode` flag; without it, runs gtest coordinator
- `FileTransferIntegrationTest` fixture with process management helpers
- All test cases (Categories A, B, C)
- Helper: `send_command()`, `wait_for_ready()`, `start_relay()`, `start_node()`

### test_node_main.c

Contains:
- `main()` — parses node mode flags, runs full node initialization
- Full node setup: `scheduler_pool_create` → `config_default` → `authority_create` → `timer_actor_create` → `block_cache_create` → `network_create` → `quic_listener_create/start` → optional `network_connect_relay`
- Control socket: TCP server on `--control-port`, accepts connections, reads lines, dispatches commands
- Command handlers: `STORE_FILE` (writeable_off_stream + writeable_descriptor pipeline), `FETCH_FILE` (readable_descriptor + readable_off_stream pipeline), `PEER_ADD`, `CONNECT_RELAY`, `WAIT_FOR_PEER`, `STATUS`, `SHUTDOWN`
- Cleanup on SHUTDOWN: reverse initialization order, close control socket, exit

### test_control_protocol.h

Contains:
- `#define` constants for command strings: `CTRL_STORE_FILE`, `CTRL_FETCH_FILE`, etc.
- `#define` constants for response prefixes: `CTRL_RESP_OK`, `CTRL_RESP_HASH`, `CTRL_RESP_DATA`, `CTRL_RESP_STATUS`, `CTRL_RESP_ERROR`
- Parse helper declarations for control messages

## CMake Integration

Add to `test/CMakeLists.txt`:

```cmake
add_executable(test_file_transfer_integration
    test_file_transfer_integration.cpp
    test_node_main.c
)
target_include_directories(test_file_transfer_integration PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_file_transfer_integration PRIVATE
    offs cbor blake3 ssl crypto hashmap http-parser
    GTest::gtest_main GTest::gmock pthread)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
    target_compile_definitions(test_file_transfer_integration PRIVATE HAS_MSQUIC)
    target_link_libraries(test_file_transfer_integration PRIVATE msquic::msquic msquic::platform)
endif()
```

Each test that requires QUIC uses `GTEST_SKIP()` if the QUIC stack is unavailable (matching the existing pattern in test_quic_integration.cpp).

## QUIC Availability Gating

All tests that require real QUIC connections check availability before running:

1. Attempt to start a QUIC listener
2. If `quic_listener_start()` returns non-zero, `GTEST_SKIP()` with message "QUIC not available"
3. If successful, stop the test listener and proceed with the test

This matches the existing pattern in `test_quic_integration.cpp` Category E tests.

## Timing and Reliability

Distributed tests are inherently timing-dependent. Mitigations:

- **Polling with timeout**: Use `STATUS` polling with 5-second timeout instead of fixed sleeps
- **Generous timeouts**: Default 5s for handshakes, 10s for file transfers
- **Port isolation**: Each test gets a unique port range (100 ports apart)
- **Process cleanup**: Best-effort SIGTERM, then SIGKILL after 2s in TearDown
- **Temp directory isolation**: Each node gets its own cache directory under `/tmp/test_fft_<pid>_<index>`

## Key Invariants Tested

1. **Data integrity**: BLAKE3 checksum of decoded file matches original
2. **Block availability**: Late-joining nodes can fetch blocks from any peer that has them
3. **OFF_STREAM_AWAITING_NETWORK**: Readable off stream correctly transitions to awaiting-network state and recovers when the network delivers blocks
4. **Descriptor linked list**: Multi-block files traverse the writeable_descriptor/readable_descriptor linked structure correctly
5. **Relay path**: All data can flow through relay server when direct connection is unavailable
6. **Block propagation**: Blocks announced via NETWORK_LOCAL_STORE_BLOCK become findable by other nodes via NETWORK_FIND_BLOCK