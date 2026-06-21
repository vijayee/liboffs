#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/ClientAPI/TCP/tcp_transport.h"
#include "../src/ClientAPI/health_handler.h"
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
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_socket.h"
#include <string.h>
#include <stdlib.h>

/* usleep is POSIX-only; platform_sleep_ms is the cross-platform equivalent.
 * Call sites pass microsecond values (e.g. 10000 == 10ms), so divide by 1000. */
#define platform_usleep(us) platform_sleep_ms((us) / 1000)
}

namespace tcp_transport_test {

static uint16_t _next_port = 29080;

static platform_socket_t* _connect_tcp(const char* host, uint16_t port) {
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
    if (sock == NULL) return NULL;
    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    if (platform_address_parse(&addr, host, port) != 0) {
        platform_socket_destroy(sock);
        return NULL;
    }
    if (platform_socket_connect(sock, &addr) != 0) {
        platform_socket_destroy(sock);
        return NULL;
    }
    platform_socket_set_nonblocking(sock);
    return sock;
}

static platform_socket_t* _connect_with_retry(const char* host, uint16_t port, int max_attempts = 50) {
    for (int attempts = 0; attempts < max_attempts; attempts++) {
        platform_usleep(10000);
        platform_socket_t* sock = _connect_tcp(host, port);
        if (sock != NULL) return sock;
    }
    return NULL;
}

static int _send_frame(platform_socket_t* sock, cbor_item_t* frame) {
    unsigned char* cbor_buf = NULL;
    size_t cbor_len = 0;
    cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
    cbor_decref(&frame);
    if (cbor_buf == NULL) return -1;

    size_t framed_len;
    uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return -1;

    /* MSG_NOSIGNAL is POSIX-only (Windows has no SIGPIPE); platform_socket_send
     * takes no flags. Loop to handle partial sends on a nonblocking socket. */
    size_t sent_total = 0;
    for (int attempts = 0; attempts < 1000 && sent_total < framed_len; attempts++) {
        ssize_t sent = platform_socket_send(sock, framed + sent_total,
                                             framed_len - sent_total);
        if (sent > 0) {
            sent_total += (size_t)sent;
        } else if (sent == 0) {
            break;
        } else {
            platform_sleep_ms(10);
        }
    }
    int rc = (sent_total == framed_len) ? 0 : -1;
    free(framed);
    return rc;
}

static cbor_item_t* _recv_frame(platform_socket_t* sock, stream_framer_t* framer, int timeout_ms = 10000) {
    uint8_t buf[65536];
    for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
        /* Check framer for buffered frames before reading the socket. */
        size_t frame_len;
        uint8_t* frame_data = stream_framer_next(framer, &frame_len);
        if (frame_data != NULL) {
            struct cbor_load_result load_result;
            cbor_item_t* item = cbor_load(frame_data, frame_len, &load_result);
            free(frame_data);
            if (item != NULL && load_result.error.code == CBOR_ERR_NONE) {
                return item;
            }
            if (item != NULL) cbor_decref(&item);
        }

        ssize_t received = platform_socket_recv(sock, buf, sizeof(buf));
        if (received > 0) {
            stream_framer_feed(framer, buf, (size_t)received);
        } else if (received == 0) {
            /* Peer closed the connection. */
            return nullptr;
        } else {
            /* EWOULDBLOCK on a nonblocking socket: back off and retry. */
            platform_sleep_ms(10);
        }
    }
    return nullptr;
}

