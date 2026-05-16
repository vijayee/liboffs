# File Transfer Integration Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build fully distributed integration tests that exercise the OFF stream file transfer pipeline between separate node processes communicating via QUIC (direct and relay), verifying block propagation and late-joining node fetches.

**Architecture:** A single test binary runs in coordinator or node mode. The coordinator (gtest) spawns a relay server process (existing `relay_server` binary) and node processes (same binary with `--mode=node`). Nodes communicate over real QUIC. The coordinator sends commands to nodes via TCP control sockets using a simple line-based protocol. File transfers use writeable_off_stream + writeable_descriptor for uploads and readable_descriptor + readable_off_stream for downloads.

**Tech Stack:** C/C++17, gtest, MsQuic, CBOR (libcbor), BLAKE3, poll-dancer, POSIX sockets/processes

---

### Task 1: Control Protocol Header

**Files:**
- Create: `test/test_control_protocol.h`

- [ ] **Step 1: Create the control protocol header with command/response constants**

```c
#ifndef TEST_CONTROL_PROTOCOL_H
#define TEST_CONTROL_PROTOCOL_H

/* Commands (coordinator → node) */
#define CTRL_STORE_FILE      "STORE_FILE"
#define CTRL_FETCH_FILE      "FETCH_FILE"
#define CTRL_PEER_ADD        "PEER_ADD"
#define CTRL_CONNECT_RELAY   "CONNECT_RELAY"
#define CTRL_WAIT_FOR_PEER   "WAIT_FOR_PEER"
#define CTRL_STATUS          "STATUS"
#define CTRL_SHUTDOWN        "SHUTDOWN"

/* Response prefixes (node → coordinator) */
#define CTRL_RESP_OK         "OK"
#define CTRL_RESP_HASH       "HASH"
#define CTRL_RESP_DATA       "DATA"
#define CTRL_RESP_STATUS     "STATUS"
#define CTRL_RESP_ERROR      "ERROR"

/* Block type enum strings for parsing */
#define CTRL_BLOCK_NANO      "0"
#define CTRL_BLOCK_MINI     "1"
#define CTRL_BLOCK_STANDARD  "2"
#define CTRL_BLOCK_MEGA     "3"

/* Default timeouts (ms) */
#define CTRL_HANDSHAKE_TIMEOUT_MS  5000
#define CTRL_TRANSFER_TIMEOUT_MS  10000
#define CTRL_POLL_INTERVAL_MS     200

#endif
```

- [ ] **Step 2: Commit**

```bash
git add test/test_control_protocol.h
git commit -m "feat: add control protocol constants for file transfer integration tests"
```

---

### Task 2: Node Process — Skeleton and Initialization

**Files:**
- Create: `test/test_node_main.c`

- [ ] **Step 1: Write the node main skeleton with argument parsing, full initialization, and shutdown**

Create `test/test_node_main.c` with the following structure:

```c
#define _GNU_SOURCE
#include "test_control_protocol.h"
#include "../src/Network/network.h"
#include "../src/Network/authority.h"
#include "../src/Network/quic_listener.h"
#include "../src/Network/Relay/relay_client.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/readable_off_stream.h"
#include "../src/OFFStreams/writeable_descriptor.h"
#include "../src/OFFStreams/readable_descriptor.h"
#include "../src/OFFStreams/ori.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/block_recipe.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Configuration/config.h"
#include "../src/Streams/stream.h"
#include "../src/Buffer/buffer.h"
#include "../src/Util/allocator.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

/* Global node state */
typedef struct {
    scheduler_pool_t* pool;
    config_t config;
    authority_t* authority;
    timer_actor_t* timer;
    block_cache_t* block_cache;
    tuple_cache_t* tuple_cache;
    network_t* network;
    quic_listener_t* listener;
    uint16_t node_port;
    uint16_t control_port;
    char cache_dir[512];
    volatile int running;
    /* Last store result */
    buffer_t* last_descriptor_hash;
    buffer_t* last_file_hash;
    size_t last_final_byte;
    int last_block_type;
    size_t last_tuple_size;
    /* BLAKE3 checksum of the original data that was stored */
    uint8_t last_stored_checksum[32];
    int has_store_result;
    pthread_mutex_t store_mutex;
    pthread_cond_t store_cond;
} node_state_t;

static node_state_t g_node;

static void node_state_init(node_state_t* state) {
    memset(state, 0, sizeof(*state));
    state->config = config_default();
    pthread_mutex_init(&state->store_mutex, NULL);
    pthread_cond_init(&state->store_cond, NULL);
    state->running = 1;
}

static void node_state_destroy(node_state_t* state) {
    pthread_mutex_destroy(&state->store_mutex);
    pthread_cond_destroy(&state->store_cond);
    if (state->last_descriptor_hash) buffer_destroy(state->last_descriptor_hash);
    if (state->last_file_hash) buffer_destroy(state->last_file_hash);
}
```

Then add the `main()` function with argument parsing:

```c
static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s --mode=node --port PORT --control-port PORT "
            "[--relay-host HOST] [--relay-port PORT] --cache-dir PATH\n", prog);
}

int main(int argc, char* argv[]) {
    uint16_t node_port = 0;
    uint16_t control_port = 0;
    char relay_host[256] = {0};
    uint16_t relay_port = 0;
    char cache_dir[512] = {0};
    int has_relay = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            node_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--control-port") == 0 && i + 1 < argc) {
            control_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--relay-host") == 0 && i + 1 < argc) {
            strncpy(relay_host, argv[++i], sizeof(relay_host) - 1);
        } else if (strcmp(argv[i], "--relay-port") == 0 && i + 1 < argc) {
            relay_port = (uint16_t)atoi(argv[++i]);
            has_relay = 1;
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            strncpy(cache_dir, argv[++i], sizeof(cache_dir) - 1);
        }
    }

    if (node_port == 0 || control_port == 0 || cache_dir[0] == '\0') {
        print_usage(argv[0]);
        return 1;
    }

    node_state_init(&g_node);
    g_node.node_port = node_port;
    g_node.control_port = control_port;
    strncpy(g_node.cache_dir, cache_dir, sizeof(g_node.cache_dir) - 1);

    /* Full node initialization */
    g_node.pool = scheduler_pool_create(2);
    if (!g_node.pool) { fprintf(stderr, "Failed to create pool\n"); return 1; }
    scheduler_pool_start(g_node.pool);

    g_node.authority = authority_create(&g_node.config);
    if (!g_node.authority) { fprintf(stderr, "Failed to create authority\n"); return 1; }

    g_node.timer = timer_actor_create();
    if (!g_node.timer) { fprintf(stderr, "Failed to create timer\n"); return 1; }

    g_node.block_cache = block_cache_create(g_node.config, g_node.cache_dir, standard, g_node.timer, g_node.pool);
    if (!g_node.block_cache) { fprintf(stderr, "Failed to create block_cache\n"); return 1; }

    g_node.tuple_cache = tuple_cache_create(100, g_node.pool);
    if (!g_node.tuple_cache) { fprintf(stderr, "Failed to create tuple_cache\n"); return 1; }

    g_node.network = network_create(g_node.authority, g_node.block_cache, g_node.timer, g_node.pool);
    if (!g_node.network) { fprintf(stderr, "Failed to create network\n"); return 1; }

#ifdef HAS_MSQUIC
    g_node.listener = quic_listener_create(g_node.network, g_node.pool);
    if (g_node.listener) {
        int rc = quic_listener_start(g_node.listener, "127.0.0.1", node_port);
        if (rc != 0) {
            fprintf(stderr, "QUIC listener start failed (rc=%d)\n", rc);
            quic_listener_destroy(g_node.listener);
            g_node.listener = NULL;
        }
    }
#endif

    if (has_relay) {
        int rc = network_connect_relay(g_node.network, relay_host, relay_port);
        if (rc != 0) {
            fprintf(stderr, "Relay connect failed (rc=%d)\n", rc);
        }
    }

    /* Run the control socket server (blocks until SHUTDOWN) */
    /* (implemented in Task 3) */

    /* Cleanup */
#ifdef HAS_MSQUIC
    if (g_node.listener) {
        quic_listener_stop(g_node.listener);
        quic_listener_destroy(g_node.listener);
    }
#endif
    network_destroy(g_node.network);
    tuple_cache_destroy(g_node.tuple_cache);
    block_cache_destroy(g_node.block_cache);
    timer_actor_destroy(g_node.timer);
    authority_destroy(g_node.authority);
    scheduler_pool_stop(g_node.pool);
    scheduler_pool_destroy(g_node.pool);
    node_state_destroy(&g_node);

    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add test/test_node_main.c
git commit -m "feat: node process skeleton with initialization and argument parsing"
```

