#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/ClientLibs/c/offs_client.h"
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
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
}

struct PutCallbackContext {
    char* ori_string;
    int called;
};

static void _put_callback(void* ctx, const char* ori_string) {
    PutCallbackContext* pctx = (PutCallbackContext*)ctx;
    pctx->called = 1;
    if (ori_string != NULL) {
        free(pctx->ori_string);
        pctx->ori_string = strdup(ori_string);
    }
}

struct GetDataCallbackContext {
    uint8_t* data;
    size_t data_len;
    int data_called;
    int end_called;
    uint8_t error_status;
    int error_called;
};

static void _get_data_callback(void* ctx, const uint8_t* data, size_t len) {
    GetDataCallbackContext* gctx = (GetDataCallbackContext*)ctx;
    if (gctx->data != NULL) {
        gctx->data = (uint8_t*)realloc(gctx->data, gctx->data_len + len);
    } else {
        gctx->data = (uint8_t*)malloc(len);
    }
    memcpy(gctx->data + gctx->data_len, data, len);
    gctx->data_len += len;
    gctx->data_called = 1;
}

static void _get_end_callback(void* ctx) {
    GetDataCallbackContext* gctx = (GetDataCallbackContext*)ctx;
    gctx->end_called = 1;
}

static void _error_callback(void* ctx, uint8_t status_code, const char* message) {
    GetDataCallbackContext* gctx = (GetDataCallbackContext*)ctx;
    gctx->error_status = status_code;
    gctx->error_called = 1;
    (void)message;
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

    void SetUp() override {
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create();

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

        snprintf(socket_path, sizeof(socket_path), "/tmp/test_client_sock_%d", getpid());
        unlink(socket_path);

        transport = unix_transport_create(pool, bc, ofd_cache, tc, socket_path, NULL);
        ASSERT_NE(transport, nullptr);
        unix_transport_start(transport);

        snprintf(url, sizeof(url), "unix://%s", socket_path);
    }

    void TearDown() override {
        if (transport != nullptr) {
            unix_transport_stop(transport);
        }
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
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

    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        usleep(10000);
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
    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        usleep(10000);
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
    for (int attempts = 0; attempts < 200 && !ctx.called; attempts++) {
        usleep(10000);
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
    for (int attempts = 0; attempts < 200 && !put_ctx.called; attempts++) {
        usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    /* Now GET the data back */
    GetDataCallbackContext get_ctx;
    memset(&get_ctx, 0, sizeof(get_ctx));
    get_ctx.data = nullptr;
    get_ctx.data_len = 0;

    result = offs_client_get(client, ori_string, _get_data_callback, _get_end_callback,
                              _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    /* Wait for GET end callback with timeout */
    for (int attempts = 0; attempts < 200 && !get_ctx.end_called && !get_ctx.error_called; attempts++) {
        usleep(10000);
    }

    if (get_ctx.error_called) {
        FAIL() << "Got error response, status_code=" << (int)get_ctx.error_status;
    }

    EXPECT_EQ(get_ctx.data_called, 1);
    EXPECT_EQ(get_ctx.end_called, 1);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(get_ctx.data_len, sizeof(data) - 1);
        EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    offs_client_disconnect(client);
}

TEST_F(TestOffsClient, GetInvalidOri) {
    offs_client_t* client = offs_client_connect(url, NULL);
    ASSERT_NE(client, nullptr);

    GetDataCallbackContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.data = nullptr;
    ctx.data_len = 0;

    int result = offs_client_get(client, "invalid-ori-string", _get_data_callback,
                                 _get_end_callback, _error_callback, &ctx);
    EXPECT_EQ(result, 0);

    /* Wait for error callback with timeout */
    for (int attempts = 0; attempts < 200 && !ctx.error_called && !ctx.end_called; attempts++) {
        usleep(10000);
    }

    EXPECT_EQ(ctx.error_called, 1);
    EXPECT_NE(ctx.error_status, CLIENT_API_STATUS_OK);

    if (ctx.data != nullptr) {
        free(ctx.data);
    }
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
        timer = timer_actor_create();

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

        port = _next_port++;
        snprintf(url, sizeof(url), "ws://127.0.0.1:%d/offs", port);

        transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, 0, NULL);
        ASSERT_NE(transport, nullptr);
        ws_transport_start(transport);

        /* Wait for server to be ready */
        usleep(100000);
    }

    void TearDown() override {
        if (transport != nullptr) {
            ws_transport_stop(transport);
        }
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
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
        usleep(10000);
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

    for (int attempts = 0; attempts < 200 && !put_ctx.called; attempts++) {
        usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    memset(&get_ctx, 0, sizeof(get_ctx));
    get_ctx.data = nullptr;
    get_ctx.data_len = 0;

    result = offs_client_get(client, ori_string, _get_data_callback, _get_end_callback,
                            _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !get_ctx.end_called && !get_ctx.error_called; attempts++) {
        usleep(10000);
    }

    if (get_ctx.error_called) {
        FAIL() << "Got error response, status_code=" << (int)get_ctx.error_status;
    }

    EXPECT_EQ(get_ctx.data_called, 1);
    EXPECT_EQ(get_ctx.end_called, 1);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(get_ctx.data_len, sizeof(data) - 1);
        EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    offs_client_disconnect(client);
}

} // namespace offs_ws_client_test

/* --- WebTransport client tests (requires MsQuic) --- */
#ifdef HAS_MSQUIC
extern "C" {
#include "../src/ClientAPI/WT/wt_transport.h"
#include "../src/Util/atomic_compat.h"
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
        timer = timer_actor_create();

        char dir_template[] = "/tmp/test_offs_wt_client_XXXXXX";
        cache_dir = mkdtemp(dir_template);
        cache_dir = strdup(cache_dir);

        /* Generate self-signed cert for WT server */
        snprintf(cert_path, sizeof(cert_path), "%s/test_cert.pem", cache_dir);
        snprintf(key_path, sizeof(key_path), "%s/test_key.pem", cache_dir);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
            "-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
            key_path, cert_path);
        int rc = system(cmd);
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

        port = _next_port++;
        snprintf(url, sizeof(url), "wt://127.0.0.1:%d", port);

        const char* cp = (cert_path[0] != '\0') ? cert_path : NULL;
        const char* kp = (key_path[0] != '\0') ? key_path : NULL;
        transport = wt_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, cp, kp, 0, NULL);
        if (transport != nullptr) {
            wt_transport_start(transport);
            /* Wait for QUIC server listener to be ready */
            for (int attempts = 0; attempts < 100 && !atomic_load(&transport->listening); attempts++) {
                usleep(10000);
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
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
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
        usleep(10000);
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

    for (int attempts = 0; attempts < 200 && !put_ctx.called; attempts++) {
        usleep(10000);
    }
    ASSERT_EQ(put_ctx.called, 1);
    ASSERT_NE(put_ctx.ori_string, nullptr);
    char* ori_string = strdup(put_ctx.ori_string);
    free(put_ctx.ori_string);

    GetDataCallbackContext get_ctx;
    memset(&get_ctx, 0, sizeof(get_ctx));
    get_ctx.data = nullptr;
    get_ctx.data_len = 0;

    result = offs_client_get(client, ori_string, _get_data_callback, _get_end_callback,
                            _error_callback, &get_ctx);
    EXPECT_EQ(result, 0);

    for (int attempts = 0; attempts < 200 && !get_ctx.end_called && !get_ctx.error_called; attempts++) {
        usleep(10000);
    }

    if (get_ctx.error_called) {
        FAIL() << "Got error response, status_code=" << (int)get_ctx.error_status;
    }

    EXPECT_EQ(get_ctx.data_called, 1);
    EXPECT_EQ(get_ctx.end_called, 1);
    if (get_ctx.data != nullptr) {
        EXPECT_EQ(get_ctx.data_len, sizeof(data) - 1);
        EXPECT_EQ(memcmp(get_ctx.data, data, sizeof(data) - 1), 0);
        free(get_ctx.data);
    }

    free(ori_string);
    offs_client_disconnect(client);
}

} // namespace offs_wt_client_test

#endif // HAS_MSQUIC