class TestTcpTransport : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    tcp_transport_t* transport;
    char* cache_dir;
    uint16_t port;

    void SetUp() override {
        port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create(pool);

        char dir_template[] = "/tmp/test_tcp_transport_XXXXXX";
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

        transport = tcp_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, NULL, NULL);
        for (int retry = 0; transport == nullptr && retry < 10; retry++) {
            port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
            transport = tcp_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, NULL, NULL);
        }
        ASSERT_NE(transport, nullptr);
        tcp_transport_start(transport);
    }

    void TearDown() override {
        if (transport != nullptr) {
            tcp_transport_stop(transport);
        }
        scheduler_pool_wait_for_idle(pool);
        scheduler_pool_stop(pool);
        timer_actor_destroy(timer);
        ofd_cache_destroy(ofd_cache);
        tuple_cache_destroy(tc);
        block_cache_destroy(bc);
        if (transport != nullptr) {
            tcp_transport_destroy(transport);
        }
        scheduler_pool_destroy(pool);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

TEST_F(TestTcpTransport, ConnectAndClose) {
    platform_socket_t* sock = _connect_with_retry("127.0.0.1", port);
    ASSERT_NE(sock, nullptr);
    platform_socket_destroy(sock);
}

TEST_F(TestTcpTransport, PutSmallData) {
    platform_socket_t* sock = _connect_with_retry("127.0.0.1", port);
    ASSERT_NE(sock, nullptr);

    const char* content_type = "application/octet-stream";
    const char* file_name = "test_tcp_file.bin";
    const uint8_t data[] = "hello tcp world";

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
    ASSERT_EQ(_send_frame(sock,frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock,framer);
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
    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

TEST_F(TestTcpTransport, GetInvalidOri) {
    platform_socket_t* sock = _connect_with_retry("127.0.0.1", port);
    ASSERT_NE(sock, nullptr);

    client_api_get_request_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.ori_string = (char*)"not-a-valid-ori";
    msg.has_range = 0;

    cbor_item_t* frame = client_api_get_request_encode(&msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock,frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock,framer);
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
    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

TEST_F(TestTcpTransport, StreamingPut) {
    platform_socket_t* sock = _connect_with_retry("127.0.0.1", port);
    ASSERT_NE(sock, nullptr);

    const char* content_type = "application/octet-stream";
    const char* file_name = "stream_tcp_test.bin";
    const uint8_t data[] = "streaming tcp data chunk";

    client_api_put_request_t put_req;
    memset(&put_req, 0, sizeof(put_req));
    put_req.content_type = (char*)content_type;
    put_req.file_name = (char*)file_name;
    put_req.stream_length = sizeof(data) - 1;
    put_req.server_address = NULL;
    put_req.data = NULL;
    put_req.data_size = 0;

    cbor_item_t* frame = client_api_put_request_encode(&put_req);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock,frame), 0);

    client_api_put_data_t data_msg;
    memset(&data_msg, 0, sizeof(data_msg));
    data_msg.data = (uint8_t*)data;
    data_msg.data_size = sizeof(data) - 1;

    frame = client_api_put_data_encode(&data_msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock,frame), 0);

    frame = client_api_put_end_encode();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock,frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock,framer);
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
    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

TEST_F(TestTcpTransport, PutAndGetRoundTrip) {
    platform_socket_t* sock = _connect_with_retry("127.0.0.1", port);
    ASSERT_NE(sock, nullptr);

    const char* content_type = "application/octet-stream";
    const char* file_name = "roundtrip_tcp.bin";
    const uint8_t data[] = "tcp round trip test data";

    client_api_put_request_t put_req;
    memset(&put_req, 0, sizeof(put_req));
    put_req.content_type = (char*)content_type;
    put_req.file_name = (char*)file_name;
    put_req.stream_length = sizeof(data) - 1;
    put_req.server_address = NULL;
    put_req.data = (uint8_t*)data;
    put_req.data_size = sizeof(data) - 1;

    cbor_item_t* frame = client_api_put_request_encode(&put_req);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock,frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock,framer);
    ASSERT_NE(response, nullptr);

    uint8_t type = client_api_wire_get_type(response);
    ASSERT_EQ(type, CLIENT_API_PUT_RESPONSE);

    client_api_put_response_t put_resp;
    memset(&put_resp, 0, sizeof(put_resp));
    ASSERT_EQ(client_api_put_response_decode(response, &put_resp), 0);
    ASSERT_NE(put_resp.ori_string, nullptr);
    char* ori_string = strdup(put_resp.ori_string);
    client_api_put_response_destroy(&put_resp);
    cbor_decref(&response);

    /* GET */
    client_api_get_request_t get_req;
    memset(&get_req, 0, sizeof(get_req));
    get_req.ori_string = ori_string;
    get_req.has_range = 0;

    frame = client_api_get_request_encode(&get_req);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock,frame), 0);
    free(ori_string);

    /* Expect GET_RESPONSE_START */
    response = _recv_frame(sock,framer);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_GET_RESPONSE_START);
    cbor_decref(&response);

    /* Expect GET_DATA */
    response = _recv_frame(sock,framer);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    if (type == CLIENT_API_GET_DATA) {
        client_api_get_data_t get_data;
        memset(&get_data, 0, sizeof(get_data));
        int decode_result = client_api_get_data_decode(response, &get_data);
        EXPECT_EQ(decode_result, 0);
        if (decode_result == 0) {
            EXPECT_EQ(get_data.data_size, sizeof(data) - 1);
            EXPECT_EQ(memcmp(get_data.data, data, sizeof(data) - 1), 0);
            client_api_get_data_destroy(&get_data);
        }
    }
    cbor_decref(&response);

    /* Expect GET_END */
    response = _recv_frame(sock,framer);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_GET_END);
    cbor_decref(&response);

    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

/* Large TCP round trip: exercises the server tcp_connection send path for a
 * frame that does not fit in one send. On Windows IOCP, PD_EVENT_WRITE is
 * never delivered, so the server must flush partial sends via a bounded
 * blocking retry (see tcp_connection.c) — without it the GET_DATA frame
 * stalls and this test times out. */
