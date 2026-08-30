#include <gtest/gtest.h>
#include <cstring>
#include <vector>
extern "C" {
#include "../src/ClientAPI/HTTP/off_routes.h"
#include "../src/ClientAPI/HTTP/http_server.h"
#include "../src/ClientAPI/HTTP/http_request.h"
#include "../src/ClientAPI/HTTP/http_response.h"
#include "../src/OFFStreams/off_url.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/index.h"
#include "../src/Buffer/buffer.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_socket.h"
#include <string.h>
#include <stdlib.h>

/* usleep is POSIX-only; platform_sleep_ms is the cross-platform equivalent.
 * Call sites pass microsecond values (e.g. 10000 == 10ms), so divide by 1000. */
#define platform_usleep(us) platform_sleep_ms((us) / 1000)
}

namespace off_routes_load_test {

static uint16_t _next_port = 21080;

static platform_socket_t* _connect_to_server(uint16_t port) {
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
    if (sock == NULL) return NULL;

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_INET;
    addr.inet.addr = 0x0100007f; /* 127.0.0.1 in network byte order */
    addr.inet.port = port;

    if (platform_socket_connect(sock, &addr) != 0) {
        platform_socket_destroy(sock);
        return NULL;
    }
    platform_socket_set_nonblocking(sock);
    return sock;
}

static int _send_all(platform_socket_t* sock, const char* buf, size_t len) {
    size_t sent_total = 0;
    for (int attempts = 0; attempts < 1000 && sent_total < len; attempts++) {
        ssize_t sent = platform_socket_send(sock, buf + sent_total, len - sent_total);
        if (sent > 0) {
            sent_total += (size_t)sent;
        } else if (sent == 0) {
            return -1;
        } else {
            /* Nonblocking socket: EWOULDBLOCK means try again shortly. */
            platform_sleep_ms(10);
        }
    }
    return (sent_total == len) ? 0 : -1;
}

static platform_socket_t* _connect_with_retry(uint16_t port) {
    platform_socket_t* sock = NULL;
    for (int attempts = 0; attempts < 50; attempts++) {
        platform_usleep(10000);
        sock = _connect_to_server(port);
        if (sock != NULL) break;
    }
    return sock;
}

/* Read the response body until the server closes the connection (EOF) or the
 * timeout expires. Load responses are close-delimited (unknown body length),
 * so a complete body is proven by recv returning 0 — the load-stream
 * termination property this suite exists to pin. Returns total bytes read,
 * or -1 if nothing arrived before the timeout. */
static int _recv_until_close(platform_socket_t* sock, char* response,
                             size_t response_size, int timeout_ms) {
    size_t total_received = 0;
    bool saw_eof = false;
    for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
        if (total_received + 1 >= response_size) break;
        ssize_t received = platform_socket_recv(sock, response + total_received,
                                                response_size - total_received - 1);
        if (received > 0) {
            total_received += (size_t)received;
            continue;
        }
        if (received == 0) {
            saw_eof = true;
            break;
        }
        /* EWOULDBLOCK on a nonblocking socket: back off and retry. */
        platform_sleep_ms(10);
    }
    response[total_received] = '\0';
    return saw_eof ? (int)total_received : -1;
}

/* Send a request and read a close-delimited response to completion (EOF). */
static int _send_and_recv_load(platform_socket_t* sock, const char* request, size_t req_len,
                               char* response, size_t response_size, int timeout_ms) {
    if (_send_all(sock, request, req_len) != 0) return -1;
    return _recv_until_close(sock, response, response_size, timeout_ms);
}

/* Send a request and read a Content-Length-delimited (keep-alive) response. */
static int _send_and_recv(platform_socket_t* sock, const char* request, size_t req_len,
                          char* response, size_t response_size, int timeout_ms) {
    if (_send_all(sock, request, req_len) != 0) return -1;

    size_t total_received = 0;
    for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
        if (total_received + 1 >= response_size) break;
        ssize_t received = platform_socket_recv(sock, response + total_received,
                                                response_size - total_received - 1);
        if (received > 0) {
            total_received += (size_t)received;
            response[total_received] = '\0';
            char* header_end = strstr(response, "\r\n\r\n");
            if (header_end != NULL) {
                size_t header_len = (size_t)(header_end - response) + 4;
                char* content_length_str = strstr(response, "Content-Length: ");
                if (content_length_str != NULL && content_length_str < header_end) {
                    size_t content_length = (size_t)atol(content_length_str + 16);
                    if (total_received >= header_len + content_length) {
                        return 0;
                    }
                }
                if (strstr(response, "Connection: close") != NULL &&
                    total_received > header_len) {
                    return 0;
                }
            }
        } else if (received == 0) {
            response[total_received] = '\0';
            return total_received > 0 ? 0 : -1;
        } else {
            /* EWOULDBLOCK on a nonblocking socket: back off and retry. */
            platform_sleep_ms(10);
        }
    }
    response[total_received] = '\0';
    return total_received > 0 ? 0 : -1;
}

