#include <gtest/gtest.h>
#include <cstring>
#include <atomic>
#include <future>
#include <string>
extern "C" {
#include "../src/ClientLibs/c/offs_client.h"
#include "../src/ClientAPI/health_handler.h"
#include "../src/ClientAPI/Unix/unix_transport.h"
#include "../src/ClientAPI/client_api_wire.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include "../src/ClientAPI/WS/ws_transport.h"
#include "../src/Platform/platform.h"
#include <string.h>
#include <stdlib.h>

/* usleep is POSIX-only; platform_sleep_ms is the cross-platform equivalent.
 * Call sites pass microsecond values (e.g. 10000 == 10ms), so divide by 1000. */
#define platform_usleep(us) platform_sleep_ms((us) / 1000)
}

struct PutCallbackContext {
    char* ori_string;
    std::promise<std::string> promise;
    std::atomic<int> called;
    PutCallbackContext() : ori_string(nullptr), called(0) {}
};

static void _put_callback(void* ctx, const char* ori_string) {
    PutCallbackContext* pctx = (PutCallbackContext*)ctx;
    if (ori_string != NULL) {
        free(pctx->ori_string);
        pctx->ori_string = strdup(ori_string);
        pctx->promise.set_value(std::string(ori_string));
    } else {
        pctx->promise.set_value(std::string{});
    }
    pctx->called.store(1, std::memory_order_release);
}

struct GetResult {
    bool error = false;
    uint8_t status_code = 0;
    size_t bytes_written = 0;
};

struct GetDataCallbackContext {
    std::promise<GetResult> promise;
    uint8_t* data;
    std::atomic<size_t> data_len;
    std::atomic<int> data_called;
    std::atomic<int> end_called;
    uint8_t error_status;
    std::atomic<int> error_called;
    GetDataCallbackContext() : data(nullptr), data_len(0), data_called(0), end_called(0), error_status(0), error_called(0) {}
};

/* Wait on a future with a timeout (in milliseconds). Returns true if the
 * future is ready (caller should .get() it), false on timeout. */