TEST_F(TestTcpTransport, PutAndGetLargeData) {
    platform_socket_t* sock = _connect_with_retry("127.0.0.1", port);
    ASSERT_NE(sock, nullptr);

    const char* content_type = "application/octet-stream";
    const char* file_name = "large_tcp.bin";
    const size_t size = 1024 * 1024;  /* 1 MB — exceeds one send on loopback */
    uint8_t* data = (uint8_t*)malloc(size);
    ASSERT_NE(data, nullptr);
    for (size_t i = 0; i < size; i++) data[i] = (uint8_t)(i * 7 + 3);

    client_api_put_request_t put_req;
    memset(&put_req, 0, sizeof(put_req));
    put_req.content_type = (char*)content_type;
    put_req.file_name = (char*)file_name;
    put_req.stream_length = size;
    put_req.server_address = NULL;
    put_req.data = data;
    put_req.data_size = size;

    cbor_item_t* frame = client_api_put_request_encode(&put_req);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
    ASSERT_NE(response, nullptr);

    uint8_t type = client_api_wire_get_type(response);
    ASSERT_EQ(type, CLIENT_API_PUT_RESPONSE);

    client_api_put_response_t put_resp;
    memset(&put_resp, 0, sizeof(put_resp));
    ASSERT_EQ(client_api_put_response_decode(response, &put_resp), 0);
    ASSERT_NE(put_resp.ori_string, nullptr);
    char* ori_string = strdup(put_resp.ori_string);
    client_api_put_response_destroy(&put_resp);
    cbor_decref(&response);

    /* GET */
    client_api_get_request_t get_req;
    memset(&get_req, 0, sizeof(get_req));
    get_req.ori_string = ori_string;
    get_req.has_range = 0;

    frame = client_api_get_request_encode(&get_req);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock, frame), 0);
    free(ori_string);

    /* Expect GET_RESPONSE_START */
    response = _recv_frame(sock, framer, 30000);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_GET_RESPONSE_START);
    cbor_decref(&response);

    /* Accumulate GET_DATA frames until GET_END. The server streams large
     * payloads in block-sized chunks (multiple GET_DATA frames), not one
     * frame — each chunk must be fully flushed by the server's send path
     * (the partial-send retry on Windows IOCP) for the round trip to
     * complete. */
    uint8_t* received = (uint8_t*)malloc(size);
    ASSERT_NE(received, nullptr);
    size_t received_total = 0;
    int saw_end = 0;
    for (int chunks = 0; chunks < 256; chunks++) {
        response = _recv_frame(sock, framer, 30000);
        ASSERT_NE(response, nullptr);
        type = client_api_wire_get_type(response);
        if (type == CLIENT_API_GET_END) {
            saw_end = 1;
            cbor_decref(&response);
            break;
        }
        ASSERT_EQ(type, CLIENT_API_GET_DATA);
        client_api_get_data_t get_data;
        memset(&get_data, 0, sizeof(get_data));
        int decode_result = client_api_get_data_decode(response, &get_data);
        EXPECT_EQ(decode_result, 0);
        cbor_decref(&response);
        if (decode_result == 0) {
            ASSERT_LE(received_total + get_data.data_size, size);
            if (get_data.data != nullptr && get_data.data_size > 0) {
                memcpy(received + received_total, get_data.data, get_data.data_size);
            }
            received_total += get_data.data_size;
            client_api_get_data_destroy(&get_data);
        }
    }
    EXPECT_TRUE(saw_end);
    EXPECT_EQ(received_total, size);
    EXPECT_EQ(memcmp(received, data, size), 0);

    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
    free(received);
    free(data);
}

TEST_F(TestTcpTransport, HealthRequest) {
    uint8_t running = 1;
    uint8_t draining = 0;
    health_context_t health_ctx;
    memset(&health_ctx, 0, sizeof(health_ctx));
    health_ctx.running = &running;
    health_ctx.draining = &draining;

    tcp_transport_t* health_transport = tcp_transport_create(
        pool, bc, ofd_cache, tc, "127.0.0.1", port + 1, NULL, NULL, NULL, &health_ctx);
    ASSERT_NE(health_transport, nullptr);
    int tcp_port = port + 1;
    for (int retry = 0; health_transport == nullptr && retry < 10; retry++) {
        tcp_port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
        health_transport = tcp_transport_create(
            pool, bc, ofd_cache, tc, "127.0.0.1", tcp_port, NULL, NULL, NULL, &health_ctx);
    }
    ASSERT_NE(health_transport, nullptr);
    tcp_transport_start(health_transport);

    platform_socket_t* sock = _connect_with_retry("127.0.0.1", tcp_port);
    ASSERT_NE(sock, nullptr);

    cbor_item_t* health_req = client_api_health_request_encode();
    ASSERT_NE(health_req, nullptr);
    int send_ret = _send_frame(sock,health_req);
    ASSERT_EQ(send_ret, 0);

    stream_framer_t* framer = stream_framer_create();
    ASSERT_NE(framer, nullptr);

    cbor_item_t* response = _recv_frame(sock,framer);
    ASSERT_NE(response, nullptr);
    uint8_t type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_HEALTH_RESPONSE);

    client_api_health_response_t decoded;
    int decode_ret = client_api_health_response_decode(response, &decoded);
    ASSERT_EQ(decode_ret, 0);
    EXPECT_NE(strstr(decoded.json_data, "\"status\""), nullptr);
    EXPECT_NE(strstr(decoded.json_data, "\"running\""), nullptr);
    client_api_health_response_destroy(&decoded);
    cbor_decref(&response);

    stream_framer_destroy(framer);
    platform_socket_destroy(sock);

    tcp_transport_stop(health_transport);
    scheduler_pool_wait_for_idle(pool);
    tcp_transport_destroy(health_transport);
}

} // namespace tcp_transport_test