class TestOffRoutesLoad : public testing::Test {
protected:
    scheduler_pool_t* pool;
    http_server_t* put_server;    /* uploads land here (seeds the tuple cache) */
    http_server_t* load_server;   /* loads run here with a fresh tuple cache */
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* put_tc;
    tuple_cache_t* load_tc;
    timer_actor_t* timer;
    uint16_t put_port;
    uint16_t load_port;
    char* cache_dir;

    void SetUp() override {
        put_port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
        load_port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);

        char dir_template[] = "/tmp/test_off_routes_load_XXXXXX";
        cache_dir = mkdtemp(dir_template);
        cache_dir = strdup(cache_dir);

        timer = timer_actor_create(pool);
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
        put_tc = tuple_cache_create(100, pool);
        load_tc = tuple_cache_create(100, pool);
        put_server = http_server_create(pool, "127.0.0.1", put_port);
        load_server = http_server_create(pool, "127.0.0.1", load_port);

        off_routes_register(put_server, pool, bc, ofd_cache, put_tc, NULL, NULL, NULL, NULL);
        off_routes_register(load_server, pool, bc, ofd_cache, load_tc, NULL, NULL, NULL, NULL);
        http_server_listen(put_server);
        http_server_listen(load_server);

        /* Give both listeners a moment to bind before the tests connect. */
        platform_socket_t* probe = _connect_to_server(load_port);
        for (int attempts = 0; attempts < 50 && probe == NULL; attempts++) {
            platform_usleep(10000);
            probe = _connect_to_server(load_port);
        }
        if (probe != NULL) platform_socket_destroy(probe);
    }

    void TearDown() override {
        http_server_stop(put_server);
        http_server_stop(load_server);
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        http_server_destroy(put_server);
        http_server_destroy(load_server);
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(put_tc);
        tuple_cache_destroy(load_tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }

    /* PUT a file through the upload route; returns the OFF URL string
     * (malloc'd, caller frees) or NULL on failure. */
    char* _upload(const void* body, size_t body_len, const char* file_name) {
        platform_socket_t* sock = _connect_with_retry(put_port);
        if (sock == NULL) return NULL;

        size_t request_size = body_len + 1024;
        char* put_request = (char*)malloc(request_size);
        int put_len = snprintf(put_request, request_size,
            "PUT /offsystem HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "type: application/octet-stream\r\n"
            "file-name: %s\r\n"
            "stream-length: %zu\r\n"
            "Content-Length: %zu\r\n"
            "\r\n",
            file_name, body_len, body_len);
        memcpy(put_request + put_len, body, body_len);
        put_len += (int)body_len;

        char put_response[8192];
        memset(put_response, 0, sizeof(put_response));
        int result = _send_and_recv(sock, put_request, (size_t)put_len,
                                    put_response, sizeof(put_response), 5000);
        free(put_request);
        platform_socket_destroy(sock);
        if (result != 0) return NULL;

        char* header_end = strstr(put_response, "\r\n\r\n");
        if (header_end == NULL) return NULL;
        char* put_body = header_end + 4;
        size_t url_len = strlen(put_body);
        while (url_len > 0 && (put_body[url_len-1] == '\r' || put_body[url_len-1] == '\n' || put_body[url_len-1] == ' '))
            put_body[--url_len] = '\0';
        /* The route may respond with an absolute URL (server-address prefix);
         * the OFF path is everything from /offsystem/v3/ on. */
        char* path_start = strstr(put_body, "/offsystem/v3/");
        if (path_start == NULL) return NULL;
        return strdup(path_start);
    }

    /* GET a URL on the load server, reading the close-delimited response to
     * EOF. Returns bytes read or -1 (nothing arrived / never terminated). */
    int _get_load(const char* url, char* response, size_t response_size, int timeout_ms) {
        platform_socket_t* sock = _connect_with_retry(load_port);
        if (sock == NULL) return -1;
        char get_request[4096];
        int get_len = snprintf(get_request, sizeof(get_request),
            "GET %s HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n",
            url);
        int result = _send_and_recv_load(sock, get_request, (size_t)get_len,
                                         response, response_size, timeout_ms);
        platform_socket_destroy(sock);
        return result;
    }
};

/* An all-loaded load streams one ndjson progress line per tuple and exactly
 * one terminal line, with Content-Type application/x-ndjson; the same URL
 * WITHOUT ?load=1 must still return the plain file bytes. */