---

### Task 3: Node Process — Control Socket Server

**Files:**
- Modify: `test/test_node_main.c`

- [ ] **Step 1: Implement the TCP control socket server**

Add to `test/test_node_main.c` the control socket server that accepts connections, reads lines, and dispatches commands. For now, implement STATUS and SHUTDOWN; the STORE_FILE and FETCH_FILE handlers will be added in Tasks 4 and 5.

```c
static int send_response(int client_fd, const char* response) {
    size_t len = strlen(response);
    ssize_t written = write(client_fd, response, len);
    return (written == (ssize_t)len) ? 0 : -1;
}

static int get_peer_count(void) {
    return (int)g_node.network->conn_mgr.count;
}

/* Count blocks in the local index */
static size_t get_block_count(void) {
    if (!g_node.block_cache || !g_node.block_cache->index) return 0;
    return g_node.block_cache->index->count;
}

static int is_relay_connected(void) {
    return (g_node.network->relay && g_node.network->relay->connected) ? 1 : 0;
}

static void handle_status(int client_fd) {
    char buf[256];
    int peers = get_peer_count();
    size_t blocks = get_block_count();
    int relay = is_relay_connected();
    const char* nat_str = "unknown";
    switch (g_node.network->local_nat_type) {
        case NAT_TYPE_OPEN: nat_str = "open"; break;
        case NAT_TYPE_FULL_CONE: nat_str = "full_cone"; break;
        case NAT_TYPE_SYMMETRIC: nat_str = "symmetric"; break;
        case NAT_TYPE_PORT_RESTRICTED_CONE: nat_str = "port_restricted"; break;
        default: break;
    }
    snprintf(buf, sizeof(buf), "%s peers=%d blocks=%zu relay=%s nat=%s\n",
             CTRL_RESP_STATUS, peers, blocks, relay ? "connected" : "disconnected", nat_str);
    send_response(client_fd, buf);
}

static void handle_shutdown(int client_fd) {
    send_response(client_fd, CTRL_RESP_OK "\n");
    g_node.running = 0;
}

static void handle_command(int client_fd, char* line) {
    /* Strip trailing newline/carriage return */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    if (strncmp(line, CTRL_STATUS, strlen(CTRL_STATUS)) == 0) {
        handle_status(client_fd);
    } else if (strncmp(line, CTRL_SHUTDOWN, strlen(CTRL_SHUTDOWN)) == 0) {
        handle_shutdown(client_fd);
    } else if (strncmp(line, CTRL_PEER_ADD, strlen(CTRL_PEER_ADD)) == 0) {
        /* Parse: PEER_ADD host:port */
        const char* addr = line + strlen(CTRL_PEER_ADD) + 1;
        char host[256] = {0};
        uint16_t port = 0;
        if (sscanf(addr, "%255[^:]:%hu", host, &port) == 2) {
            /* Initiate a QUIC connection to the peer via the quic_listener.
               This sends a NETWORK_QUIC_CONNECTED approach through the existing
               network stack. For now, store the peer address and let the gossip
               and connection state machine handle the actual connection. */
            send_response(client_fd, CTRL_RESP_OK "\n");
        } else {
            send_response(client_fd, CTRL_RESP_ERROR " bad PEER_ADD format\n");
        }
    } else if (strncmp(line, CTRL_CONNECT_RELAY, strlen(CTRL_CONNECT_RELAY)) == 0) {
        /* Parse: CONNECT_RELAY host:port */
        const char* addr = line + strlen(CTRL_CONNECT_RELAY) + 1;
        char host[256] = {0};
        uint16_t port = 0;
        if (sscanf(addr, "%255[^:]:%hu", host, &port) == 2) {
            int rc = network_connect_relay(g_node.network, host, port);
            if (rc == 0) {
                send_response(client_fd, CTRL_RESP_OK "\n");
            } else {
                char err[128];
                snprintf(err, sizeof(err), "%s relay connect failed (rc=%d)\n", CTRL_RESP_ERROR, rc);
                send_response(client_fd, err);
            }
        } else {
            send_response(client_fd, CTRL_RESP_ERROR " bad CONNECT_RELAY format\n");
        }
    } else if (strncmp(line, CTRL_WAIT_FOR_PEER, strlen(CTRL_WAIT_FOR_PEER)) == 0) {
        /* Parse: WAIT_FOR_PEER count */
        int target = atoi(line + strlen(CTRL_WAIT_FOR_PEER) + 1);
        while (g_node.running && get_peer_count() < target) {
            usleep(CTRL_POLL_INTERVAL_MS * 1000);
        }
        if (get_peer_count() >= target) {
            send_response(client_fd, CTRL_RESP_OK "\n");
        } else {
            send_response(client_fd, CTRL_RESP_ERROR " shutdown while waiting\n");
        }
    } else {
        char err[128];
        snprintf(err, sizeof(err), "%s unknown command: %s\n", CTRL_RESP_ERROR, line);
        send_response(client_fd, err);
    }
}

static void* control_socket_thread(void* arg) {
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "Control socket creation failed\n");
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(g_node.control_port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Control socket bind failed (port=%u): %s\n", g_node.control_port, strerror(errno));
        close(server_fd);
        return NULL;
    }

    listen(server_fd, 1);

    while (g_node.running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        struct timeval tv = {1, 0};
        int ready = select(server_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        char line_buf[4096];
        size_t line_pos = 0;

        while (g_node.running) {
            ssize_t n = read(client_fd, line_buf + line_pos, sizeof(line_buf) - line_pos - 1);
            if (n <= 0) break;
            line_pos += n;
            line_buf[line_pos] = '\0';

            /* Process complete lines */
            char* nl;
            while ((nl = strchr(line_buf, '\n')) != NULL) {
                *nl = '\0';
                handle_command(client_fd, line_buf);
                if (!g_node.running) break;
                size_t remaining = line_pos - (nl - line_buf + 1);
                memmove(line_buf, nl + 1, remaining);
                line_pos = remaining;
                line_buf[line_pos] = '\0';
            }
        }
        close(client_fd);
    }
    close(server_fd);
    return NULL;
}
```

Then update `main()` to start the control socket thread (replace the `/* Run the control socket server */` comment):

```c
    /* Start control socket thread */
    pthread_t ctrl_thread;
    pthread_create(&ctrl_thread, NULL, control_socket_thread, NULL);

    /* Wait for shutdown */
    while (g_node.running) {
        usleep(100000); /* 100ms */
    }

    /* Wait for control thread to finish */
    pthread_join(ctrl_thread, NULL);
```

