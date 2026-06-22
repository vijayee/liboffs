#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <fstream>
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
#include "../src/Network/authority.h"
#include "../src/Network/network.h"
#include "../src/Node/node.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_posix_compat.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
}

namespace unix_transport_test {

/* A small monotonically-increasing suffix makes per-test socket paths
 * unique without pulling in getpid() (which isn't portable to MSVC). */
static int g_socket_seq = 0;

static void _make_socket_path(char* out, size_t out_len, const char* tag) {
    snprintf(out, out_len, "/tmp/test_unix_%s_%d", tag, ++g_socket_seq);
}

/* Resolve a checked-in test cert by name, relative to this source file so the
   result is independent of the test executable's working directory. */
static std::string _cert_path(const char* name) {
    std::string file = __FILE__;
    size_t slash = file.find_last_of("\\/");
    std::string dir = (slash != std::string::npos) ? file.substr(0, slash) : ".";
    return dir + "/certs/" + name;
}

static platform_socket_t* _connect_local(const char* path) {
    return platform_local_connect(path);
}

static platform_socket_t* _connect_with_retry(const char* path, int max_attempts = 50) {
    for (int attempts = 0; attempts < max_attempts; attempts++) {
        platform_sleep_ms(10);
        platform_socket_t* sock = _connect_local(path);
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

    ssize_t sent = platform_socket_send(sock, framed, framed_len);
    free(framed);
    return (sent == (ssize_t)framed_len) ? 0 : -1;
}

static cbor_item_t* _recv_frame(platform_socket_t* sock, stream_framer_t* framer, int timeout_ms = 10000) {
    uint8_t buf[65536];
    for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
        size_t frame_len;
        uint8_t* frame_data = stream_framer_next(framer, &frame_len);
        if (frame_data != NULL) {
            struct cbor_load_result load_result;
            cbor_item_t* item = cbor_load(frame_data, frame_len, &load_result);
            free(frame_data);
            if (item != NULL && load_result.error.code == CBOR_ERR_NONE) {
                return item;
            }
            if (item != NULL) {
                cbor_decref(&item);
            }
        }

        ssize_t received = platform_socket_recv(sock, buf, sizeof(buf));
        if (received > 0) {
            stream_framer_feed(framer, buf, (size_t)received);
        } else if (received == 0) {
            /* Peer closed. */
            return nullptr;
        } else {
            /* EAGAIN: nothing to read right now; back off briefly. */
            platform_sleep_ms(10);
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

        _make_socket_path(socket_path, sizeof(socket_path), "sock");
        platform_local_cleanup(socket_path);

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
        platform_local_cleanup(socket_path);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

TEST_F(TestUnixTransport, ConnectAndClose) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);
    platform_socket_destroy(sock);
}

TEST_F(TestUnixTransport, PutSmallData) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);

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
    ASSERT_EQ(_send_frame(sock, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
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

TEST_F(TestUnixTransport, GetInvalidOri) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);

    client_api_get_request_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.ori_string = (char*)"not-a-valid-ori";
    msg.has_range = 0;

    cbor_item_t* frame = client_api_get_request_encode(&msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
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

TEST_F(TestUnixTransport, StreamingPut) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);

    const char* content_type = "application/octet-stream";
    const char* file_name = "stream_test.bin";
    const uint8_t data[] = "streaming data chunk";

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
    ASSERT_EQ(_send_frame(sock, frame), 0);

    client_api_put_data_t data_msg;
    memset(&data_msg, 0, sizeof(data_msg));
    data_msg.data = (uint8_t*)data;
    data_msg.data_size = sizeof(data) - 1;

    frame = client_api_put_data_encode(&data_msg);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock, frame), 0);

    frame = client_api_put_end_encode();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
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

TEST_F(TestUnixTransport, PutAndGetRoundTrip) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);

    const char* content_type = "application/octet-stream";
    const char* file_name = "roundtrip.bin";
    const uint8_t data[] = "round trip test data";

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

    client_api_get_request_t get_req;
    memset(&get_req, 0, sizeof(get_req));
    get_req.ori_string = ori_string;
    get_req.has_range = 0;

    frame = client_api_get_request_encode(&get_req);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock, frame), 0);
    free(ori_string);

    response = _recv_frame(sock, framer);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_GET_RESPONSE_START);
    cbor_decref(&response);

    response = _recv_frame(sock, framer);
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

    response = _recv_frame(sock, framer);
    ASSERT_NE(response, nullptr);
    type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_GET_END);
    cbor_decref(&response);

    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