TEST_F(TestOffRoutesLoad, LoadStreamsNdjsonTerminalLoaded) {
    const char* body = "Hello OFF System!";
    size_t body_len = strlen(body);
    char* url = _upload(body, body_len, "load_test.txt");
    ASSERT_NE(url, nullptr);

    char request_url[4096];
    snprintf(request_url, sizeof(request_url), "%s?load=1", url);

    char response[8192];
    memset(response, 0, sizeof(response));
    int received = _get_load(request_url, response, sizeof(response), 5000);
    ASSERT_GT(received, 0) << "load response never terminated (no EOF)";
    EXPECT_NE(strstr(response, "HTTP/1.1 200"), nullptr);
    EXPECT_NE(strstr(response, "application/x-ndjson"), nullptr);

    char* body_start = strstr(response, "\r\n\r\n");
    ASSERT_NE(body_start, nullptr);
    body_start += 4;

    /* 17 bytes = one 128000-byte tuple. One progress line + one terminal. */
    EXPECT_NE(strstr(body_start, "{\"tuples_loaded\":1,\"tuples_total\":1}\n"), nullptr);
    const char* terminal = strstr(body_start, "{\"status\":");
    ASSERT_NE(terminal, nullptr) << "terminal ndjson line missing";
    EXPECT_NE(strstr(terminal, "\"status\":\"loaded\""), nullptr);
    /* Exactly one terminal line. */
    EXPECT_EQ(strstr(terminal + 1, "{\"status\":"), nullptr);

    platform_socket_t* sock = _connect_with_retry(load_port);
    ASSERT_NE(sock, nullptr);

    /* Regression pin: the bare URL (no ?load=1) still serves the file bytes. */
    int get_len = snprintf(request_url, sizeof(request_url),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", url);
    char plain_response[8192];
    memset(plain_response, 0, sizeof(plain_response));
    ASSERT_EQ(_send_and_recv(sock, request_url, (size_t)get_len,
                             plain_response, sizeof(plain_response), 5000), 0);
    EXPECT_EQ(strstr(plain_response, "HTTP/1.1 200"), plain_response);
    EXPECT_EQ(strstr(plain_response, "Content-Type: application/x-ndjson"), nullptr);
    char* plain_body = strstr(plain_response, "\r\n\r\n");
    ASSERT_NE(plain_body, nullptr);
    EXPECT_STREQ(plain_body + 4, body);

    /* Bare `?load` (no =1) must trigger the load flow too. */
    snprintf(request_url, sizeof(request_url), "%s?load", url);
    char bare_response[8192];
    memset(bare_response, 0, sizeof(bare_response));
    ASSERT_GT(_get_load(request_url, bare_response, sizeof(bare_response), 5000), 0);
    EXPECT_NE(strstr(bare_response, "application/x-ndjson"), nullptr);
    EXPECT_NE(strstr(bare_response, "{\"status\":\"loaded\""), nullptr);

    /* `?load=0` explicitly disables the load flow: the request must be
       served as a plain file response, not the ndjson load stream. */
    char url_disabled[4096];
    snprintf(url_disabled, sizeof(url_disabled), "%s?load=0", url);
    char disabled_response[8192];
    memset(disabled_response, 0, sizeof(disabled_response));
    get_len = snprintf(request_url, sizeof(request_url),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", url_disabled);
    ASSERT_EQ(_send_and_recv(sock, request_url, (size_t)get_len,
                             disabled_response, sizeof(disabled_response), 5000), 0);
    EXPECT_EQ(strstr(disabled_response, "Content-Type: application/x-ndjson"), nullptr);
    char* disabled_body = strstr(disabled_response, "\r\n\r\n");
    ASSERT_NE(disabled_body, nullptr);
    EXPECT_STREQ(disabled_body + 4, body);

    free(url);
}

/* Delete one non-descriptor data block, then load the same ORI with a fresh
 * tuple cache: the damaged tuple is skipped and tallied, and — this is the
 * hang the load-mode fix targets — the response still terminates at EOF with
 * a "partial" terminal line. */
TEST_F(TestOffRoutesLoad, LoadPartialSkipsMissingTuples) {
    /* 384000 bytes spans three 128000-byte standard blocks -> 3 tuples. */
    const size_t data_size = 384000;
    std::vector<uint8_t> data(data_size);
    for (size_t index = 0; index < data_size; index++) {
        data[index] = (uint8_t)((index * 7) & 0xFF);
    }

    char* url = _upload(data.data(), data_size, "load_partial.bin");
    ASSERT_NE(url, nullptr);

    /* Corrupt the block cache: remove one stored block that is not the
     * descriptor block. The load server uses a fresh tuple cache, so the
     * victim tuple can only resolve out of the block cache — where the block
     * is now missing. */
    off_url_t* parsed = off_url_parse(url);
    ASSERT_NE(parsed, nullptr);
    ASSERT_NE(parsed->descriptor_hash, nullptr);
    index_entry_vec_t* entries = index_to_array(bc->index);
    ASSERT_NE(entries, nullptr);
    buffer_t* victim_hash = NULL;
    for (int index = 0; index < entries->length; index++) {
        index_entry_t* entry = entries->data[index];
        if (buffer_compare(entry->hash, parsed->descriptor_hash) != 0) {
            victim_hash = entry->hash;
            break;
        }
    }
    ASSERT_NE(victim_hash, nullptr) << "no data block found in the cache index";
    buffer_t* victim_copy = buffer_copy(victim_hash);
    ASSERT_NE(victim_copy, nullptr);
    block_cache_remove(bc, victim_copy, NULL);
    off_url_destroy(parsed);
    scheduler_pool_wait_for_idle(pool);

    char request_url[4096];
    snprintf(request_url, sizeof(request_url), "%s?load=1", url);

    /* Generous timeout: the load resolves two tuples then runs to EOF. A
     * hang here (no EOF) is exactly the load-stream termination bug. */
    char* response = (char*)malloc(16384);
    memset(response, 0, 16384);
    int received = _get_load(request_url, response, 16384, 15000);
    ASSERT_GT(received, 0) << "partial load never terminated (no EOF)";

    EXPECT_NE(strstr(response, "application/x-ndjson"), nullptr);
    char* body_start = strstr(response, "\r\n\r\n");
    ASSERT_NE(body_start, nullptr);
    body_start += 4;

    const char* terminal = strstr(body_start, "{\"status\":");
    ASSERT_NE(terminal, nullptr) << "terminal ndjson line missing";
    EXPECT_NE(strstr(terminal, "\"status\":\"partial\""), nullptr);
    EXPECT_NE(strstr(terminal, "\"tuples_loaded\":2"), nullptr);
    EXPECT_NE(strstr(terminal, "\"tuples_total\":3"), nullptr);
    EXPECT_EQ(strstr(terminal + 1, "{\"status\":"), nullptr);

    free(response);
    buffer_destroy(victim_copy);
    free(url);
}

/* A descriptor hash that is not in the cache (and no network is configured)
 * must fail the load cleanly: the response still terminates at EOF with a
 * "failed" terminal line. */
TEST_F(TestOffRoutesLoad, LoadGarbageDescriptorFails) {
    /* base58 of 32 bytes of 0x01 — valid base58, parseable OFF URL. */
    const char* garbage_hash = "4vJ9JU1bJJE96FWSJKvHsmmFADCg4gpZQff4P3bkLKi";

    char request_url[4096];
    snprintf(request_url, sizeof(request_url),
        "/offsystem/v3/standard/100/%s/%s/load_ghost.bin?load=1",
        garbage_hash, garbage_hash);

    char response[8192];
    memset(response, 0, sizeof(response));
    int received = _get_load(request_url, response, sizeof(response), 5000);
    ASSERT_GT(received, 0) << "failed load never terminated (no EOF)";

    char* body_start = strstr(response, "\r\n\r\n");
    ASSERT_NE(body_start, nullptr);
    body_start += 4;

    const char* terminal = strstr(body_start, "{\"status\":");
    ASSERT_NE(terminal, nullptr) << "terminal ndjson line missing";
    EXPECT_NE(strstr(terminal, "\"status\":\"failed\""), nullptr);
    EXPECT_NE(strstr(terminal, "\"tuples_loaded\":0"), nullptr);
    EXPECT_EQ(strstr(terminal + 1, "{\"status\":"), nullptr);
}

/* A directory URL under ?load=1 is rejected with 400 — v1 parity with the
 * socket LOAD handler (directories resolve in HTTP land, v1 cannot load
 * them). */
TEST_F(TestOffRoutesLoad, LoadDirectoryOriRejected) {
    char request_url[4096];
    snprintf(request_url, sizeof(request_url),
        "/offsystem/v3/offsystem/directory/100/%s/%s/dir.ofd?load=1",
        "2yJAEiVufYkKVW8yAtavemMgAyvFRbeYFyVnEcUMcVUP",
        "2yJAEiVufYkKVW8yAtavemMgAyvFRbeYFyVnEcUMcVUP");

    platform_socket_t* sock = _connect_with_retry(load_port);
    ASSERT_NE(sock, nullptr);
    char get_request[4096];
    int get_len = snprintf(get_request, sizeof(get_request),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", request_url);
    /* The 400 rejection is a plain Content-Length response (keep-alive), so
       read it with the CL-aware helper rather than waiting for EOF. */
    char response[4096];
    memset(response, 0, sizeof(response));
    ASSERT_EQ(_send_and_recv(sock, get_request, (size_t)get_len,
                             response, sizeof(response), 5000), 0);
    platform_socket_destroy(sock);
    EXPECT_NE(strstr(response, "HTTP/1.1 400"), nullptr);
    EXPECT_NE(strstr(response, "Load requires a file ORI, not a directory"), nullptr);
}

} // namespace off_routes_load_test