template <typename T>
static bool _wait_future(std::future<T>& fut, int timeout_ms) {
    return fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

static void _get_data_callback(void* ctx, const uint8_t* data, size_t len) {
    GetDataCallbackContext* gctx = (GetDataCallbackContext*)ctx;
    size_t old_len = gctx->data_len.load(std::memory_order_relaxed);
    size_t new_len = old_len + len;
    uint8_t* new_data;
    if (gctx->data != NULL) {
        new_data = (uint8_t*)realloc(gctx->data, new_len);
    } else {
        new_data = (uint8_t*)malloc(new_len);
    }
    if (new_data == NULL) {
        /* realloc/malloc failure — drop the frame but don't crash the test.
           Production code should handle this, but for tests the next
           memcmp on a partial buffer would be a confusing failure. */
        gctx->data_called.store(1, std::memory_order_release);
        return;
    }
    memcpy(new_data + old_len, data, len);
    gctx->data = new_data;
    gctx->data_len.store(new_len, std::memory_order_release);
    gctx->data_called.store(1, std::memory_order_release);
}

static void _get_end_callback(void* ctx) {
    GetDataCallbackContext* gctx = (GetDataCallbackContext*)ctx;
    gctx->end_called.store(1, std::memory_order_release);
    GetResult result;
    result.error = false;
    result.status_code = 0;
    result.bytes_written = gctx->data_len.load(std::memory_order_acquire);
    gctx->promise.set_value(result);
}

static void _error_callback(void* ctx, uint8_t status_code, const char* message) {
    GetDataCallbackContext* gctx = (GetDataCallbackContext*)ctx;
    gctx->error_status = status_code;
    gctx->error_called.store(1, std::memory_order_release);
    GetResult result;
    result.error = true;
    result.status_code = status_code;
    result.bytes_written = gctx->data_len.load(std::memory_order_acquire);
    gctx->promise.set_value(result);
    (void)message;
}

struct BlockPutCallbackContext {
    std::atomic<int> called;
    uint8_t status;
    uint8_t* hash_data;
    size_t hash_len;
    uint8_t hash_is_text;
};

static void _block_put_callback(void* ctx, uint8_t status,
    const uint8_t* hash_data, size_t hash_len, uint8_t hash_is_text) {
    BlockPutCallbackContext* bctx = (BlockPutCallbackContext*)ctx;
    bctx->status = status;
    if (hash_data != NULL && hash_len > 0) {
        bctx->hash_data = (uint8_t*)malloc(hash_len);
        memcpy(bctx->hash_data, hash_data, hash_len);
        bctx->hash_len = hash_len;
        bctx->hash_is_text = hash_is_text;
    }
    /* Set `called=1` LAST with release semantics so the reader sees all
     * preceding writes. */
    bctx->called.store(1, std::memory_order_release);
}

struct BlockGetCallbackContext {
    std::atomic<int> called;
    uint8_t status;
    uint8_t* data;
    size_t data_len;
};

static void _block_get_callback(void* ctx, uint8_t status,
    const uint8_t* data, size_t data_len) {
    BlockGetCallbackContext* bctx = (BlockGetCallbackContext*)ctx;
    bctx->status = status;
    if (data != NULL && data_len > 0) {
        bctx->data = (uint8_t*)malloc(data_len);
        memcpy(bctx->data, data, data_len);
        bctx->data_len = data_len;
    }
    /* Set `called=1` LAST with release semantics so the reader that polls
     * `called` is guaranteed to see all preceding writes (status, data,
     * data_len). Without this, the reader can observe `called=1` while
     * the data fields are still in the writer's CPU store buffer. */
    bctx->called.store(1, std::memory_order_release);
}

struct BlockDeleteCallbackContext {
    std::atomic<int> called;
    uint8_t status;
};

static void _block_delete_callback(void* ctx, uint8_t status) {
    BlockDeleteCallbackContext* bctx = (BlockDeleteCallbackContext*)ctx;
    bctx->status = status;
    bctx->called.store(1, std::memory_order_release);
}

struct HealthCallbackContext {
    int called;
    char* json_response;
};

static void _health_callback(void* ctx, const char* json_response) {
    HealthCallbackContext* hctx = (HealthCallbackContext*)ctx;
    hctx->called = 1;
    if (json_response != NULL) {
        hctx->json_response = strdup(json_response);
    }
}

namespace offs_client_test {

class TestOffsClient : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    unix_transport_t* transport;
    char* cache_dir;
    char socket_path[128];
    char url[256];
    health_context_t health_ctx;
    uint8_t running_val;
    uint8_t draining_val;

    void SetUp() override {
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create(pool);

        char dir_template[] = "/tmp/test_offs_client_XXXXXX";
        cache_dir = mkdtemp(dir_template);
        cache_dir = strdup(cache_dir);

        config_t config = {
            .index_bucket_size = 10,
            .index_wait = 1000,
            .index_max_wait = 5000,
            .section_size = 128000,
            .section_wait = 1000,
            .section_max_wait = 5000,
            .cache_size = 50,
            .max_tuple_size = 30,
            .lru_size = 50
        };
        bc = block_cache_create(config, cache_dir, standard, timer, pool, NULL, 0);
        ofd_cache = ofd_cache_create(pool, bc, 300000);
        tc = tuple_cache_create(100, pool);

        running_val = 1;
        draining_val = 0;
        memset(&health_ctx, 0, sizeof(health_ctx));
        health_ctx.block_cache = bc;
        health_ctx.running = &running_val;
        health_ctx.draining = &draining_val;

        snprintf(socket_path, sizeof(socket_path), "/tmp/test_client_sock_%d", platform_getpid());
        unlink(socket_path);

        transport = unix_transport_create(pool, bc, ofd_cache, tc, socket_path, NULL, &health_ctx);
        ASSERT_NE(transport, nullptr);
        unix_transport_start(transport);

        snprintf(url, sizeof(url), "unix://%s", socket_path);
    }

    void TearDown() override {
        if (transport != nullptr) {
            unix_transport_stop(transport);
        }
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        if (transport != nullptr) {
            unix_transport_destroy(transport);
        }
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

TEST_F(TestOffsClient, ConnectWithApiKey) {
    offs_client_t* client = offs_client_connect(url, "test-api-key");
    ASSERT_NE(client, nullptr);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, ConnectAndDisconnect) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutBufferedWithApiKey) {
    offs_client_t* client = offs_client_connect(url, "my-api-key");
    ASSERT_NE(client, nullptr);

    PutCallbackContext ctx;
    ctx.ori_string = nullptr;
    ctx.called = 0;

    const uint8_t data[] = "hello with api key";
    int result = offs_client_put(client, "application/octet-stream", "test.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    EXPECT_NE(ctx.ori_string, nullptr);
    free(ctx.ori_string);

    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutBuffered) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    PutCallbackContext ctx;
    ctx.ori_string = nullptr;
    ctx.called = 0;

    const uint8_t data[] = "hello from client";
    int result = offs_client_put(client, "application/octet-stream", "test.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &ctx);
    EXPECT_EQ(result, 0);

    /* Wait for callback with timeout */
    for (int attempts = 0; attempts < 200 && !ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    EXPECT_NE(ctx.ori_string, nullptr);
    free(ctx.ori_string);

    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutStreaming) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    PutCallbackContext ctx;
    ctx.ori_string = nullptr;
    ctx.called = 0;

    const uint8_t data[] = "streaming client data";
    int result = offs_client_put_stream_start(client, "application/octet-stream",
                                               "stream.bin", sizeof(data) - 1);
    EXPECT_EQ(result, 0);

    result = offs_client_put_stream_data(client, data, sizeof(data) - 1);
    EXPECT_EQ(result, 0);

    result = offs_client_put_stream_end(client, _put_callback, &ctx);
    EXPECT_EQ(result, 0);

    /* Wait for callback with timeout */
    for (int attempts = 0; attempts < 200 && !ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    EXPECT_NE(ctx.ori_string, nullptr);
    free(ctx.ori_string);

    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, GetAfterPut) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    const uint8_t data[] = "round trip client data";
    int result = offs_client_put(client, "application/octet-stream", "roundtrip.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    /* Wait for PUT callback */
    for (int attempts = 0; attempts < 200 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    /* Now GET the data back */
    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    // get_ctx fields initialized by default constructor

    result = offs_client_get(client, ori_string, _get_data_callback, _get_end_callback,
                              _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    /* Wait for GET end callback with timeout */
    if (!_wait_future(get_fut, 30000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.data_called, 1);
    EXPECT_EQ(get_ctx.end_called, 1);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(get_ctx.data_len.load(), sizeof(data) - 1);
        EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    offs_client_disconnect(client);
}

/* Generate deterministic pseudo-random data for upload/download tests.
   LCG: x_{n+1} = (x_n * 1103515245 + 12345) & 0xFF  */
static void _fill_random(uint8_t* buf, size_t len) {
    uint32_t state = 1;
    for (size_t i = 0; i < len; i++) {
        state = state * 1103515245u + 12345u;
        buf[i] = (uint8_t)(state >> 16);
    }
}

/* Verify that upload/download round-trips with larger file sizes,
   exercising the streaming backpressure path through the actor system. */
TEST_F(TestOffsClient, PutAndGet_256KB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);
    const size_t size = 256 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    _fill_random(data, size);
    PutCallbackContext put_ctx;
    offs_client_put(client, "application/octet-stream", "256kb.bin", size, data, size, _put_callback, &put_ctx);
    for (int a = 0; a < 500 && !put_ctx.called.load(std::memory_order_acquire); a++) platform_usleep(10000);
    ASSERT_EQ(put_ctx.called, 1);
    char* ori = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);
    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    offs_client_get(client, ori, _get_data_callback, _get_end_callback, _error_callback, &get_ctx);
    if (!_wait_future(get_fut, 30000)) { FAIL() << "GET timed out"; }
    (void)get_fut.get();
    EXPECT_EQ(get_ctx.end_called, 1) << " data_len=" << get_ctx.data_len.load();
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data) { EXPECT_EQ(memcmp(get_ctx.data, data, size), 0); free(get_ctx.data); }
    free(ori); free(data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutAndGet_128KB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);
    const size_t size = 128 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    _fill_random(data, size);
    PutCallbackContext put_ctx;
    offs_client_put(client, "application/octet-stream", "128kb.bin", size, data, size, _put_callback, &put_ctx);
    for (int a = 0; a < 500 && !put_ctx.called.load(std::memory_order_acquire); a++) platform_usleep(10000);
    ASSERT_EQ(put_ctx.called, 1);
    char* ori = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);
    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    offs_client_get(client, ori, _get_data_callback, _get_end_callback, _error_callback, &get_ctx);
    if (!_wait_future(get_fut, 30000)) { FAIL() << "GET timed out"; }
    (void)get_fut.get();
    EXPECT_EQ(get_ctx.end_called, 1) << " data_len=" << get_ctx.data_len.load();
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data) { EXPECT_EQ(memcmp(get_ctx.data, data, size), 0); free(get_ctx.data); }
    free(ori); free(data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutAndGet_100KB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);
    const size_t size = 100 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    _fill_random(data, size);
    PutCallbackContext put_ctx;
    offs_client_put(client, "application/octet-stream", "100kb.bin", size, data, size, _put_callback, &put_ctx);
    for (int a = 0; a < 500 && !put_ctx.called.load(std::memory_order_acquire); a++) platform_usleep(10000);
    ASSERT_EQ(put_ctx.called, 1);
    char* ori = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);
    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    offs_client_get(client, ori, _get_data_callback, _get_end_callback, _error_callback, &get_ctx);
    if (!_wait_future(get_fut, 30000)) { FAIL() << "GET timed out"; }
    (void)get_fut.get();
    EXPECT_EQ(get_ctx.end_called, 1);
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data) { EXPECT_EQ(memcmp(get_ctx.data, data, size), 0); free(get_ctx.data); }
    free(ori); free(data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutAndGet_512KB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);
    const size_t size = 512 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    _fill_random(data, size);
    PutCallbackContext put_ctx;
    offs_client_put(client, "application/octet-stream", "512kb.bin", size, data, size, _put_callback, &put_ctx);
    for (int a = 0; a < 500 && !put_ctx.called.load(std::memory_order_acquire); a++) platform_usleep(10000);
    ASSERT_EQ(put_ctx.called, 1);
    char* ori = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);
    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    get_ctx.data = (uint8_t*)malloc(size);
    offs_client_get(client, ori, _get_data_callback, _get_end_callback, _error_callback, &get_ctx);
    /* Block on the future — signalled by end_callback or error_callback. */
    if (!_wait_future(get_fut, 30000)) { FAIL() << "GET timed out"; }
    (void)get_fut.get();
    EXPECT_EQ(get_ctx.end_called.load(), 1) << " data_len=" << get_ctx.data_len.load();
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data) { EXPECT_EQ(memcmp(get_ctx.data, data, size), 0); free(get_ctx.data); }
    free(ori); free(data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutAndGet_1MB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    const size_t size = 1024 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    _fill_random(data, size);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    int result = offs_client_put(client, "application/octet-stream", "1mb.bin",
                                  size, data, size, _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 500 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    result = offs_client_get(client, ori_string, _get_data_callback,
                              _get_end_callback, _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    if (!_wait_future(get_fut, 30000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.end_called, 1);
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(memcmp(get_ctx.data, data, size), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    free(data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, PutAndGet_1_5MB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    const size_t size = 1536 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    _fill_random(data, size);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    int result = offs_client_put(client, "application/octet-stream", "1_5mb.bin",
                                  size, data, size, _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 1000 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    result = offs_client_get(client, ori_string, _get_data_callback,
                              _get_end_callback, _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    if (!_wait_future(get_fut, 60000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.end_called, 1);
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(memcmp(get_ctx.data, data, size), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    free(data);
    offs_client_disconnect(client);
}

/* Exercises the client's auto-chunking path: offs_client_put with a buffer
   larger than STREAM_FRAMER_MAX_FRAME_SIZE must be internally chunked into
   streaming frames by offs_client_put_ex. See docs/liboffs-audit-report.md #3
   and the 2 MB frame cap. */
TEST_F(TestOffsClient, PutAndGet_Buffered_3MB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    const size_t size = 3 * 1024 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    _fill_random(data, size);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    int result = offs_client_put(client, "application/octet-stream", "3mb_buffered.bin",
                                  size, data, size, _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 1000 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    result = offs_client_get(client, ori_string, _get_data_callback,
                              _get_end_callback, _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    if (!_wait_future(get_fut, 60000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.end_called, 1);
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(memcmp(get_ctx.data, data, size), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    free(data);
    offs_client_disconnect(client);
}

/* Streaming upload in 64KB chunks — exercises the writeable_off_stream
   backpressure path more aggressively than the single-shot buffered API. */
TEST_F(TestOffsClient, PutStreaming_1MB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    const size_t total_size = 1024 * 1024;
    const size_t chunk_size = 64 * 1024;
    uint8_t* data = (uint8_t*)malloc(total_size);
    _fill_random(data, total_size);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    int result = offs_client_put_stream_start(client, "application/octet-stream",
                                               "stream_1mb.bin", total_size);
    EXPECT_EQ(result, 0);

    for (size_t offset = 0; offset < total_size; offset += chunk_size) {
        size_t len = (offset + chunk_size <= total_size) ? chunk_size : (total_size - offset);
        result = offs_client_put_stream_data(client, data + offset, len);
        EXPECT_EQ(result, 0);
    }

    result = offs_client_put_stream_end(client, _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 500 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    result = offs_client_get(client, ori_string, _get_data_callback,
                              _get_end_callback, _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    if (!_wait_future(get_fut, 30000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.end_called, 1);
    EXPECT_EQ(get_ctx.data_len, total_size);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(memcmp(get_ctx.data, data, total_size), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    free(data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, GetInvalidOri) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    GetDataCallbackContext ctx;
    std::future<GetResult> get_fut = ctx.promise.get_future();

    int result = offs_client_get(client, "invalid-ori-string", _get_data_callback,
                                 _get_end_callback, _error_callback, &ctx);
    EXPECT_EQ(result, 0);

    /* Wait for error callback with timeout */
    if (!_wait_future(get_fut, 30000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    EXPECT_TRUE(get_res.error);
    EXPECT_NE(get_res.status_code, CLIENT_API_STATUS_OK);

    if (ctx.data != nullptr) {
        free(ctx.data);
    }
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, BlockPut) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    BlockPutCallbackContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    const uint8_t data[] = "block put test data";
    int result = offs_client_block_put(client, data, sizeof(data) - 1, 0,
                                       _block_put_callback, &ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    if (ctx.hash_data != nullptr) {
        free(ctx.hash_data);
    }

    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, BlockPutGetRoundTrip) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    const uint8_t data[] = "round trip block data";
    BlockPutCallbackContext put_ctx;
    memset(&put_ctx, 0, sizeof(put_ctx));

    int result = offs_client_block_put(client, data, sizeof(data) - 1, 0,
                                       _block_put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200
         && put_ctx.called.load(std::memory_order_acquire) == 0;
         attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.hash_data, nullptr);
    ASSERT_GT(put_ctx.hash_len, 0u);

    BlockGetCallbackContext get_ctx;

    result = offs_client_block_get(client, put_ctx.hash_data, put_ctx.hash_len,
                                   _block_get_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    /* Use atomic load with acquire to synchronize with the callback's
     * release-store on `called`. */
    for (int attempts = 0; attempts < 200
         && get_ctx.called.load(std::memory_order_acquire) == 0;
         attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(get_ctx.called, 1);
    if (get_ctx.status != 0) {
        FAIL() << "Block GET returned status=" << (int)get_ctx.status;
    }
    if (get_ctx.data == nullptr) {
        FAIL() << "Block GET returned null data (size=" << get_ctx.data_len << ")";
    }
    EXPECT_GE(get_ctx.data_len, sizeof(data) - 1);
    EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);

    free(put_ctx.hash_data);
    free(get_ctx.data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, BlockDelete) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    const uint8_t data[] = "delete test data";
    BlockPutCallbackContext put_ctx;
    memset(&put_ctx, 0, sizeof(put_ctx));

    int result = offs_client_block_put(client, data, sizeof(data) - 1, 0,
                                       _block_put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200
         && put_ctx.called.load(std::memory_order_acquire) == 0;
         attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.hash_data, nullptr);

    BlockDeleteCallbackContext del_ctx;
    memset(&del_ctx, 0, sizeof(del_ctx));

    result = offs_client_block_delete(client, put_ctx.hash_data, put_ctx.hash_len,
                                      _block_delete_callback, &del_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !del_ctx.called; attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(del_ctx.called, 1);

    free(put_ctx.hash_data);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, Health) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    HealthCallbackContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    int result = offs_client_health(client, _health_callback, &ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    ASSERT_NE(ctx.json_response, nullptr);
    EXPECT_NE(strstr(ctx.json_response, "\"status\""), nullptr);

    free(ctx.json_response);
    offs_client_disconnect(client);
}

} // namespace offs_client_test

namespace offs_ws_client_test {

class TestOffsWsClient : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    ws_transport_t* transport;
    char* cache_dir;
    static uint16_t _next_port;
    uint16_t port;
    char url[256];

    void SetUp() override {
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create(pool);

        char dir_template[] = "/tmp/test_offs_ws_client_XXXXXX";
        cache_dir = mkdtemp(dir_template);
        cache_dir = strdup(cache_dir);

        config_t config = {
            .index_bucket_size = 10,
            .index_wait = 1000,
            .index_max_wait = 5000,
            .section_size = 128000,
            .section_wait = 1000,
            .section_max_wait = 5000,
            .cache_size = 50,
            .max_tuple_size = 30,
            .lru_size = 50
        };
        bc = block_cache_create(config, cache_dir, standard, timer, pool, NULL, 0);
        ofd_cache = ofd_cache_create(pool, bc, 300000);
        tc = tuple_cache_create(100, pool);

        port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
        snprintf(url, sizeof(url), "ws://127.0.0.1:%d/offs", port);

        transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, 0, NULL, NULL);
        for (int retry = 0; transport == nullptr && retry < 10; retry++) {
            port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
            snprintf(url, sizeof(url), "ws://127.0.0.1:%d/offs", port);
            transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, 0, NULL, NULL);
        }
        ASSERT_NE(transport, nullptr);
        ws_transport_start(transport);

        /* Wait for server to be ready */
        platform_usleep(100000);
    }

    void TearDown() override {
        if (transport != nullptr) {
            ws_transport_stop(transport);
        }
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        if (transport != nullptr) {
            ws_transport_destroy(transport);
        }
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

uint16_t TestOffsWsClient::_next_port = 40080;

TEST_F(TestOffsWsClient, ConnectWithApiKey) {
    offs_client_t* client = offs_client_connect(url, "ws-api-key");
    ASSERT_NE(client, nullptr);
    offs_client_disconnect(client);
}

TEST_F(TestOffsWsClient, ConnectAndDisconnect) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);
    offs_client_disconnect(client);
}

TEST_F(TestOffsWsClient, PutBuffered) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    PutCallbackContext ctx;
    ctx.ori_string = nullptr;
    ctx.called = 0;

    const uint8_t data[] = "hello ws client";
    int result = offs_client_put(client, "application/octet-stream", "test.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    EXPECT_NE(ctx.ori_string, nullptr);
    free(ctx.ori_string);

    offs_client_disconnect(client);
}

TEST_F(TestOffsWsClient, GetAfterPut) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    const uint8_t data[] = "ws round trip data";
    int result = offs_client_put(client, "application/octet-stream", "roundtrip.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    // get_ctx fields initialized by default constructor

    result = offs_client_get(client, ori_string, _get_data_callback, _get_end_callback,
                            _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    if (!_wait_future(get_fut, 30000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.data_called, 1);
    EXPECT_EQ(get_ctx.end_called, 1);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(get_ctx.data_len.load(), sizeof(data) - 1);
        EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    offs_client_disconnect(client);
}

/* Large WS round trip: exercises the server ws_connection send path for a frame
 * that does not fit in one send. On Windows IOCP, PD_EVENT_WRITE is never
 * delivered, so the server must flush partial sends via a bounded blocking
 * retry (see ws_connection.c) — without it this test times out at GET. */
TEST_F(TestOffsWsClient, PutAndGet_1MB) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    const size_t size = 1024 * 1024;
    uint8_t* data = (uint8_t*)malloc(size);
    /* LCG fill (same generator as _fill_random in the offs_client_test
     * namespace) — inlined here because _fill_random is not visible across
     * namespaces. */
    {
        uint32_t st = 1;
        for (size_t i = 0; i < size; i++) { st = st * 1103515245u + 12345u; data[i] = (uint8_t)(st >> 16); }
    }

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    int result = offs_client_put(client, "application/octet-stream", "1mb_ws.bin",
                                  size, data, size, _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 500 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    result = offs_client_get(client, ori_string, _get_data_callback,
                              _get_end_callback, _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    if (!_wait_future(get_fut, 30000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.end_called, 1);
    EXPECT_EQ(get_ctx.data_len.load(), size);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(memcmp(get_ctx.data, data, size), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    free(data);
    offs_client_disconnect(client);
}

} // namespace offs_ws_client_test

/* --- WebTransport client tests (requires MsQuic) --- */
#ifdef HAS_MSQUIC
extern "C" {
#include "../src/ClientAPI/WT/wt_transport.h"
#include "../src/Util/atomic_compat.h"
#include "../tools/offs-ca/ca_ops.h"
}

namespace offs_wt_client_test {

class TestOffsWtClient : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    wt_transport_t* transport;
    char* cache_dir;
    char cert_path[256];
    char key_path[256];
    static uint16_t _next_port;
    uint16_t port;
    char url[256];

    void SetUp() override {
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create(pool);

        char dir_template[] = "/tmp/test_offs_wt_client_XXXXXX";
        cache_dir = mkdtemp(dir_template);
        cache_dir = strdup(cache_dir);

        /* Generate a self-signed RSA cert for the WT server in-process via the
         * same ca_generate routine the offs-ca tool exposes (and that
         * test_peer_verify uses). The old system("openssl req ... 2>/dev/null")
         * shell-out broke on Windows cmd, which does not understand /dev/null,
         * so the redirect failed before openssl ran, no cert was produced, and
         * wt_transport_create failed at ConfigurationLoadCredential. RSA is
         * used (not ed25519) because Schannel — msquic's Windows TLS provider
         * — reliably accepts RSA. */
        snprintf(cert_path, sizeof(cert_path), "%s/test_cert.pem", cache_dir);
        snprintf(key_path, sizeof(key_path), "%s/test_key.pem", cache_dir);
        int rc = ca_generate("/CN=localhost", 1, "rsa", cert_path, key_path);
        if (rc != 0) {
            /* Cannot generate cert — tests will skip */
            cert_path[0] = '\0';
            key_path[0] = '\0';
        }

        config_t config = {
            .index_bucket_size = 10,
            .index_wait = 1000,
            .index_max_wait = 5000,
            .section_size = 128000,
            .section_wait = 1000,
            .section_max_wait = 5000,
            .cache_size = 50,
            .max_tuple_size = 30,
            .lru_size = 50
        };
        bc = block_cache_create(config, cache_dir, standard, timer, pool, NULL, 0);
        ofd_cache = ofd_cache_create(pool, bc, 300000);
        tc = tuple_cache_create(100, pool);

        port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
        snprintf(url, sizeof(url), "wt://127.0.0.1:%d", port);

        const char* cp = (cert_path[0] != '\0') ? cert_path : NULL;
        const char* kp = (key_path[0] != '\0') ? key_path : NULL;
        transport = wt_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, cp, kp, 0, NULL, NULL);
        if (transport != nullptr) {
            wt_transport_start(transport);
            /* Wait for QUIC server listener to be ready */
            for (int attempts = 0; attempts < 100 && !atomic_load(&transport->listening); attempts++) {
                platform_usleep(10000);
            }
            if (!atomic_load(&transport->listening)) {
                /* Server thread exited early — join it and destroy transport */
                wt_transport_stop(transport);
                wt_transport_destroy(transport);
                transport = nullptr;
            }
        }
    }

    void TearDown() override {
        if (transport != nullptr) {
            wt_transport_stop(transport);
        }
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        /*
         * wt_transport_destroy calls MsQuic RegistrationClose which blocks
         * indefinitely if server-side connections haven't fully shut down.
         * Since the test process will clean up on exit, we skip
         * wt_transport_destroy and let the OS reclaim resources.
         * The server thread has already been stopped and joined above.
         */
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

uint16_t TestOffsWtClient::_next_port = 45200;

TEST_F(TestOffsWtClient, ConnectAndDisconnect) {
    if (transport == nullptr) {
        GTEST_SKIP() << "WT transport creation failed (MsQuic not available)";
    }
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);
    offs_client_disconnect(client);
}

TEST_F(TestOffsWtClient, PutBuffered) {
    if (transport == nullptr) {
        GTEST_SKIP() << "WT transport creation failed (MsQuic not available)";
    }
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    PutCallbackContext ctx;
    ctx.ori_string = nullptr;
    ctx.called = 0;

    const uint8_t data[] = "hello wt client";
    int result = offs_client_put(client, "application/octet-stream", "test.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    EXPECT_NE(ctx.ori_string, nullptr);
    free(ctx.ori_string);

    offs_client_disconnect(client);
}

TEST_F(TestOffsWtClient, GetAfterPut) {
    if (transport == nullptr) {
        GTEST_SKIP() << "WT transport creation failed (MsQuic not available)";
    }
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    PutCallbackContext put_ctx;
    put_ctx.ori_string = nullptr;
    put_ctx.called = 0;

    const uint8_t data[] = "wt round trip data";
    int result = offs_client_put(client, "application/octet-stream", "roundtrip.bin",
                                  sizeof(data) - 1, data, sizeof(data) - 1,
                                  _put_callback, &put_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !put_ctx.called.load(std::memory_order_acquire); attempts++) {
        platform_usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    std::future<GetResult> get_fut = get_ctx.promise.get_future();
    // get_ctx fields initialized by default constructor

    result = offs_client_get(client, ori_string, _get_data_callback, _get_end_callback,
                            _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    if (!_wait_future(get_fut, 30000)) {
        FAIL() << "GET timed out";
    }
    GetResult get_res = get_fut.get();

    if (get_res.error) {
        FAIL() << "Got error response, status_code=" << (int)get_res.status_code;
    }

    EXPECT_EQ(get_ctx.data_called, 1);
    EXPECT_EQ(get_ctx.end_called, 1);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(get_ctx.data_len.load(), sizeof(data) - 1);
        EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    offs_client_disconnect(client);
}

TEST_F(TestOffsWtClient, Health) {
    if (transport == nullptr) {
        GTEST_SKIP() << "WT transport creation failed (MsQuic not available)";
    }
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    HealthCallbackContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    int result = offs_client_health(client, _health_callback, &ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        platform_usleep(10000);
    }
    EXPECT_EQ(ctx.called, 1);
    ASSERT_NE(ctx.json_response, nullptr);
    EXPECT_NE(strstr(ctx.json_response, "\"status\""), nullptr);

    free(ctx.json_response);
    offs_client_disconnect(client);
}

} // namespace offs_wt_client_test

#endif // HAS_MSQUIC