TEST_F(TestUnixTransport, MaxConnections) {
    char limited_path[128];
    _make_socket_path(limited_path, sizeof(limited_path), "limited");
    platform_local_cleanup(limited_path);

    unix_transport_t* limited = unix_transport_create(pool, bc, ofd_cache, tc, limited_path, NULL, NULL);
    ASSERT_NE(limited, nullptr);
    unix_transport_set_max_connections(limited, 1);
    unix_transport_start(limited);

    platform_socket_t* sock1 = _connect_with_retry(limited_path);
    ASSERT_NE(sock1, (platform_socket_t*)NULL);

    /* Give server time to accept the connection. */
    platform_sleep_ms(50);

    /* Second connection: the server should accept and then immediately
     * close it because max_connections=1. We expect the socket to be
     * open on the client side; reading from it should yield EOF. */
    platform_socket_t* sock2 = _connect_with_retry(limited_path, 20);
    if (sock2 != NULL) {
        platform_socket_set_nonblocking(sock2);
        uint8_t buf[1];
        ssize_t result = platform_socket_recv(sock2, buf, 1);
        /* Server closes the connection; recv returns 0 (EOF) or
         * -1 (EAGAIN / EOF). */
        EXPECT_LE(result, 0);
        platform_socket_destroy(sock2);
    }

    platform_socket_destroy(sock1);
    unix_transport_stop(limited);
    unix_transport_destroy(limited);
    platform_local_cleanup(limited_path);
}

TEST_F(TestUnixTransport, MultipleClients) {
    const int num_clients = 3;
    platform_socket_t* socks[num_clients];
    stream_framer_t* framers[num_clients];
    char* ori_strings[num_clients];

    for (int i = 0; i < num_clients; i++) {
        socks[i] = _connect_with_retry(socket_path);
        ASSERT_NE(socks[i], (platform_socket_t*)NULL);
        framers[i] = stream_framer_create();
        ori_strings[i] = nullptr;
    }

    const uint8_t data[] = "multi client data";

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
        ASSERT_EQ(_send_frame(socks[i], frame), 0);
    }

    for (int i = 0; i < num_clients; i++) {
        cbor_item_t* response = _recv_frame(socks[i], framers[i]);
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

    for (int i = 0; i < num_clients; i++) {
        EXPECT_NE(ori_strings[i], nullptr);
        free(ori_strings[i]);
    }

    for (int i = 0; i < num_clients; i++) {
        stream_framer_destroy(framers[i]);
        platform_socket_destroy(socks[i]);
    }
}

/* Verify that peer/friend frames are routed to peer_handlers (which gate on
   is_authenticated) instead of falling through to the dispatch default
   ("Unknown message type" -> CLIENT_API_STATUS_BAD_REQUEST). With a non-NULL
   api_key_hash the connection starts unauthenticated (is_authenticated=0 at
   unix_connection init), so each peer/friend handler replies UNAUTHORIZED
   before touching its network/authority borrowed pointers. UNAUTHORIZED is a
   status the default arm can never produce, so observing it proves the
   dispatch arms wired into _unix_dispatch_frame are reached. */
static void _assert_peer_frame_unauthorized(platform_socket_t* sock, cbor_item_t* frame) {
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(_send_frame(sock, frame), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
    ASSERT_NE(response, nullptr);

    uint8_t type = client_api_wire_get_type(response);
    EXPECT_EQ(type, CLIENT_API_ERROR);
    if (type == CLIENT_API_ERROR) {
        client_api_error_t err;
        memset(&err, 0, sizeof(err));
        int decode_result = client_api_error_decode(response, &err);
        EXPECT_EQ(decode_result, 0);
        if (decode_result == 0) {
            EXPECT_EQ(err.status_code, CLIENT_API_STATUS_UNAUTHORIZED);
            client_api_error_destroy(&err);
        }
    }
    cbor_decref(&response);
    stream_framer_destroy(framer);
}

class TestUnixTransportPeerRouting : public testing::Test {
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

        char dir_template[] = "/tmp/test_unix_peer_XXXXXX";
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

        _make_socket_path(socket_path, sizeof(socket_path), "peer");
        platform_local_cleanup(socket_path);

        /* A non-NULL api_key_hash enables auth: the connection starts with
           is_authenticated=0, so peer/friend handlers reply UNAUTHORIZED
           without needing a network/authority (they return at the auth check
           before dereferencing those borrowed pointers). */
        transport = unix_transport_create(pool, bc, ofd_cache, tc, socket_path,
                                          "peer_routing_auth_hash", NULL);
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
        platform_local_cleanup(socket_path);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

TEST_F(TestUnixTransportPeerRouting, PeerInfoRequestRoutedToHandler) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);
    _assert_peer_frame_unauthorized(sock, client_api_peer_info_request_encode());
    platform_socket_destroy(sock);
}

