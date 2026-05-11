#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/HTTP/off_routes.h"
#include "../src/HTTP/http_server.h"
#include "../src/HTTP/http_request.h"
#include "../src/HTTP/http_response.h"
#include "../src/OFFStreams/off_url.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/Buffer/buffer.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
}

namespace off_routes_test {

static uint16_t _next_port = 19080;

static int _connect_to_server(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int _send_and_recv(int fd, const char* request, size_t req_len,
                          char* response, size_t response_size, int timeout_ms) {
    ssize_t sent = send(fd, request, req_len, 0);
    if (sent != (ssize_t)req_len) return -1;

    size_t total_received = 0;
    for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
        struct pollfd poll_fd;
        poll_fd.fd = fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        int poll_result = poll(&poll_fd, 1, 10);
        if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
            ssize_t received = recv(fd, response + total_received,
                                    response_size - total_received - 1, 0);
            if (received > 0) {
                total_received += (size_t)received;
                response[total_received] = '\0';
                char* header_end = strstr(response, "\r\n\r\n");
                if (header_end != NULL) {
                    size_t header_len = (header_end - response) + 4;
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
                return total_received > 0 ? 0 : -1;
            }
        }
    }
    return total_received > 0 ? 0 : -1;
}

class TestOffRoutes : public testing::Test {
protected:
    scheduler_pool_t* pool;
    http_server_t* server;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    timer_actor_t* timer;
    uint16_t port;
    char* cache_dir;

    void SetUp() override {
        port = _next_port++;
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);

        char dir_template[] = "/tmp/test_off_routes_XXXXXX";
        cache_dir = mkdtemp(dir_template);
        cache_dir = strdup(cache_dir);

        timer = timer_actor_create();
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
        bc = block_cache_create(config, cache_dir, standard, timer);
        ofd_cache = ofd_cache_create(pool, bc, 300000);
        server = http_server_create(pool, "127.0.0.1", port);
    }

    void TearDown() override {
        if (server != NULL) {
            http_server_stop(server);
            http_server_destroy(server);
        }
        ofd_cache_destroy(ofd_cache);
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        scheduler_pool_stop(pool);
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

TEST_F(TestOffRoutes, PutMissingHeaders) {
    off_routes_register(server, pool, bc, ofd_cache);
    http_server_listen(server);

    int fd = -1;
    for (int attempts = 0; attempts < 50; attempts++) {
        usleep(10000);
        fd = _connect_to_server(port);
        if (fd >= 0) break;
    }
    ASSERT_GE(fd, 0);

    char response[4096];
    const char* request = "PUT /offsystem HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
    int result = _send_and_recv(fd, request, strlen(request), response, sizeof(response), 2000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(response, "400"), nullptr);

    close(fd);
}

TEST_F(TestOffRoutes, GetInvalidUrl) {
    off_routes_register(server, pool, bc, ofd_cache);
    http_server_listen(server);

    int fd = -1;
    for (int attempts = 0; attempts < 50; attempts++) {
        usleep(10000);
        fd = _connect_to_server(port);
        if (fd >= 0) break;
    }
    ASSERT_GE(fd, 0);

    char response[4096];
    const char* request = "GET /offsystem/v3/invalid HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int result = _send_and_recv(fd, request, strlen(request), response, sizeof(response), 2000);
    EXPECT_EQ(result, 0);
    // URL doesn't match OFF_GET_PATTERN, so server returns 404
    EXPECT_NE(strstr(response, "404"), nullptr);

    close(fd);
}

TEST_F(TestOffRoutes, DeleteInvalidUrl) {
    off_routes_register(server, pool, bc, ofd_cache);
    http_server_listen(server);

    int fd = -1;
    for (int attempts = 0; attempts < 50; attempts++) {
        usleep(10000);
        fd = _connect_to_server(port);
        if (fd >= 0) break;
    }
    ASSERT_GE(fd, 0);

    char response[4096];
    const char* request = "DELETE /offsystem/v3/invalid HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int result = _send_and_recv(fd, request, strlen(request), response, sizeof(response), 2000);
    EXPECT_EQ(result, 0);
    // URL doesn't match OFF_GET_PATTERN, so server returns 404
    EXPECT_NE(strstr(response, "404"), nullptr);

    close(fd);
}

TEST_F(TestOffRoutes, PutAndGetRoundTrip) {
    off_routes_register(server, pool, bc, ofd_cache);
    http_server_listen(server);

    int fd = -1;
    for (int attempts = 0; attempts < 50; attempts++) {
        usleep(10000);
        fd = _connect_to_server(port);
        if (fd >= 0) break;
    }
    ASSERT_GE(fd, 0);

    // PUT request with a small body
    const char* body = "Hello OFF System!";
    size_t body_len = strlen(body);
    char put_request[4096];
    int put_len = snprintf(put_request, sizeof(put_request),
        "PUT /offsystem HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "type: application/octet-stream\r\n"
        "file-name: test.txt\r\n"
        "stream-length: %zu\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        body_len, body_len, body);

    char put_response[8192];
    memset(put_response, 0, sizeof(put_response));
    int result = _send_and_recv(fd, put_request, (size_t)put_len,
                                put_response, sizeof(put_response), 5000);
    EXPECT_EQ(result, 0);
    EXPECT_NE(strstr(put_response, "200"), nullptr);

    // Extract the OFF URL from the response body
    char* header_end = strstr(put_response, "\r\n\r\n");
    ASSERT_NE(header_end, nullptr);
    char* put_body = header_end + 4;
    // Strip trailing whitespace from the body (URL)
    size_t url_len = strlen(put_body);
    while (url_len > 0 && (put_body[url_len-1] == '\r' || put_body[url_len-1] == '\n' || put_body[url_len-1] == ' '))
        put_body[--url_len] = '\0';
    // The body should contain /offsystem/v3/...
    EXPECT_NE(strstr(put_body, "/offsystem/v3/"), nullptr);

    close(fd);

    // GET the content back using the returned URL
    fd = -1;
    for (int attempts = 0; attempts < 50; attempts++) {
        usleep(10000);
        fd = _connect_to_server(port);
        if (fd >= 0) break;
    }
    ASSERT_GE(fd, 0);

    char get_request[4096];
    int get_len = snprintf(get_request, sizeof(get_request),
        "GET %s HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n",
        put_body);

    char get_response[8192];
    result = _send_and_recv(fd, get_request, (size_t)get_len,
                            get_response, sizeof(get_response), 5000);
    EXPECT_EQ(result, 0);

    close(fd);
}

} // namespace off_routes_test