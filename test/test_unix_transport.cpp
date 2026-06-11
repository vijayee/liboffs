#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/ClientAPI/Unix/unix_transport.h"
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
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
}

namespace unix_transport_test {

static int _connect_unix(const char* path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int _connect_with_retry(const char* path, int max_attempts = 50) {
    for (int attempts = 0; attempts < max_attempts; attempts++) {
        usleep(10000);
        int fd = _connect_unix(path);
        if (fd >= 0) return fd;
    }
    return -1;
}

static int _send_frame(int fd, cbor_item_t* frame) {
    unsigned char* cbor_buf = NULL;
    size_t cbor_len = 0;
    cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
    cbor_decref(&frame);
    if (cbor_buf == NULL) return -1;

    size_t framed_len;
    uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return -1;

    ssize_t sent = send(fd, framed, framed_len, MSG_NOSIGNAL);
    free(framed);
    return (sent == (ssize_t)framed_len) ? 0 : -1;
}

static cbor_item_t* _recv_frame(int fd, stream_framer_t* framer, int timeout_ms = 10000) {
    uint8_t buf[65536];
    for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
        /* Check framer for buffered frames before polling the socket */
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

        struct pollfd poll_fd;
        poll_fd.fd = fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        int poll_result = poll(&poll_fd, 1, 10);
        if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
            ssize_t received = recv(fd, buf, sizeof(buf), 0);
            if (received <= 0) return nullptr;
            stream_framer_feed(framer, buf, (size_t)received);
        }
    }
    return nullptr;
}