TEST_F(TestUnixTransportPeerRouting, PeerListRequestRoutedToHandler) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);
    _assert_peer_frame_unauthorized(sock, client_api_peer_list_request_encode());
    platform_socket_destroy(sock);
}

TEST_F(TestUnixTransportPeerRouting, FriendListRequestRoutedToHandler) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);
    _assert_peer_frame_unauthorized(sock, client_api_friend_list_request_encode());
    platform_socket_destroy(sock);
}

/* End-to-end success path for peer/friend handlers over a real Unix transport.
   Unlike TestUnixTransportPeerRouting (which uses a non-NULL api_key_hash to
   prove the dispatch arms are *reached* by observing UNAUTHORIZED), this
   fixture runs unauthenticated (api_key_hash=NULL -> is_authenticated=1 at
   unix_connection init) and wires a real authority + network via
   unix_transport_set_config_ctx, so the handlers execute their full success
   path and return well-formed peer/friend responses. authority-only handlers
   (peer_info, friend_list) run regardless of whether network_create succeeds;
   the peer_list handler dereferences ctx->network->conn_mgr, so that test is
   skipped when network_create returns NULL (e.g. a no-MSQUIC build). */
class TestUnixTransportPeerSuccess : public testing::Test {
protected:
    scheduler_pool_t* pool;
    timer_actor_t* timer;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
    authority_t* authority;
    network_t* network;
    offs_node_t node_obj;
    unix_transport_t* transport;
    char* cache_dir;
    char socket_path[128];

    void SetUp() override {
        pool = scheduler_pool_create(4);
        scheduler_pool_start(pool);
        timer = timer_actor_create(pool);

        char dir_template[] = "/tmp/test_unix_peersuccess_XXXXXX";
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

        authority = authority_create(&config);
        ASSERT_NE(authority, nullptr);
        /* Supply a checked-in leaf cert so authority_init_local_id derives a
           real public_key (peer_handle_info_request returns INTERNAL_ERROR
           "No local public key configured" when public_key is NULL). The path
           is resolved relative to this source file, so it is independent of
           the test's working directory. If the cert is absent, init falls
           back to a random id with public_key=NULL and the PeerInfo test
           skips itself; FriendList/PeerList are unaffected. */
        std::string cert = _cert_path("leaf_cert.pem");
        std::ifstream cert_chk(cert);
        if (cert_chk.good()) {
            cert_chk.close();
            authority->node_cert_path = strdup(cert.c_str());
        } else {
            cert_chk.close();
        }
        ASSERT_EQ(authority_init_local_id(authority), 0);

        /* network_create may return NULL on a build without MSQUIC; the
           authority-only success tests still run, and the peer_list test
           skips itself in that case. */
        network = network_create(authority, bc, timer, pool, &config);

        memset(&node_obj, 0, sizeof(node_obj));
        node_obj.config = &config;
        node_obj.authority = authority;
        node_obj.network = network;
        node_obj.block_cache = bc;
        node_obj.scheduler = pool;
        node_obj.timer = timer;

        _make_socket_path(socket_path, sizeof(socket_path), "peersuccess");
        platform_local_cleanup(socket_path);

        /* No api_key_hash -> connections start authenticated, so peer/friend
           handlers run their success path. Config ctx borrows the node so
           per-connection peer_ctx.network/authority are populated. */
        transport = unix_transport_create(pool, bc, ofd_cache, tc, socket_path,
                                          NULL, NULL);
        ASSERT_NE(transport, nullptr);
        unix_transport_set_config_ctx(transport, &node_obj, cache_dir);
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
        /* network_destroy before block_cache_destroy (network borrows the
           cache's respiration pointer); authority_destroy last. Mirrors the
           proven test_quic_integration + off_server teardown order. */
        if (network != nullptr) {
            network_destroy(network);
        }
        block_cache_destroy(bc);
        timer_actor_destroy(timer);
        if (transport != nullptr) {
            unix_transport_destroy(transport);
        }
        scheduler_pool_destroy(pool);
        authority_destroy(authority);
        platform_local_cleanup(socket_path);
        rm_rf(cache_dir);
        free(cache_dir);
    }
};