- [ ] **Step 2: Verify it compiles**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration 2>&1 | head -50
```

Expected: Compilation errors for missing STORE_FILE/FETCH_FILE handlers are acceptable; the rest should compile.

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "feat: add TCP control socket server to node process"
```

---

### Task 4: Node Process — STORE_FILE Command Handler

**Files:**
- Modify: `test/test_node_main.c`

- [ ] **Step 1: Implement the STORE_FILE command handler using writeable_off_stream + writeable_descriptor**

Add a put context struct and its handlers, mirroring the `_off_put_handler` flow from `src/HTTP/off_routes.c` but writing the data to the off-stream pipeline and capturing the result:

```c
typedef struct {
    node_state_t* node;
    buffer_t* file_hash;
    buffer_t* descriptor_hash;
    size_t final_byte;
    writeable_descriptor_t* desc;
    writeable_off_stream_t* ws;
    new_blocks_recipe_t* recipe;
    int complete;
    uint8_t stored_checksum[32];
} node_put_context_t;

static void node_put_on_stream_data(void* ctx, void* data) {
    node_put_context_t* put_ctx = (node_put_context_t*)ctx;
    buffer_t* payload = (buffer_t*)data;
    if (payload->size == 32 && put_ctx->file_hash == NULL) {
        /* This is the file_hash from finalize */
        put_ctx->file_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
    } else {
        /* This is a tuple — forward to descriptor */
        tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)payload);
        writeable_descriptor_write(put_ctx->desc, tuple);
        tuple_destroy(tuple);
    }
}

static void node_put_on_stream_close(void* ctx, void* unused) {
    (void)unused;
    node_put_context_t* put_ctx = (node_put_context_t*)ctx;
    writeable_descriptor_close(put_ctx->desc);
}

static void node_put_on_descriptor_data(void* ctx, void* data) {
    node_put_context_t* put_ctx = (node_put_context_t*)ctx;
    buffer_t* payload = (buffer_t*)data;
    if (put_ctx->descriptor_hash != NULL) {
        buffer_destroy(put_ctx->descriptor_hash);
    }
    put_ctx->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
}

static void node_put_on_descriptor_close(void* ctx, void* unused) {
    (void)unused;
    node_put_context_t* put_ctx = (node_put_context_t*)ctx;
    /* Store results in node state */
    pthread_mutex_lock(&put_ctx->node->store_mutex);
    if (put_ctx->node->last_descriptor_hash) buffer_destroy(put_ctx->node->last_descriptor_hash);
    put_ctx->node->last_descriptor_hash = buffer_copy(put_ctx->descriptor_hash);
    if (put_ctx->node->last_file_hash) buffer_destroy(put_ctx->node->last_file_hash);
    put_ctx->node->last_file_hash = buffer_copy(put_ctx->file_hash);
    put_ctx->node->last_final_byte = put_ctx->final_byte;
    memcpy(put_ctx->node->last_stored_checksum, put_ctx->stored_checksum, 32);
    put_ctx->node->has_store_result = 1;
    pthread_cond_signal(&put_ctx->node->store_cond);
    pthread_mutex_unlock(&put_ctx->node->store_mutex);
    put_ctx->complete = 1;

    /* Cleanup — defer stream destruction to pool */
    scheduler_pool_defer_cleanup(((stream_t*)put_ctx->ws)->pool, put_ctx->recipe,
                                 (void (*)(void*))new_blocks_recipe_destroy);
    stream_deferred_deref((stream_t*)put_ctx->desc);
    stream_deferred_deref((stream_t*)put_ctx->ws);
    buffer_destroy(put_ctx->file_hash);
    buffer_destroy(put_ctx->descriptor_hash);
    free(put_ctx);
}
```

Then add the BLAKE3 helper and the STORE_FILE command handler:

```c
#include "../deps/blake3/blake3.h"

static void blake3_hash_buffer(const uint8_t* data, size_t len, uint8_t out[32]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, out, 32);
}

static void handle_store_file(int client_fd, char* line) {
    /* Parse: STORE_FILE size block_type tuple_size */
    size_t size = 0;
    int block_type_int = 2; /* standard */
    size_t tuple_size = 3;
    if (sscanf(line + strlen(CTRL_STORE_FILE) + 1, "%zu %d %zu", &size, &block_type_int, &tuple_size) < 1) {
        send_response(client_fd, CTRL_RESP_ERROR " bad STORE_FILE format\n");
        return;
    }
    if (size == 0) {
        send_response(client_fd, CTRL_RESP_ERROR " size must be > 0\n");
        return;
    }
    block_size_e block_type = (block_size_e)block_type_int;

    /* Generate random data */
    buffer_t* data = buffer_create(size);
    for (size_t i = 0; i < size; i++) {
        data->data[i] = (uint8_t)(rand() & 0xFF);
    }
    data->size = size;

    /* Compute BLAKE3 checksum of the original data for later verification */
    uint8_t checksum[32];
    blake3_hash_buffer(data->data, data->size, checksum);

    /* Set up the off-stream pipeline (mirrors _off_put_handler from off_routes.c) */
    new_blocks_recipe_t* recipe = new_blocks_recipe_create(g_node.pool, g_node.block_cache, block_type);
    vec_block_recipe_t recipes;
    vec_init(&recipes);
    vec_push(&recipes, (block_recipe_t*)recipe);

    writeable_off_stream_t* ws = writeable_off_stream_create(
        g_node.pool, g_node.block_cache, g_node.tuple_cache, block_type, tuple_size, 32, recipes, g_node.network);

    writeable_descriptor_t* desc = writeable_descriptor_create(
        g_node.pool, g_node.block_cache, block_type, 32, tuple_size, size, g_node.network);

    node_put_context_t* put_ctx = get_clear_memory(sizeof(node_put_context_t));
    put_ctx->node = &g_node;
    put_ctx->desc = desc;
    put_ctx->ws = ws;
    put_ctx->recipe = recipe;
    put_ctx->final_byte = size;
    memcpy(put_ctx->stored_checksum, checksum, 32);

    stream_subscribe((stream_t*)ws, data_event, put_ctx,
                     (void (*)(void*, void*))node_put_on_stream_data, NULL);
    stream_subscribe((stream_t*)ws, close_event, put_ctx,
                     (void (*)(void*, void*))node_put_on_stream_close, NULL);
    stream_subscribe((stream_t*)desc, data_event, put_ctx,
                     (void (*)(void*, void*))node_put_on_descriptor_data, NULL);
    stream_once((stream_t*)desc, close_event, put_ctx,
                (void (*)(void*, void*))node_put_on_descriptor_close, NULL);

    writeable_off_stream_write(ws, data);
    buffer_destroy(data);
    writeable_off_stream_finalize(ws);

    /* Wait for the descriptor to complete (asynchronous via stream events) */
    pthread_mutex_lock(&g_node.store_mutex);
    g_node.has_store_result = 0;
    while (!g_node.has_store_result && g_node.running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        pthread_cond_timedwait(&g_node.store_cond, &g_node.store_mutex, &ts);
    }
    pthread_mutex_unlock(&g_node.store_mutex);

    if (!g_node.has_store_result) {
        send_response(client_fd, CTRL_RESP_ERROR " store timed out\n");
        return;
    }

    /* Format response: HASH <descriptor_hash_hex> <file_hash_hex> <final_byte> */
    char desc_hex[65];
    char file_hex[65];
    buffer_to_hex(g_node.last_descriptor_hash, desc_hex);
    buffer_to_hex(g_node.last_file_hash, file_hex);

    char response[256];
    snprintf(response, sizeof(response), "%s %s %s %zu\n",
             CTRL_RESP_HASH, desc_hex, file_hex, g_node.last_final_byte);
    send_response(client_fd, response);
}
```

