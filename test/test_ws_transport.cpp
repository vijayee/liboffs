#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/ClientAPI/WS/ws_transport.h"
#include "../src/ClientAPI/WS/ws_frame.h"
#include "../src/ClientAPI/client_api_wire.h"
#include "../src/Network/stream_framer.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
}

namespace ws_transport_test {

static uint16_t _next_port = 39080;

static int _connect_tcp(const char* host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int _connect_with_retry(const char* host, uint16_t port, int max_attempts = 50) {
    for (int attempts = 0; attempts < max_attempts; attempts++) {
        usleep(10000);
        int fd = _connect_tcp(host, port);
        if (fd >= 0) return fd;
    }
    return -1;
}

/* Perform WebSocket HTTP upgrade handshake */
static int _ws_upgrade(int fd) {
    const char* upgrade_request =
        "GET /offs HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ssize_t sent = send(fd, upgrade_request, strlen(upgrade_request), MSG_NOSIGNAL);
    if (sent != (ssize_t)strlen(upgrade_request)) return -1;

    char response[4096];
    memset(response, 0, sizeof(response));
    /* Wait for response with poll */
    for (int attempts = 0; attempts < 200; attempts++) {
        struct pollfd poll_fd;
        poll_fd.fd = fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        int poll_result = poll(&poll_fd, 1, 10);
        if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
            ssize_t received = recv(fd, response, sizeof(response) - 1, 0);
            if (received <= 0) return -1;
            response[received] = '\0';
            if (strstr(response, "101") != NULL) {
                return 0;
            }
        }
    }
    return -1;
}

/* Build a masked binary WebSocket frame (client-to-server, opcode 0x02) */
static uint8_t* _ws_build_frame(const uint8_t* payload, size_t payload_len, size_t* out_len) {
    size_t header_len;
    if (payload_len < 126) {
        header_len = 6;
    } else if (payload_len < 65536) {
        header_len = 8;
    } else {
        header_len = 14;
    }
    uint8_t* frame = (uint8_t*)malloc(header_len + payload_len);
    if (frame == NULL) return NULL;
    size_t pos = 0;
    frame[pos++] = 0x82; /* FIN + binary opcode */
    if (payload_len < 126) {
        frame[pos++] = 0x80 | (uint8_t)payload_len;
    } else if (payload_len < 65536) {
        frame[pos++] = 0x80 | 126;
        frame[pos++] = (payload_len >> 8) & 0xFF;
        frame[pos++] = payload_len & 0xFF;
    } else {
        frame[pos++] = 0x80 | 127;
        for (int i = 56; i >= 0; i -= 8) {
            frame[pos++] = (payload_len >> i) & 0xFF;
        }
    }
    /* Masking key: all zeros for test simplicity */
    frame[pos++] = 0x00;
    frame[pos++] = 0x00;
    frame[pos++] = 0x00;
    frame[pos++] = 0x00;
    memcpy(frame + pos, payload, payload_len);
    *out_len = header_len + payload_len;
    return frame;
}

/* Send a CBOR message as a masked binary WebSocket frame */
static int _ws_send_cbor(int fd, cbor_item_t* frame) {
    unsigned char* cbor_buf = NULL;
    size_t cbor_len = 0;
    cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
    cbor_decref(&frame);
    if (cbor_buf == NULL) return -1;

    size_t ws_len;
    uint8_t* ws_frame = _ws_build_frame(cbor_buf, cbor_len, &ws_len);
    free(cbor_buf);
    if (ws_frame == NULL) return -1;

    ssize_t sent = send(fd, ws_frame, ws_len, MSG_NOSIGNAL);
    free(ws_frame);
    return (sent == (ssize_t)ws_len) ? 0 : -1;
}

/* Receive a WebSocket frame from the server and parse out the CBOR payload.
 * Returns decoded cbor_item_t* or NULL. */
static cbor_item_t* _ws_recv_cbor(int fd, int timeout_ms = 3000) {
    uint8_t buf[65536];
    size_t buf_len = 0;

    for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
        if (buf_len > 0) {
            /* Try to parse a complete WS frame from buffer */
            if (buf_len < 2) goto need_more;

            uint8_t opcode = buf[0] & 0x0F;
            if (opcode != 0x02) {
                /* Not a binary frame — skip it or fail */
                /* For close/ping frames, handle differently */
                if (opcode == 0x08) return nullptr; /* close frame */
                goto need_more;
            }

            uint8_t len_byte = buf[1];
            size_t payload_offset;
            size_t payload_len;

            if (len_byte < 126) {
                payload_len = len_byte;
                payload_offset = 2;
            } else if (len_byte == 126) {
                if (buf_len < 4) goto need_more;
                payload_len = ((size_t)buf[2] << 8) | buf[3];
                payload_offset = 4;
            } else {
                if (buf_len < 10) goto need_more;
                payload_len = 0;
                for (int i = 4; i < 10; i++) {
                    payload_len = (payload_len << 8) | buf[i];
                }
                payload_offset = 10;
            }

            if (buf_len < payload_offset + payload_len) goto need_more;

            /* Server frames are unmasked */
            struct cbor_load_result load_result;
            cbor_item_t* item = cbor_load(buf + payload_offset, payload_len, &load_result);
            if (item != NULL && load_result.error.code == CBOR_ERR_NONE) {
                return item;
            }
            if (item != NULL) cbor_decref(&item);
            return nullptr;
        }

need_more:
        struct pollfd poll_fd;
        poll_fd.fd = fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        int poll_result = poll(&poll_fd, 1, 10);
        if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
            ssize_t received = recv(fd, buf + buf_len, sizeof(buf) - buf_len, 0);
            if (received <= 0) return nullptr;
            buf_len += (size_t)received;
        }
    }
    return nullptr;
}

