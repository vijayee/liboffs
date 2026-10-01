//
// Route tests for POST /offsystem/cache/gc — keep-list garbage collection over
// the HTTP API. Uses the TestOffRoutes fixture pattern (raw socket HTTP/1.1).
//
// Auth is deliberately NOT covered here: GC route auth is middleware-level
// (off_routes_register wires auth_middleware only when config->api_key_hash is
// set) and that shared middleware is covered by its own suite. These tests
// register off_routes with a NULL config, as the existing route tests do.
//
// Cycle (GC_LINE_CYCLE) is equally out of scope here — it is documented in
// test_gc_collect.cpp's header comment.
//
#include <gtest/gtest.h>
#include <cstring>
#include <string>
extern "C" {
#include "../src/ClientAPI/HTTP/off_routes.h"
#include "../src/ClientAPI/HTTP/http_server.h"
#include "../src/ClientAPI/HTTP/http_request.h"
#include "../src/ClientAPI/HTTP/http_response.h"
#include "../src/ClientAPI/client_api_wire.h"
#include "../src/OFFStreams/off_url.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/ofd.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/block_gc.h"
#include "../src/Buffer/buffer.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Util/allocator.h"
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_socket.h"
#include <string.h>
#include <stdlib.h>

/* usleep is POSIX-only; platform_sleep_ms is the cross-platform equivalent.
 * Call sites pass microsecond values (e.g. 10000 == 10ms), so divide by 1000. */
#define platform_usleep(us) platform_sleep_ms((us) / 1000)
}

namespace off_routes_gc_test {

/* 20080 (not 19080): this TU and test_off_routes.cpp both run inside the one
   testliboffs process; overlapping listen ports would make whichever suite
   binds second fail to listen and its connects would hit the other suite's
   server. */
static uint16_t _next_port = 20080;

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

/* PUT /offsystem a small octet-stream body and pull the returned OFF URL out
   of the response body (trailing CRLF/SP stripped). Returns false on any
   step that leaves no usable URL. */
static int _put_off_url(platform_socket_t* sock, const char* body, char* url_out,
                        size_t url_out_size) {
    size_t body_len = strlen(body);
    char* request = (char*)get_memory(1024 + body_len);
    int req_len = snprintf(request, 1024 + body_len,
        "PUT /offsystem HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "type: application/octet-stream\r\n"
        "file-name: gc_route.bin\r\n"
        "stream-length: %zu\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        body_len, body_len, body);

    char* response = (char*)get_clear_memory(8192);
    int result = _send_and_recv(sock, request, (size_t)req_len,
                                response, 8192, 5000);
    free(request);
    if (result != 0) {
        free(response);
        return -1;
    }
    char* header_end = strstr(response, "\r\n\r\n");
    if (header_end == NULL) {
        free(response);
        return -1;
    }
    const char* put_body = header_end + 4;
    size_t url_len = strlen(put_body);
    if (url_len == 0 || url_len >= url_out_size) {
        free(response);
        return -1;
    }
    while (url_len > 0 && (put_body[url_len - 1] == '\r' || put_body[url_len - 1] == '\n' ||
                           put_body[url_len - 1] == ' ')) {
        url_len--;
    }
    memcpy(url_out, put_body, url_len);
    url_out[url_len] = '\0';
    free(response);
    return 0;
}

class TestOffRoutesGc : public testing::Test {
protected:
    scheduler_pool_t* pool;
    http_server_t* server;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    timer_actor_t* timer;
    uint16_t port;
    char* cache_dir;

    void SetUp() override {
        port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);

        char dir_template[] = "/tmp/test_off_routes_gc_XXXXXX";
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
        tc = tuple_cache_create(100, pool);
        server = http_server_create(pool, "127.0.0.1", port);
    }

    void TearDown() override {
        if (server != NULL) {
            http_server_stop(server);
        }
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        if (server != NULL) {
            http_server_destroy(server);
        }
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }
    /* Connect with retries until the listener accepts. */
    platform_socket_t* wait_connect() {
        platform_socket_t* sock = NULL;
        for (int attempts = 0; attempts < 50; attempts++) {
            platform_usleep(10000);
            sock = _connect_to_server(port);
            if (sock != NULL) break;
        }
        return sock;
    }
};

/* Empty request body is rejected at the handler with 400 before any
   orchestration is created (no async response, no keep-list). */
TEST_F(TestOffRoutesGc, EmptyBodyIsRejectedWith400) {
    off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL, NULL, NULL);
    http_server_listen(server);

    platform_socket_t* sock = wait_connect();
    ASSERT_NE(sock, nullptr);