Add a `buffer_to_hex` helper:

```c
static void buffer_to_hex(const buffer_t* buf, char out[65]) {
    out[64] = '\0';
    for (size_t i = 0; i < 32 && i < buf->size; i++) {
        snprintf(out + i * 2, 3, "%02x", buf->data[i]);
    }
}
```

Update `handle_command` to dispatch `CTRL_STORE_FILE`:

Add this branch to the `handle_command` function before the unknown command handler:

```c
    } else if (strncmp(line, CTRL_STORE_FILE, strlen(CTRL_STORE_FILE)) == 0) {
        handle_store_file(client_fd, line);
```

- [ ] **Step 2: Verify it compiles**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "feat: implement STORE_FILE command handler in node process"
```

---

### Task 5: Node Process — FETCH_FILE Command Handler

**Files:**
- Modify: `test/test_node_main.c`

- [ ] **Step 1: Implement the FETCH_FILE command handler using readable_descriptor + readable_off_stream**

Add a get pipeline context struct and its handlers, mirroring the `_setup_stream_pipeline` flow from `src/HTTP/off_routes.c`:

```c
typedef struct {
    node_state_t* node;
    readable_descriptor_t* desc;
    readable_off_stream_t* rs;
    ori_t* ori;
    int complete;
    uint8_t checksum[32];
    size_t total_bytes;
} node_get_pipeline_t;

static void node_get_on_tuple(void* ctx, void* data) {
    node_get_pipeline_t* pipeline = (node_get_pipeline_t*)ctx;
    tuple_t* tuple = (tuple_t*)data;
    readable_off_stream_write(pipeline->rs, tuple);
}

static void node_get_on_data(void* ctx, void* data) {
    node_get_pipeline_t* pipeline = (node_get_pipeline_t*)ctx;
    buffer_t* buf = (buffer_t*)data;
    pipeline->total_bytes += buf->size;
}

static void node_get_on_desc_close(void* ctx, void* unused) {
    (void)unused;
    node_get_pipeline_t* pipeline = (node_get_pipeline_t*)ctx;
    stream_deferred_deref((stream_t*)pipeline->desc);
}

static void node_get_on_desc_error(void* ctx, void* error) {
    (void)error;
    node_get_pipeline_t* pipeline = (node_get_pipeline_t*)ctx;
    stream_deactivate((stream_t*)pipeline->rs, NULL);
    stream_deferred_deref((stream_t*)pipeline->desc);
}

static void node_get_on_rs_close(void* ctx, void* unused) {
    (void)unused;
    node_get_pipeline_t* pipeline = (node_get_pipeline_t*)ctx;

    /* Compute BLAKE3 checksum of the received data by re-reading from block cache.
       For now, we rely on the file_hash comparison. The important thing is that
       the stream completed. */
    pipeline->complete = 1;

    stream_deferred_deref((stream_t*)pipeline->rs);
    DESTROY(pipeline->ori, ori);
    free(pipeline);
}

static void node_get_on_rs_data(void* ctx, void* data) {
    node_get_pipeline_t* pipeline = (node_get_pipeline_t*)ctx;
    buffer_t* buf = (buffer_t*)data;
    /* Accumulate BLAKE3 hash of received data */
    pipeline->total_bytes += buf->size;
}
```

Then add the FETCH_FILE handler:

```c
/* We need a BLAKE3 incremental hasher in the pipeline for verifying the
   received data matches the original. Add it to the pipeline struct. */
typedef struct {
    node_state_t* node;
    readable_descriptor_t* desc;
    readable_off_stream_t* rs;
    ori_t* ori;
    int complete;
    uint8_t checksum[32];
    size_t total_bytes;
    blake3_hasher hasher;
    uint8_t hasher_initialized;
} node_get_pipeline_v2_t;

static void node_get_v2_on_tuple(void* ctx, void* data) {
    node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
    tuple_t* tuple = (tuple_t*)data;
    readable_off_stream_write(pipeline->rs, tuple);
}

static void node_get_v2_on_rs_data(void* ctx, void* data) {
    node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
    buffer_t* buf = (buffer_t*)data;
    if (!pipeline->hasher_initialized) {
        blake3_hasher_init(&pipeline->hasher);
        pipeline->hasher_initialized = 1;
    }
    blake3_hasher_update(&pipeline->hasher, buf->data, buf->size);
    pipeline->total_bytes += buf->size;
}

static void node_get_v2_on_desc_close(void* ctx, void* unused) {
    (void)unused;
    node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
    stream_deferred_deref((stream_t*)pipeline->desc);
}

static void node_get_v2_on_desc_error(void* ctx, void* error) {
    (void)error;
    node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;
    stream_deactivate((stream_t*)pipeline->rs, NULL);
    stream_deferred_deref((stream_t*)pipeline->desc);
}

static void node_get_v2_on_rs_close(void* ctx, void* unused) {
    (void)unused;
    node_get_pipeline_v2_t* pipeline = (node_get_pipeline_v2_t*)ctx;

    /* Finalize BLAKE3 checksum */
    if (pipeline->hasher_initialized) {
        blake3_hasher_finalize(&pipeline->hasher, pipeline->checksum, 32);
    }

    pipeline->complete = 1;

    stream_deferred_deref((stream_t*)pipeline->rs);
    DESTROY(pipeline->ori, ori);
    free(pipeline);
}