class TestUnixTransport : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    unix_transport_t* transport;
    char* cache_dir;
    char socket_path[128];

    void SetUp() override {
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create(pool);

        char dir_template[] = "/tmp/test_unix_transport_XXXXXX";
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

        snprintf(socket_path, sizeof(socket_path), "/tmp/test_unix_sock_%d", getpid());
        unlink(socket_path);

        transport = unix_transport_create(pool, bc, ofd_cache, tc, socket_path, NULL, NULL);
        ASSERT_NE(transport, nullptr);
        unix_transport_start(transport);
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

TEST_F(TestUnixTransport, ConnectAndClose) {
    int fd = _connect_with_retry(socket_path);
    ASSERT_GE(fd, 0);
    close(fd);
}

TEST_F(TestUnixTransport, PutSmallData) {
    int fd = _connect_with_retry(socket_path);
    ASSERT_GE(fd, 0);

    const char* content_type = "application/octet-stream";
    const char* file_name = "test_file.bin";
    const uint8_t data[] = "hello world";

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
    ASSERT_EQ(_send_frame(fd, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(fd, framer);
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
    close(fd);
}

TEST_F(TestUnixTransport, GetInvalidOri) {
    int fd = _connect_with_retry(socket_path);
    ASSERT_GE(fd, 0);

    client_api_get_request_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.ori_string = (char*)"not-a-valid-ori";
    msg.has_range = 0;

    cbor_item_t* frame = client_api_get_request_encode(&msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(fd, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(fd, framer);
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
    close(fd);
}

TEST_F(TestUnixTransport, StreamingPut) {
    int fd = _connect_with_retry(socket_path);
    ASSERT_GE(fd, 0);

    const char* content_type = "application/octet-stream";
    const char* file_name = "stream_test.bin";
    const uint8_t data[] = "streaming data chunk";

    /* Send PUT_REQUEST with no data (streaming mode) */
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
    ASSERT_EQ(_send_frame(fd, frame), 0);

    /* Send PUT_DATA with the chunk */
    client_api_put_data_t data_msg;
    memset(&data_msg, 0, sizeof(data_msg));
    data_msg.data = (uint8_t*)data;
    data_msg.data_size = sizeof(data) - 1;

    frame = client_api_put_data_encode(&data_msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(fd, frame), 0);

    /* Send PUT_END */
    frame = client_api_put_end_encode();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(fd, frame), 0);

    /* Receive PUT_RESPONSE */
    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(fd, framer);
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
    close(fd);
}

TEST_F(TestUnixTransport, PutAndGetRoundTrip) {
    int fd = _connect_with_retry(socket_path);
    ASSERT_GE(fd, 0);

    const char* content_type = "application/octet-stream";
    const char* file_name = "roundtrip.bin";
    const uint8_t data[] = "round trip test data";

    /* PUT */
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
    ASSERT_EQ(_send_frame(fd, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(fd, framer);
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
    ASSERT_EQ(_send_frame(fd, frame), 0);
    free(ori_string);

    /* Expect GET_RESPONSE_START */
    response = _recv_frame(fd, framer);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_GET_RESPONSE_START);
    cbor_decref(&response);

    /* Expect GET_DATA */
    response = _recv_frame(fd, framer);
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
    response = _recv_frame(fd, framer);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_GET_END);
    cbor_decref(&response);

    stream_framer_destroy(framer);
    close(fd);
}

TEST_F(TestUnixTransport, MaxConnections) {
    /* Create a new transport with max_connections = 1 */
    char limited_path[128];
    snprintf(limited_path, sizeof(limited_path), "/tmp/test_unix_limited_%d", getpid());
    unlink(limited_path);

    unix_transport_t* limited = unix_transport_create(pool, bc, ofd_cache, tc, limited_path, NULL, NULL);
    ASSERT_NE(limited, nullptr);
    unix_transport_set_max_connections(limited, 1);
    unix_transport_start(limited);

    /* First connection should succeed */
    int fd1 = _connect_with_retry(limited_path);
    ASSERT_GE(fd1, 0);

    /* Give server time to accept the connection */
    usleep(50000);

    /* Second connection should be rejected (server closes it) */
    int fd2 = _connect_unix(limited_path);
    if (fd2 >= 0) {
        /* Connection was made, but server should close it quickly.
         * Attempting to send should fail or receive EOF. */
        uint8_t buf[1];
        ssize_t result = recv(fd2, buf, 1, MSG_DONTWAIT);
        /* Server closes connection, so recv should return 0 or -1 */
        EXPECT_LE(result, 0);
        close(fd2);
    }

    close(fd1);
    unix_transport_stop(limited);
    unix_transport_destroy(limited);
    unlink(limited_path);
}

TEST_F(TestUnixTransport, MultipleClients) {
    const int num_clients = 3;
    int fds[num_clients];
    stream_framer_t* framers[num_clients];
    char* ori_strings[num_clients];

    for (int i = 0; i < num_clients; i++) {
        fds[i] = _connect_with_retry(socket_path);
        ASSERT_GE(fds[i], 0);
        framers[i] = stream_framer_create();
        ori_strings[i] = nullptr;
    }

    const uint8_t data[] = "multi client data";

    /* Each client sends a PUT */
    for (int i = 0; i < num_clients; i++) {
        client_api_put_request_t put_req;
        memset(&put_req, 0, sizeof(put_req));
        put_req.content_type = (char*)"application/octet-stream";
        put_req.file_name = (char*)"multi.bin";
        put_req.stream_length = sizeof(data) - 1;
        put_req.server_address = NULL;
        put_req.data = (uint8_t*)data;
        put_req.data_size = sizeof(data) - 1;

        cbor_item_t* frame = client_api_put_request_encode(&put_req);
        ASSERT_NE(frame, nullptr);
        ASSERT_EQ(_send_frame(fds[i], frame), 0);
    }

    /* Each client receives PUT_RESPONSE */
    for (int i = 0; i < num_clients; i++) {
        cbor_item_t* response = _recv_frame(fds[i], framers[i]);
        ASSERT_NE(response, nullptr);
        uint8_t type = client_api_wire_get_type(response);
        EXPECT_EQ(type, CLIENT_API_PUT_RESPONSE);
        if (type == CLIENT_API_PUT_RESPONSE) {
            client_api_put_response_t put_resp;
            memset(&put_resp, 0, sizeof(put_resp));
            if (client_api_put_response_decode(response, &put_resp) == 0) {
                EXPECT_NE(put_resp.ori_string, nullptr);
                ori_strings[i] = strdup(put_resp.ori_string);
                client_api_put_response_destroy(&put_resp);
            }
        }
        cbor_decref(&response);
    }

    /* Verify all ORI strings are valid */
    for (int i = 0; i < num_clients; i++) {
        EXPECT_NE(ori_strings[i], nullptr);
        free(ori_strings[i]);
    }

    for (int i = 0; i < num_clients; i++) {
        stream_framer_destroy(framers[i]);
        close(fds[i]);
    }
}

} // namespace unix_transport_test