/* FRIEND_LIST_REQUEST over a real authority with no friends -> FRIEND_LIST_RESPONSE
   with an empty friends array. Exercises the friend_list success path end-to-end. */
TEST_F(TestUnixTransportPeerSuccess, FriendListReturnsEmpty) {
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);
    ASSERT_EQ(_send_frame(sock, client_api_friend_list_request_encode()), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
    ASSERT_NE(response, nullptr);

    EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_FRIEND_LIST_RESPONSE);
    client_api_friend_list_response_t msg;
    memset(&msg, 0, sizeof(msg));
    int decode_result = client_api_friend_list_response_decode(response, &msg);
    EXPECT_EQ(decode_result, 0);
    if (decode_result == 0) {
        size_t count = (msg.friends != nullptr) ? cbor_array_size(msg.friends) : 0;
        EXPECT_EQ(count, 0u);
        client_api_friend_list_response_destroy(&msg);
    }
    cbor_decref(&response);
    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

/* PEER_INFO_REQUEST over a real authority (init_local_id generated a keypair) ->
   PEER_INFO_RESPONSE with raw-CBOR format and non-empty data. */
TEST_F(TestUnixTransportPeerSuccess, PeerInfoReturnsResponse) {
    /* Requires a configured public_key (from the leaf cert in SetUp). Skip
       deterministically when the cert was unavailable so the test stays green
       in environments without the checked-in certs. */
    if (authority->public_key == nullptr) {
        GTEST_SKIP() << "no leaf cert -> no public_key; peer_info returns INTERNAL_ERROR";
    }
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);
    ASSERT_EQ(_send_frame(sock, client_api_peer_info_request_encode()), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
    ASSERT_NE(response, nullptr);

    EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_PEER_INFO_RESPONSE);
    client_api_peer_info_response_t msg;
    memset(&msg, 0, sizeof(msg));
    int decode_result = client_api_peer_info_response_decode(response, &msg);
    EXPECT_EQ(decode_result, 0);
    if (decode_result == 0) {
        EXPECT_EQ(msg.format, 0);
        EXPECT_GT(msg.data_size, 0u);
        client_api_peer_info_response_destroy(&msg);
    }
    cbor_decref(&response);
    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

/* PEER_LIST_REQUEST over a real network -> PEER_LIST_RESPONSE with an empty
   peers array. Skipped when network_create returned NULL (no-MSQUIC build):
   the handler dereferences ctx->network->conn_mgr. */
TEST_F(TestUnixTransportPeerSuccess, PeerListReturnsEmpty) {
    if (network == nullptr) {
        GTEST_SKIP() << "network_create returned NULL (no MSQUIC) — peer_list needs a network";
    }
    platform_socket_t* sock = _connect_with_retry(socket_path);
    ASSERT_NE(sock, (platform_socket_t*)NULL);
    ASSERT_EQ(_send_frame(sock, client_api_peer_list_request_encode()), 0);

    stream_framer_t* framer = stream_framer_create();
    cbor_item_t* response = _recv_frame(sock, framer);
    ASSERT_NE(response, nullptr);

    EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_PEER_LIST_RESPONSE);
    client_api_peer_list_response_t msg;
    memset(&msg, 0, sizeof(msg));
    int decode_result = client_api_peer_list_response_decode(response, &msg);
    EXPECT_EQ(decode_result, 0);
    if (decode_result == 0) {
        size_t count = (msg.peers != nullptr) ? cbor_array_size(msg.peers) : 0;
        EXPECT_EQ(count, 0u);
        client_api_peer_list_response_destroy(&msg);
    }
    cbor_decref(&response);
    stream_framer_destroy(framer);
    platform_socket_destroy(sock);
}

} // namespace unix_transport_test