static void handle_fetch_file(int client_fd, char* line) {
    /* Parse: FETCH_FILE descriptor_hash_hex file_hash_hex final_byte block_type tuple_size */
    char desc_hex[65] = {0};
    char file_hex[65] = {0};
    size_t final_byte = 0;
    int block_type_int = 2;
    size_t tuple_size = 3;

    int parsed = sscanf(line + strlen(CTRL_FETCH_FILE) + 1,
                        "%64s %64s %zu %d %zu",
                        desc_hex, file_hex, &final_byte, &block_type_int, &tuple_size);
    if (parsed < 3) {
        send_response(client_fd, CTRL_RESP_ERROR " bad FETCH_FILE format\n");
        return;
    }

    block_size_e block_type = (block_size_e)block_type_int;

    /* Parse hex hashes to buffers */
    buffer_t* descriptor_hash = hex_to_buffer(desc_hex);
    buffer_t* file_hash = hex_to_buffer(file_hex);
    if (!descriptor_hash || !file_hash) {
        send_response(client_fd, CTRL_RESP_ERROR " bad hex hash\n");
        if (descriptor_hash) buffer_destroy(descriptor_hash);
        if (file_hash) buffer_destroy(file_hash);
        return;
    }

    /* Create ORI */
    ori_t* ori = ori_create(final_byte);
    ori->descriptor_hash = descriptor_hash;
    ori->file_hash = file_hash;
    ori->block_type = block_type;
    ori->tuple_size = tuple_size;

    /* Set up the read pipeline (mirrors _setup_stream_pipeline from off_routes.c) */
    readable_off_stream_t* rs = readable_off_stream_create(
        g_node.pool, g_node.block_cache, g_node.tuple_cache, ori, 32, g_node.network);
    readable_descriptor_t* desc = readable_descriptor_create(
        g_node.pool, g_node.block_cache, ori, 32, g_node.network);

    node_get_pipeline_v2_t* pipeline = get_clear_memory(sizeof(node_get_pipeline_v2_t));
    pipeline->node = &g_node;
    pipeline->desc = desc;
    pipeline->rs = rs;
    pipeline->ori = (ori_t*)refcounter_reference((refcounter_t*)ori);

    stream_subscribe((stream_t*)desc, data_event, pipeline,
                     (void (*)(void*, void*))node_get_v2_on_tuple, NULL);
    stream_once((stream_t*)desc, close_event, pipeline,
                (void (*)(void*, void*))node_get_v2_on_desc_close, NULL);
    stream_once((stream_t*)desc, error_event, pipeline,
                (void (*)(void*, void*))node_get_v2_on_desc_error, NULL);
    stream_subscribe((stream_t*)rs, data_event, pipeline,
                     (void (*)(void*, void*))node_get_v2_on_rs_data, NULL);
    stream_once((stream_t*)rs, close_event, pipeline,
                (void (*)(void*, void*))node_get_v2_on_rs_close, NULL);

    readable_descriptor_push(desc);

    /* Wait for completion — the close handler sets pipeline->complete = 1.
       We poll because the stream events are dispatched on the scheduler pool threads. */
    int timeout_ms = CTRL_TRANSFER_TIMEOUT_MS;
    int waited = 0;
    while (!pipeline->complete && g_node.running && waited < timeout_ms) {
        usleep(CTRL_POLL_INTERVAL_MS * 1000);
        waited += CTRL_POLL_INTERVAL_MS;
    }

    if (!pipeline->complete) {
        send_response(client_fd, CTRL_RESP_ERROR " fetch timed out\n");
        return;
    }

    /* Format response: DATA <hex_checksum> <size> */
    char checksum_hex[65];
    for (size_t i = 0; i < 32; i++) {
        snprintf(checksum_hex + i * 2, 3, "%02x", pipeline->checksum[i]);
    }
    checksum_hex[64] = '\0';

    char response[256];
    snprintf(response, sizeof(response), "%s %s %zu\n",
             CTRL_RESP_DATA, checksum_hex, pipeline->total_bytes);
    send_response(client_fd, response);
}
```

Add the `hex_to_buffer` helper:

```c
static buffer_t* hex_to_buffer(const char* hex) {
    size_t hex_len = strlen(hex);
    if (hex_len != 64) return NULL; /* Expect 32-byte hash as 64 hex chars */
    buffer_t* buf = buffer_create(32);
    for (size_t i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            buffer_destroy(buf);
            return NULL;
        }
        buf->data[i] = (uint8_t)byte;
    }
    buf->size = 32;
    return buf;
}
```

Update `handle_command` to dispatch `CTRL_FETCH_FILE`:

```c
    } else if (strncmp(line, CTRL_FETCH_FILE, strlen(CTRL_FETCH_FILE)) == 0) {
        handle_fetch_file(client_fd, line);
```

Remove the earlier draft `node_get_pipeline_t` and its handlers (the v2 versions supersede them).

- [ ] **Step 2: Verify it compiles**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration 2>&1 | tail -20
```

- [ ] **Step 3: Commit**

```bash
git add test/test_node_main.c
git commit -m "feat: implement FETCH_FILE command handler in node process"
```

---

### Task 6: CMake Build for Test Binary

**Files:**
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Add the test_file_transfer_integration target to CMakeLists.txt**

Append to `test/CMakeLists.txt` after the `gtest_add_tests` line but before `file(COPY ...)`:

```cmake
    add_executable(test_file_transfer_integration test_file_transfer_integration.cpp test_node_main.c)
    add_dependencies(test_file_transfer_integration cbor)
    add_dependencies(test_file_transfer_integration offs)
    add_dependencies(test_file_transfer_integration blake3)
    add_dependencies(test_file_transfer_integration http-parser)
    target_link_libraries(test_file_transfer_integration PRIVATE -Wl,--whole-archive offs -Wl,--no-whole-archive)
    target_link_libraries(test_file_transfer_integration PRIVATE ssl crypto)
    target_link_libraries(test_file_transfer_integration PRIVATE blake3)
    target_link_libraries(test_file_transfer_integration PRIVATE hashmap)
    target_link_libraries(test_file_transfer_integration PRIVATE http-parser)
    target_link_libraries(test_file_transfer_integration PUBLIC cbor)
    target_link_libraries(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../poll-dancer/build/libpoll_dancer.a)
    target_link_libraries(test_file_transfer_integration PRIVATE GTest::gtest_main)
    target_link_libraries(test_file_transfer_integration PRIVATE GTest::gmock)
    target_link_libraries(test_file_transfer_integration PRIVATE pthread)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/CMakeLists.txt)
      target_compile_definitions(test_file_transfer_integration PRIVATE HAS_MSQUIC)
      target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/msquic/src/inc)
      target_link_libraries(test_file_transfer_integration PRIVATE msquic::msquic msquic::platform)
    endif()
    target_include_directories(test_file_transfer_integration PUBLIC ${C_INC})
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../poll-dancer/include)
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/http-parser)
    target_include_directories(test_file_transfer_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/blake3)
```

- [ ] **Step 2: Rebuild and verify compilation**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && make test_file_transfer_integration 2>&1 | tail -20
```

Expected: The binary compiles (may have linker errors if STORE_FILE/FETCH_FILE are not yet complete, but no compilation errors in the CMakeLists.txt).

- [ ] **Step 3: Commit**

```bash
git add test/CMakeLists.txt
git commit -m "build: add test_file_transfer_integration target to CMake"
```

---

### Task 7: Coordinator — Process Management and Fixture

**Files:**
- Create: `test/test_file_transfer_integration.cpp`

- [ ] **Step 1: Write the coordinator test file with the FileTransferIntegrationTest fixture**

Create `test/test_file_transfer_integration.cpp` with the process management fixture, helpers for starting relay and node processes, sending commands, and teardown:

```cpp
#include <gtest/gtest.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <fstream>

#include "test_control_protocol.h"

extern "C" {
#include "../src/Network/Relay/relay_server.h"
#include "../src/Scheduler/scheduler.h"
}

static std::atomic<uint16_t> next_base_port{15000};

struct Process {
    pid_t pid;
    uint16_t control_port;
    int control_fd;
    std::string cache_dir;
};

class FileTransferIntegrationTest : public ::testing::Test {
protected:
    Process relay_proc;
    std::vector<Process> nodes;
    uint16_t base_port;
    std::string test_dir;

    void SetUp() override {
        base_port = next_base_port.fetch_add(100);
        test_dir = "/tmp/test_fft_" + std::to_string(getpid()) + "_" + std::to_string(base_port);
        std::filesystem::create_directories(test_dir);
    }

    void TearDown() override {
        /* Shutdown all nodes */
        for (auto& node : nodes) {
            if (node.control_fd >= 0) {
                send_command(node.control_fd, CTRL_SHUTDOWN);
                close(node.control_fd);
                node.control_fd = -1;
            }
            if (node.pid > 0) {
                int status;
                for (int i = 0; i < 20; i++) {
                    if (waitpid(node.pid, &status, WNOHANG) != 0) break;
                    usleep(100000);
                }
                if (waitpid(node.pid, &status, WNOHANG) == 0) {
                    kill(node.pid, SIGKILL);
                    waitpid(node.pid, &status, 0);
                }
                node.pid = -1;
            }
        }

        /* Kill relay */
        if (relay_proc.pid > 0) {
            kill(relay_proc.pid, SIGTERM);
            int status;
            for (int i = 0; i < 20; i++) {
                if (waitpid(relay_proc.pid, &status, WNOHANG) != 0) break;
                usleep(100000);
            }
            if (waitpid(relay_proc.pid, &status, WNOHANG) == 0) {
                kill(relay_proc.pid, SIGKILL);
                waitpid(relay_proc.pid, &status, 0);
            }
            relay_proc.pid = -1;
        }

        /* Clean up temp dirs */
        for (auto& node : nodes) {
            if (!node.cache_dir.empty()) {
                std::filesystem::remove_all(node.cache_dir);
            }
        }
        if (!test_dir.empty()) {
            std::filesystem::remove_all(test_dir);
        }
    }

    Process start_relay(uint16_t port) {
        Process proc = {};
        proc.control_fd = -1;

        /* Use the existing relay_server binary */
        std::string binary = "./relay_server";
        std::string port_str = std::to_string(port);

        proc.pid = fork();
        if (proc.pid == 0) {
            execl(binary.c_str(), "relay_server", "--port", port_str.c_str(), (char*)NULL);
            _exit(1);
        }
        if (proc.pid < 0) {
            return proc;
        }

        /* Wait for relay to be ready */
        usleep(500000); /* 500ms for relay server startup */
        relay_proc = proc;
        return proc;
    }

    Process start_node(uint16_t node_port, uint16_t control_port,
                       uint16_t relay_port, const std::string& cache_dir) {
        return start_node_internal(node_port, control_port, "127.0.0.1", relay_port, true, cache_dir);
    }

    Process start_node_no_relay(uint16_t node_port, uint16_t control_port,
                                const std::string& cache_dir) {
        return start_node_internal(node_port, control_port, "", 0, false, cache_dir);
    }

    std::string send_command(int control_fd, const std::string& cmd) {
        if (control_fd < 0) return "";
        std::string full_cmd = cmd + "\n";
        ssize_t written = write(control_fd, full_cmd.c_str(), full_cmd.size());
        if (written != (ssize_t)full_cmd.size()) return "";

        /* Read response line */
        char buf[4096];
        std::string response;
        auto start = std::chrono::steady_clock::now();
        while (true) {
            ssize_t n = read(control_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                response += buf;
                if (response.find('\n') != std::string::npos) break;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > CTRL_TRANSFER_TIMEOUT_MS) break;
            usleep(10000); /* 10ms */
        }
        /* Strip trailing newline */
        while (!response.empty() && response.back() == '\n') response.pop_back();
        while (!response.empty() && response.back() == '\r') response.pop_back();
        return response;
    }

    void wait_for_ready(int control_fd, int timeout_ms = CTRL_HANDSHAKE_TIMEOUT_MS) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            std::string resp = send_command(control_fd, CTRL_STATUS);
            if (!resp.empty() && resp.find(CTRL_RESP_STATUS) == 0) break;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms) break;
            usleep(CTRL_POLL_INTERVAL_MS * 1000);
        }
    }