    const char* request = "POST /offsystem/cache/gc HTTP/1.1\r\nHost: localhost\r\n"
                          "Content-Length: 0\r\n\r\n";
    char response[4096];
    int result = _send_and_recv(sock, request, strlen(request), response,
                                sizeof(response), 3000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(response, "400"), nullptr);
    EXPECT_NE(strstr(response, "missing keep-list body"), nullptr);

    platform_socket_destroy(sock);
}

/* A body larger than the wire pair's 4 MiB cap is rejected with 413; the
   keep-list text never reaches the orchestrator. */
TEST_F(TestOffRoutesGc, OversizedBodyIsRejectedWith413) {
    off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL, NULL, NULL);
    http_server_listen(server);

    platform_socket_t* sock = wait_connect();
    ASSERT_NE(sock, nullptr);

    /* CLIENT_API_GC_MAX_URLS_TEXT + 1 bytes of 'u'. */
    std::string body(CLIENT_API_GC_MAX_URLS_TEXT + 1, 'u');
    char head[256];
    int head_len = snprintf(head, sizeof(head),
        "POST /offsystem/cache/gc HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: %zu\r\n\r\n", body.size());
    std::string request(head, (size_t)head_len);
    request.append(body);

    char response[4096];
    int result = _send_and_recv(sock, request.data(), request.size(), response,
                                sizeof(response), 20000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(response, "413"), nullptr);
    EXPECT_NE(strstr(response, "keep-list body too large"), nullptr);

    platform_socket_destroy(sock);
}

/* Full sweep round trip: two representations uploaded, only one in the
   keep-list. The decoy representation's blocks are deleted, the kept
   representation's are spared, and the JSON carries the tallies. Decoy loss
   is verified with a 404 GET, spared survival with a 200 GET. */