class TestWsTransport : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    ws_transport_t* transport;
    char* cache_dir;
    uint16_t port;

    void SetUp() override {
        port = _next_port++;
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create();

        char dir_template[] = "/tmp/test_ws_transport_XXXXXX";
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

        transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, 0, NULL);
        ASSERT_NE(transport, nullptr);
        ws_transport_start(transport);
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

TEST_F(TestWsTransport, ConnectAndUpgrade) {
    int fd = _connect_with_retry("127.0.0.1", port);
    ASSERT_GE(fd, 0);

    int upgrade_result = _ws_upgrade(fd);
    EXPECT_EQ(upgrade_result, 0);

    close(fd);
}

TEST_F(TestWsTransport, PutSmallData) {
    int fd = _connect_with_retry("127.0.0.1", port);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(_ws_upgrade(fd), 0);

    const char* content_type = "application/octet-stream";
    const char* file_name = "test_ws_file.bin";
    const uint8_t data[] = "hello ws world";

    client_api_put_request_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.content_type = (char*)content_type;
    msg.file_name = (char*)file_name;
    msg.stream_length = sizeof(data) - 1;
    msg.server_address = NULL;
    msg.data = (uint8_t*)data;
    msg.data_size = sizeof(data) - 1;

    cbor_item_t* frame = client_api_put_request_encode(&msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_ws_send_cbor(fd, frame), 0);

    cbor_item_t* response = _ws_recv_cbor(fd);
    ASSERT_NE(response, nullptr);

    uint8_t type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_PUT_RESPONSE);

    if (type == CLIENT_API_PUT_RESPONSE) {
        client_api_put_response_t put_resp;
        memset(&put_resp, 0, sizeof(put_resp));
        int decode_result = client_api_put_response_decode(response, &put_resp);
        EXPECT_EQ(decode_result, 0);
        if (decode_result == 0) {
            EXPECT_NE(put_resp.ori_string, nullptr);
            client_api_put_response_destroy(&put_resp);
        }
    }
    cbor_decref(&response);
    close(fd);
}

TEST_F(TestWsTransport, GetInvalidOri) {
    int fd = _connect_with_retry("127.0.0.1", port);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(_ws_upgrade(fd), 0);

    client_api_get_request_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.ori_string = (char*)"not-a-valid-ori";
    msg.has_range = 0;

    cbor_item_t* frame = client_api_get_request_encode(&msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_ws_send_cbor(fd, frame), 0);

    cbor_item_t* response = _ws_recv_cbor(fd);
    ASSERT_NE(response, nullptr);

    uint8_t type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_ERROR);

    if (type == CLIENT_API_ERROR) {
        client_api_error_t err;
        memset(&err, 0, sizeof(err));
        int decode_result = client_api_error_decode(response, &err);
        EXPECT_EQ(decode_result, 0);
        if (decode_result == 0) {
            EXPECT_NE(err.status_code, CLIENT_API_STATUS_OK);
            client_api_error_destroy(&err);
        }
    }
    cbor_decref(&response);
    close(fd);
}

TEST(TestWsFrame, BuildMaskedRoundTrip) {
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    size_t frame_len;
    uint8_t* frame = ws_frame_build_masked(WS_OPCODE_BINARY, payload, sizeof(payload), &frame_len);
    ASSERT_NE(frame, nullptr);
    EXPECT_GT(frame_len, sizeof(payload));

    ws_frame_t parsed;
    size_t needed;
    ssize_t consumed = ws_frame_parse(frame, frame_len, &parsed, &needed);
    EXPECT_EQ(consumed, (ssize_t)frame_len);
    EXPECT_EQ(parsed.fin, 1);
    EXPECT_EQ(parsed.opcode, WS_OPCODE_BINARY);
    EXPECT_EQ(parsed.mask, 1);
    EXPECT_EQ(parsed.payload_len, sizeof(payload));
    ASSERT_NE(parsed.payload, nullptr);
    EXPECT_EQ(memcmp(parsed.payload, payload, sizeof(payload)), 0);

    free(frame);
    ws_frame_destroy(&parsed);
}

} // namespace ws_transport_test