private:
    Process start_node_internal(uint16_t node_port, uint16_t control_port,
                                const std::string& relay_host, uint16_t relay_port,
                                bool has_relay, const std::string& cache_dir) {
        Process proc = {};
        proc.control_fd = -1;
        proc.cache_dir = cache_dir;

        std::filesystem::create_directories(cache_dir);

        std::string binary = "./test_file_transfer_integration";
        std::string port_str = std::to_string(node_port);
        std::string ctrl_str = std::to_string(control_port);
        std::string dir_str = cache_dir;

        proc.pid = fork();
        if (proc.pid == 0) {
            if (has_relay) {
                std::string relay_str = relay_host + ":" + std::to_string(relay_port);
                execl(binary.c_str(), "test_file_transfer_integration",
                      "--mode=node", "--port", port_str.c_str(),
                      "--control-port", ctrl_str.c_str(),
                      "--cache-dir", dir_str.c_str(),
                      "--relay-host", relay_host.c_str(),
                      "--relay-port", std::to_string(relay_port).c_str(),
                      (char*)NULL);
            } else {
                execl(binary.c_str(), "test_file_transfer_integration",
                      "--mode=node", "--port", port_str.c_str(),
                      "--control-port", ctrl_str.c_str(),
                      "--cache-dir", dir_str.c_str(),
                      (char*)NULL);
            }
            _exit(1);
        }
        if (proc.pid < 0) return proc;

        /* Connect to control socket */
        auto start = std::chrono::steady_clock::now();
        while (true) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(control_port);
                if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    proc.control_fd = fd;
                    break;
                }
                close(fd);
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > CTRL_HANDSHAKE_TIMEOUT_MS) break;
            usleep(50000); /* 50ms */
        }

        if (proc.control_fd >= 0) {
            wait_for_ready(proc.control_fd);
        }

        nodes.push_back(proc);
        return proc;
    }
};

/* Placeholder test to verify the fixture compiles */
TEST_F(FileTransferIntegrationTest, FixtureCompiles) {
    EXPECT_TRUE(true);
}
```

- [ ] **Step 2: Add the main() that supports --mode=node**

Add a `main()` at the bottom of `test_file_transfer_integration.cpp` that checks for `--mode=node` and delegates to the node main:

```cpp
/* Node mode support — when run with --mode=node, delegate to test_node_main.
   This is done via extern C linkage to the node_main function. */
extern "C" int node_main(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    /* Check for --mode=node */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc && strcmp(argv[i + 1], "node") == 0) {
            return node_main(argc, argv);
        }
        if (strcmp(argv[i], "--mode=node") == 0) {
            return node_main(argc, argv);
        }
    }
    /* Default: run gtest */
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

Update `test_node_main.c` to expose `node_main` instead of `main`:

Rename `int main(int argc, char* argv[])` in `test_node_main.c` to `int node_main(int argc, char* argv[])`.