TEST_F(TestOffRoutesGc, SweepRoundTripKeepsKeepDeletesDecoy) {
    off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL, NULL, NULL);
    http_server_listen(server);

    platform_socket_t* sock = wait_connect();
    ASSERT_NE(sock, nullptr);

    char keep_url[2048];
    char decoy_url[2048];
    ASSERT_EQ(0, _put_off_url(sock, "Hello OFF GC keep!", keep_url, sizeof(keep_url)));
    ASSERT_NE(strstr(keep_url, "/offsystem/v3/"), nullptr);
    /* One request per socket, as the repo's route tests do. */
    platform_socket_destroy(sock);
    sock = wait_connect();
    ASSERT_NE(sock, nullptr);
    ASSERT_EQ(0, _put_off_url(sock, "Hello OFF GC decoy!", decoy_url, sizeof(decoy_url)));
    ASSERT_NE(strstr(decoy_url, "/offsystem/v3/"), nullptr);

    /* The sweep is asynchronous (piped response driven by the orchestrator's
       BLOCK_GC_RESULT completion): a keep-list body of one URL deletes every
       decoy block. One tuple is 2 random + 1 off + 1 descriptor = 4 blocks,
       so both tallies must be exactly 4. */
    char* request = (char*)get_memory(2048 + strlen(keep_url));
    int req_len = snprintf(request, 2048 + strlen(keep_url),
        "POST /offsystem/cache/gc HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: %zu\r\n\r\n"
        "%s",
        strlen(keep_url), keep_url);
    char response[8192];
    int result = _send_and_recv(sock, request, (size_t)req_len, response,
                                sizeof(response), 10000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(response, "200"), nullptr);
    EXPECT_NE(strstr(response,
        "{\"status\":\"ok\",\"urls_request\":1,\"urls_collected\":1,"
        "\"blocks_deleted\":4,\"blocks_kept\":4,"), nullptr);
    EXPECT_NE(strstr(response, "\"skipped\":{\"pinned\":0,\"ephemeral_claimed\":0}"),
              nullptr);
    EXPECT_NE(strstr(response, "\"failed\":[]"), nullptr);
    EXPECT_NE(strstr(response, "\"defrag\":{\"applied\":0,\"result\":0,\"sections\":0,"
                                "\"blocks_relocated\":0}"), nullptr);
    free(request);
    platform_socket_destroy(sock);

    /* Decoy blocks are gone: GET of the decoy URL misses (the GET route
       answers with a failure status rather than body data). */
    sock = wait_connect();
    ASSERT_NE(sock, nullptr);
    char get_request[8192];
    int get_len = snprintf(get_request, sizeof(get_request),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", decoy_url);
    char get_response[8192];
    result = _send_and_recv(sock, get_request, (size_t)get_len, get_response,
                            sizeof(get_response), 5000);
    EXPECT_EQ(result, 0);
    /* The decoy body is gone from the cache — whatever failure status the GET
       route picks for a missing descriptor, the content must not come back. */
    EXPECT_EQ(strstr(get_response, "Hello OFF GC decoy!"), nullptr);
    platform_socket_destroy(sock);

    /* Kept content still resolves: the spare direction of the sweep. */
    sock = wait_connect();
    ASSERT_NE(sock, nullptr);
    get_len = snprintf(get_request, sizeof(get_request),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", keep_url);
    result = _send_and_recv(sock, get_request, (size_t)get_len, get_response,
                            sizeof(get_response), 5000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(get_response, "Hello OFF GC keep!"), nullptr);
    platform_socket_destroy(sock);
}

/* Every line unresolvable arms the empty-keep refusal: status 1, nothing
   deleted, and the route answers 500 with the "error" JSON plus the failed
   rows naming the lines. */
TEST_F(TestOffRoutesGc, AllMalformedRefusalAnswers500) {
    off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL, NULL, NULL);
    http_server_listen(server);

    platform_socket_t* sock = wait_connect();
    ASSERT_NE(sock, nullptr);

    /* Preload one representation so a botched sweep would have had something
       to delete — the refusal must leave it untouched. */
    char keep_url[2048];
    ASSERT_EQ(0, _put_off_url(sock, "refusal-survivor", keep_url, sizeof(keep_url)));
    platform_socket_destroy(sock);
    sock = wait_connect();
    ASSERT_NE(sock, nullptr);

    const char* body = "not-a-url\nhttp://localhost/nothing\n";
    char request[1024];
    int req_len = snprintf(request, sizeof(request),
        "POST /offsystem/cache/gc HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: %zu\r\n\r\n"
        "%s", strlen(body), body);
    char response[8192];
    int result = _send_and_recv(sock, request, (size_t)req_len, response,
                                sizeof(response), 10000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(response, "500"), nullptr);
    EXPECT_NE(strstr(response, "{\"status\":\"error\",\"urls_request\":2,"
                               "\"urls_collected\":0,\"blocks_deleted\":0"), nullptr);
    /* Two failed rows, both malformed_url: the reason name and the raw line
       text pass through the JSON string writer verbatim (no escapes needed). */
    EXPECT_NE(strstr(response, "\"reason\":\"malformed_url\""), nullptr);
    EXPECT_NE(strstr(response, "\"text\":\"not-a-url\""), nullptr);
    EXPECT_NE(strstr(response, "\"http://localhost/nothing\""), nullptr);
    EXPECT_NE(strstr(response, "\"defrag\":{\"applied\":0"), nullptr);

    /* The refusal deleted nothing — the preloaded content still resolves. */
    platform_socket_destroy(sock);
    sock = wait_connect();
    ASSERT_NE(sock, nullptr);
    char get_request[8192];
    int get_len = snprintf(get_request, sizeof(get_request),
        "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", keep_url);
    char get_response[8192];
    result = _send_and_recv(sock, get_request, (size_t)get_len, get_response,
                            sizeof(get_response), 5000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(get_response, "refusal-survivor"), nullptr);
    platform_socket_destroy(sock);
}

/* The defrag query flag chains the defragment pass; the JSON carries its
   summary alongside the sweep tallies. */
TEST_F(TestOffRoutesGc, DefragQueryChainsThePass) {
    off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL, NULL, NULL);
    http_server_listen(server);

    platform_socket_t* sock = wait_connect();
    ASSERT_NE(sock, nullptr);

    char keep_url[2048];
    char decoy_url[2048];
    ASSERT_EQ(0, _put_off_url(sock, "defrag keep content", keep_url, sizeof(keep_url)));
    ASSERT_EQ(0, _put_off_url(sock, "defrag decoy content", decoy_url, sizeof(decoy_url)));

    char* request = (char*)get_memory(2048 + strlen(keep_url));
    int req_len = snprintf(request, 2048 + strlen(keep_url),
        "POST /offsystem/cache/gc?defrag=1&threshold=0.9 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n\r\n"
        "%s",
        strlen(keep_url), keep_url);
    char response[8192];
    int result = _send_and_recv(sock, request, (size_t)req_len, response,
                                sizeof(response), 10000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(response, "200"), nullptr);
    EXPECT_NE(strstr(response, "{\"status\":\"ok\""), nullptr);
    /* The chained pass is applied and reports a zero result; section counts
       for this tiny cache are zero-hole-free (all data fits one section). */
    EXPECT_NE(strstr(response,
        "\"defrag\":{\"applied\":1,\"result\":0,\"sections\":0,"
        "\"blocks_relocated\":0}"), nullptr);
    free(request);
    platform_socket_destroy(sock);
}

} /* namespace off_routes_gc_test */