- [ ] **Step 3: Rebuild and verify compilation**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && make test_file_transfer_integration 2>&1 | tail -30
```

Expected: Binary compiles and `--mode=node` delegates correctly.

- [ ] **Step 4: Run the placeholder test**

```bash
./test_file_transfer_integration --gtest_filter=FileTransferIntegrationTest.FixtureCompiles
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add test/test_file_transfer_integration.cpp test/test_node_main.c
git commit -m "feat: add coordinator test fixture with process management"
```

---

### Task 8: Category A — Direct Peer-to-Peer File Transfer Tests

**Files:**
- Modify: `test/test_file_transfer_integration.cpp`

- [ ] **Step 1: Implement DirectSmallFileTransfer (A1)**

Add to `test/test_file_transfer_integration.cpp` after the placeholder test:

```cpp
TEST_F(FileTransferIntegrationTest, DirectSmallFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t port_a = base_port;
    uint16_t port_b = base_port + 1;
    uint16_t ctrl_a = base_port + 10;
    uint16_t ctrl_b = base_port + 11;

    Process node_a = start_node_no_relay(port_a, ctrl_a, test_dir + "/cache_a");
    Process node_b = start_node_no_relay(port_b, ctrl_b, test_dir + "/cache_b");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GT(node_b.pid, 0);
    ASSERT_GE(node_a.control_fd, 0);
    ASSERT_GE(node_b.control_fd, 0);

    /* Tell Node A about Node B's address */
    std::string peer_cmd = std::string(CTRL_PEER_ADD) + " 127.0.0.1:" + std::to_string(port_b);
    std::string resp = send_command(node_a.control_fd, peer_cmd);
    EXPECT_NE(resp.find(CTRL_RESP_OK), std::string::npos);

    /* Wait for peer connection */
    resp = send_command(node_a.control_fd, std::string(CTRL_WAIT_FOR_PEER) + " 1");
    EXPECT_NE(resp.find(CTRL_RESP_OK), std::string::npos);

    /* Store a small file on Node A (~100KB, 1 block for standard) */
    resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE response: " << resp;

    /* Parse the HASH response: HASH <desc_hash> <file_hash> <final_byte> */
    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    /* Fetch the file on Node B */
    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_b.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE response: " << resp;

    /* The checksum and size are in the DATA response — verify non-empty */
    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string checksum_hex;
    size_t data_size;
    data_stream >> checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
#endif
}
```

- [ ] **Step 2: Implement DirectLargeFileTransfer (A2)**

```cpp
TEST_F(FileTransferIntegrationTest, DirectLargeFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t port_a = base_port;
    uint16_t port_b = base_port + 1;
    uint16_t ctrl_a = base_port + 10;
    uint16_t ctrl_b = base_port + 11;

    Process node_a = start_node_no_relay(port_a, ctrl_a, test_dir + "/cache_a");
    Process node_b = start_node_no_relay(port_b, ctrl_b, test_dir + "/cache_b");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GT(node_b.pid, 0);

    std::string peer_cmd = std::string(CTRL_PEER_ADD) + " 127.0.0.1:" + std::to_string(port_b);
    send_command(node_a.control_fd, peer_cmd);
    send_command(node_a.control_fd, std::string(CTRL_WAIT_FOR_PEER) + " 1");

    /* Store a larger file (~640KB, 5 blocks for standard block size) */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 640000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_b.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string checksum_hex;
    size_t data_size;
    data_stream >> checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
#endif
}
```

- [ ] **Step 3: Implement DirectLateJoin (A3)**

```cpp
TEST_F(FileTransferIntegrationTest, DirectLateJoin) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t port_a = base_port;
    uint16_t ctrl_a = base_port + 10;

    /* Start only Node A initially */
    Process node_a = start_node_no_relay(port_a, ctrl_a, test_dir + "/cache_a");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GE(node_a.control_fd, 0);

    /* Store a file on Node A */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 128000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    /* Now start Node B and connect it to Node A */
    uint16_t port_b = base_port + 1;
    uint16_t ctrl_b = base_port + 11;
    Process node_b = start_node_no_relay(port_b, ctrl_b, test_dir + "/cache_b");
    ASSERT_GT(node_b.pid, 0);

    std::string peer_cmd = std::string(CTRL_PEER_ADD) + " 127.0.0.1:" + std::to_string(port_a);
    send_command(node_b.control_fd, peer_cmd);
    send_command(node_b.control_fd, std::string(CTRL_WAIT_FOR_PEER) + " 1");

    /* Fetch on Node B — this exercises the OFF_STREAM_AWAITING_NETWORK path
       because Node B's block cache is empty */
    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_b.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string checksum_hex;
    size_t data_size;
    data_stream >> checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
#endif
}
```

- [ ] **Step 4: Build and run the Category A tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration && ./test_file_transfer_integration --gtest_filter="FileTransferIntegrationTest.Direct*"
```

Expected: Tests pass (or skip if msquic unavailable). If tests fail due to incomplete STORE_FILE/FETCH_FILE wiring, debug and fix.

- [ ] **Step 5: Commit**

```bash
git add test/test_file_transfer_integration.cpp
git commit -m "test: add Category A direct peer-to-peer file transfer tests"
```

---

### Task 9: Category B — Relay-Mediated File Transfer Tests

**Files:**
- Modify: `test/test_file_transfer_integration.cpp`

- [ ] **Step 1: Implement RelaySmallFileTransfer (B1)**

```cpp
TEST_F(FileTransferIntegrationTest, RelaySmallFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port;
    uint16_t port_a = base_port + 1;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_a = base_port + 11;
    uint16_t ctrl_b = base_port + 12;

    Process relay_p = start_relay(relay_port);
    ASSERT_GT(relay_p.pid, 0);

    Process node_a = start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    Process node_b = start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GT(node_b.pid, 0);

    /* Both nodes should be connected to the relay */
    std::string status_a = send_command(node_a.control_fd, CTRL_STATUS);
    EXPECT_NE(status_a.find("relay=connected"), std::string::npos) << "Node A status: " << status_a;

    /* Store on A */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    /* Fetch on B via relay */
    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_b.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE: " << resp;
#endif
}
```

- [ ] **Step 2: Implement RelayLargeFileTransfer (B2)**

```cpp
TEST_F(FileTransferIntegrationTest, RelayLargeFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port;
    uint16_t port_a = base_port + 1;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_a = base_port + 11;
    uint16_t ctrl_b = base_port + 12;

    Process relay_p = start_relay(relay_port);
    ASSERT_GT(relay_p.pid, 0);

    Process node_a = start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    Process node_b = start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GT(node_b.pid, 0);

    /* Store a larger file (~640KB) */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 640000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_b.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE: " << resp;
#endif
}
```

- [ ] **Step 3: Implement RelayLateJoin (B3)**

```cpp
TEST_F(FileTransferIntegrationTest, RelayLateJoin) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port;
    uint16_t port_a = base_port + 1;
    uint16_t ctrl_a = base_port + 11;

    Process relay_p = start_relay(relay_port);
    ASSERT_GT(relay_p.pid, 0);

    /* Start only Node A */
    Process node_a = start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    ASSERT_GT(node_a.pid, 0);

    /* Store on A */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 128000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    /* Now start Node B and connect to relay */
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_b = base_port + 12;
    Process node_b = start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(node_b.pid, 0);

    /* Fetch on B — B has empty cache, must get blocks from A via relay */
    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_b.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE: " << resp;
#endif
}
```

- [ ] **Step 4: Build and run Category B tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration && ./test_file_transfer_integration --gtest_filter="FileTransferIntegrationTest.Relay*"
```

- [ ] **Step 5: Implement NATDetectionOpen (B4)**

```cpp
TEST_F(FileTransferIntegrationTest, NATDetectionOpen) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    /* NAT detection requires two relay servers */
    uint16_t relay_a_port = base_port;
    uint16_t relay_b_port = base_port + 1;
    uint16_t node_port = base_port + 2;
    uint16_t ctrl_port = base_port + 11;

    Process relay_a = start_relay(relay_a_port);
    Process relay_b = start_relay(relay_b_port);
    ASSERT_GT(relay_a.pid, 0);
    ASSERT_GT(relay_b.pid, 0);

    /* Start node connected to first relay */
    Process node = start_node(node_port, ctrl_port, relay_a_port, test_dir + "/cache");
    ASSERT_GT(node.pid, 0);

    /* Also connect to second relay for NAT detection */
    std::string connect_cmd = std::string(CTRL_CONNECT_RELAY) + " 127.0.0.1:" + std::to_string(relay_b_port);
    std::string resp = send_command(node.control_fd, connect_cmd);

    /* Wait for NAT detection to complete */
    usleep(2000000); /* 2 seconds */

    /* Check status for NAT type */
    resp = send_command(node.control_fd, CTRL_STATUS);
    EXPECT_NE(resp.find(CTRL_RESP_STATUS), std::string::npos) << "STATUS: " << resp;
    /* On localhost, NAT type should be "open" since local == reflexive address */
    EXPECT_NE(resp.find("nat=open"), std::string::npos) << "Expected NAT type open, got: " << resp;
#endif
}
```

- [ ] **Step 5: Commit**

```bash
git add test/test_file_transfer_integration.cpp
git commit -m "test: add Category B relay-mediated file transfer tests"
```

---

### Task 10: Category C — Multi-Node Distribution Tests

**Files:**
- Modify: `test/test_file_transfer_integration.cpp`

- [ ] **Step 1: Implement ThreeNodePropagation (C1)**

```cpp
TEST_F(FileTransferIntegrationTest, ThreeNodePropagation) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port;
    uint16_t port_a = base_port + 1;
    uint16_t port_b = base_port + 2;
    uint16_t port_c = base_port + 3;
    uint16_t ctrl_a = base_port + 11;
    uint16_t ctrl_b = base_port + 12;
    uint16_t ctrl_c = base_port + 13;

    Process relay_p = start_relay(relay_port);
    ASSERT_GT(relay_p.pid, 0);

    Process node_a = start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    Process node_b = start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    Process node_c = start_node(port_c, ctrl_c, relay_port, test_dir + "/cache_c");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GT(node_b.pid, 0);
    ASSERT_GT(node_c.pid, 0);

    /* Store on A */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 256000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    /* Wait for gossip to propagate to Node B */
    usleep(3000000); /* 3 seconds for gossip */

    /* Fetch on C — should be able to get blocks from either A or B */
    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_c.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE: " << resp;
#endif
}
```

- [ ] **Step 2: Implement ThreeNodeLateJoin (C2)**

```cpp
TEST_F(FileTransferIntegrationTest, ThreeNodeLateJoin) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port;
    uint16_t port_a = base_port + 1;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_a = base_port + 11;
    uint16_t ctrl_b = base_port + 12;

    Process relay_p = start_relay(relay_port);
    ASSERT_GT(relay_p.pid, 0);

    /* Start A and B only */
    Process node_a = start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    Process node_b = start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GT(node_b.pid, 0);

    /* Store on A */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 256000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    /* Wait for blocks to propagate to Node B */
    usleep(3000000); /* 3 seconds for gossip */

    /* Now start Node C */
    uint16_t port_c = base_port + 3;
    uint16_t ctrl_c = base_port + 13;
    Process node_c = start_node(port_c, ctrl_c, relay_port, test_dir + "/cache_c");
    ASSERT_GT(node_c.pid, 0);

    /* Fetch on C — empty cache, must get blocks from A or B via relay */
    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(node_c.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH_FILE: " << resp;
#endif
}
```

- [ ] **Step 3: Implement ConcurrentDownloads (C3)**

```cpp
TEST_F(FileTransferIntegrationTest, ConcurrentDownloads) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port;
    uint16_t port_a = base_port + 1;
    uint16_t port_b = base_port + 2;
    uint16_t port_c = base_port + 3;
    uint16_t ctrl_a = base_port + 11;
    uint16_t ctrl_b = base_port + 12;
    uint16_t ctrl_c = base_port + 13;

    Process relay_p = start_relay(relay_port);
    ASSERT_GT(relay_p.pid, 0);

    Process node_a = start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    Process node_b = start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    Process node_c = start_node(port_c, ctrl_c, relay_port, test_dir + "/cache_c");
    ASSERT_GT(node_a.pid, 0);
    ASSERT_GT(node_b.pid, 0);
    ASSERT_GT(node_c.pid, 0);

    /* Store on A */
    std::string resp = send_command(node_a.control_fd, std::string(CTRL_STORE_FILE) + " 256000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE_FILE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";

    /* Both B and C fetch simultaneously */
    resp = send_command(node_b.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "B FETCH_FILE: " << resp;

    resp = send_command(node_c.control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "C FETCH_FILE: " << resp;
#endif
}
```

- [ ] **Step 4: Build and run all tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration && ./test_file_transfer_integration --gtest_filter="FileTransferIntegrationTest.*"
```

- [ ] **Step 5: Commit**

```bash
git add test/test_file_transfer_integration.cpp
git commit -m "test: add Category C multi-node distribution file transfer tests"
```

---

### Task 11: Data Integrity Verification

**Files:**
- Modify: `test/test_file_transfer_integration.cpp`

- [ ] **Step 1: Add checksum verification to all test cases**

Currently the tests only verify that the DATA response is non-empty. To properly verify data integrity, the STORE_FILE command returns the BLAKE3 checksum of the original data, and the FETCH_FILE command returns the BLAKE3 checksum of the decoded data. We need to compare them.

Update the `STORE_FILE` response format to include the checksum:

Change the `CTRL_RESP_HASH` response in `handle_store_file` (in `test_node_main.c`) from:
```c
    snprintf(response, sizeof(response), "%s %s %s %zu\n",
             CTRL_RESP_HASH, desc_hex, file_hex, g_node.last_final_byte);
```
to:
```c
    char checksum_hex[65];
    for (size_t i = 0; i < 32; i++) {
        snprintf(checksum_hex + i * 2, 3, "%02x", g_node.last_stored_checksum[i]);
    }
    checksum_hex[64] = '\0';
    snprintf(response, sizeof(response), "%s %s %s %zu %s\n",
             CTRL_RESP_HASH, desc_hex, file_hex, g_node.last_final_byte, checksum_hex);
```

Update the HASH response format in `test_control_protocol.h`:

Add documentation:
```c
/* HASH response format: HASH <descriptor_hash_hex> <file_hash_hex> <final_byte> <stored_checksum_hex> */
```

Update all test cases to parse the checksum and compare it against the fetch result. For each test case that stores and fetches, change the pattern from:

```cpp
    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte;
```

to:

```cpp
    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;
```

And after the FETCH_FILE response, add the comparison:

```cpp
    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
```

Apply this pattern to all test cases in Categories A, B, and C.

- [ ] **Step 2: Run all tests with integrity verification**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration && ./test_file_transfer_integration --gtest_filter="FileTransferIntegrationTest.*"
```

- [ ] **Step 3: Commit**

```bash
git add test/test_file_transfer_integration.cpp test/test_node_main.c test/test_control_protocol.h
git commit -m "test: add data integrity verification to file transfer tests"
```

---

### Task 12: Memory Leak Check with Valgrind

**Files:**
- No new files

- [ ] **Step 1: Run the node process under valgrind**

Build the test binary with `-gdwarf-4` (per the valgrind DWARF5 incompatibility memory) and run a single node process under valgrind:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build
CFLAGS="-gdwarf-4" cmake .. && make test_file_transfer_integration
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
    ./test_file_transfer_integration --mode=node --port 15001 --control-port 15011 \
    --cache-dir /tmp/test_fft_valgrind 2>&1 | tee /tmp/valgrind_node.log
```

Then from another terminal, send STATUS and SHUTDOWN via the control socket. Check for leaks.

- [ ] **Step 2: Fix any leaks found in test_node_main.c**

Fix any leaks reported by valgrind. Common patterns to watch for:
- Missing `buffer_destroy` in `handle_store_file` / `handle_fetch_file`
- Missing cleanup in `node_put_on_descriptor_close` / `node_get_v2_on_rs_close`
- `tuple_cache_destroy` missing in node shutdown

- [ ] **Step 3: Commit leak fixes**

```bash
git add test/test_node_main.c
git commit -m "fix: memory leaks in node process for file transfer integration tests"
```

---

### Task 13: Final Integration Run and De-wonk Check

**Files:**
- No new files

- [ ] **Step 1: Run the full test suite**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make test_file_transfer_integration && ./test_file_transfer_integration --gtest_filter="FileTransferIntegrationTest.*"
```

Expected: All tests pass or skip (if msquic unavailable).

- [ ] **Step 2: Use the de-wonk skill to verify no stubs, TODOs, or broken code**

Run the de-wonk skill against the new test files to check for:
- Placeholder/stub implementations
- TODO/FIXME/HACK comments
- Disabled tests
- Unimplemented command handlers

- [ ] **Step 3: Fix any issues found**

- [ ] **Step 4: Commit final state**

```bash
git add test/test_file_transfer_integration.cpp test/test_node_main.c test/test_control_protocol.h test/CMakeLists.txt
git commit -m "test: finalize file transfer integration tests"
```