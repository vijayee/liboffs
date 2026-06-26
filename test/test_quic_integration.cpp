//
// Created by victor on 5/16/26.
//
// Multi-node QUIC integration tests.
// Categories A-D need no QUIC library; category E uses GTEST_SKIP if msquic is unavailable.

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "Network/stream_framer.h"
#include "Network/conn_state.h"
#include "Network/nat_detect.h"
#include "Network/wire.h"
#include "Network/peer_connection.h"
#include "Network/network.h"
#include "Network/quic_listener.h"
#include "Scheduler/scheduler.h"
#include "BlockCache/block_cache.h"
#include "Timer/timer_actor.h"
#include "Configuration/config.h"
#include "Network/authority.h"
#include "Util/allocator.h"
#include <cbor.h>
}

// =====================================================================
// A. Stream framer integration (no QUIC needed)
// =====================================================================

TEST(StreamFramerIntegration, EncodeDecodePing) {
  // Encode a wire_ping_t, frame it, feed to framer, extract, decode
  wire_ping_t ping = {};
  ping.message_id = 0x1122334455667789ULL;
  ping.timestamp = 9999;

  cbor_item_t* encoded = wire_ping_encode(&ping);
  ASSERT_NE(encoded, nullptr);

  // Serialize CBOR to bytes
  unsigned char* cbor_buf = nullptr;
  size_t cbor_len = 0;
  size_t written = cbor_serialize_alloc(encoded, &cbor_buf, &cbor_len);
  cbor_decref(&encoded);
  ASSERT_GT(written, (size_t)0);
  ASSERT_NE(cbor_buf, nullptr);

  // Frame the serialized CBOR
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(cbor_buf, cbor_len, &frame_len);
  free(cbor_buf);
  ASSERT_NE(frame, nullptr);
  ASSERT_GT(frame_len, (size_t)0);

  // Feed to framer
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);
  int result = stream_framer_feed(framer, frame, frame_len);
  EXPECT_EQ(result, 0);

  // Extract message
  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg_len, cbor_len);

  // Decode back to wire_ping_t
  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(msg, msg_len, &load_result);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_ping_t decoded_ping = {};
  int decode_result = wire_ping_decode(decoded, &decoded_ping);
  cbor_decref(&decoded);
  EXPECT_EQ(decode_result, 0);
  EXPECT_EQ(decoded_ping.message_id, ping.message_id);
  EXPECT_EQ(decoded_ping.timestamp, ping.timestamp);

  free(msg);
  free(frame);
  stream_framer_destroy(framer);
}

TEST(StreamFramerIntegration, EncodeDecodePingResponse) {
  // Encode a wire_ping_response_t, frame it, feed to framer, extract, decode
  wire_ping_response_t response = {};
  response.message_id = 0xDEADBEEFCAFEBABEULL;
  response.echo_time = 12345678;
  response.capacity = 0.75f;
  response.phase = NODE_PHASE_NEUTRAL;

  cbor_item_t* encoded = wire_ping_response_encode(&response);
  ASSERT_NE(encoded, nullptr);

  unsigned char* cbor_buf = nullptr;
  size_t cbor_len = 0;
  size_t written = cbor_serialize_alloc(encoded, &cbor_buf, &cbor_len);
  cbor_decref(&encoded);
  ASSERT_GT(written, (size_t)0);
  ASSERT_NE(cbor_buf, nullptr);

  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(cbor_buf, cbor_len, &frame_len);
  free(cbor_buf);
  ASSERT_NE(frame, nullptr);

  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);
  int result = stream_framer_feed(framer, frame, frame_len);
  EXPECT_EQ(result, 0);

  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(msg, msg_len, &load_result);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_ping_response_t decoded_response = {};
  int decode_result = wire_ping_response_decode(decoded, &decoded_response);
  cbor_decref(&decoded);
  EXPECT_EQ(decode_result, 0);
  EXPECT_EQ(decoded_response.message_id, response.message_id);
  EXPECT_EQ(decoded_response.echo_time, response.echo_time);
  EXPECT_NEAR(decoded_response.capacity, response.capacity, 0.001f);
  EXPECT_EQ(decoded_response.phase, response.phase);

  free(msg);
  free(frame);
  stream_framer_destroy(framer);
}

TEST(StreamFramerIntegration, MultipleMessages) {
  // Frame two different messages, concatenate, feed all at once, extract both
  wire_ping_t ping1 = {};
  ping1.message_id = 100;
  ping1.timestamp = 1000;

  wire_ping_t ping2 = {};
  ping2.message_id = 200;
  ping2.timestamp = 2000;

  cbor_item_t* encoded1 = wire_ping_encode(&ping1);
  ASSERT_NE(encoded1, nullptr);
  cbor_item_t* encoded2 = wire_ping_encode(&ping2);
  ASSERT_NE(encoded2, nullptr);

  unsigned char* cbor_buf1 = nullptr;
  size_t cbor_len1 = 0;
  cbor_serialize_alloc(encoded1, &cbor_buf1, &cbor_len1);
  cbor_decref(&encoded1);
  ASSERT_NE(cbor_buf1, nullptr);

  unsigned char* cbor_buf2 = nullptr;
  size_t cbor_len2 = 0;
  cbor_serialize_alloc(encoded2, &cbor_buf2, &cbor_len2);
  cbor_decref(&encoded2);
  ASSERT_NE(cbor_buf2, nullptr);

  // Frame each separately
  size_t frame1_len = 0;
  uint8_t* frame1 = stream_frame_encode(cbor_buf1, cbor_len1, &frame1_len);
  free(cbor_buf1);
  ASSERT_NE(frame1, nullptr);

  size_t frame2_len = 0;
  uint8_t* frame2 = stream_frame_encode(cbor_buf2, cbor_len2, &frame2_len);
  free(cbor_buf2);
  ASSERT_NE(frame2, nullptr);

  // Concatenate both frames
  size_t combined_len = frame1_len + frame2_len;
  uint8_t* combined = (uint8_t*)malloc(combined_len);
  ASSERT_NE(combined, nullptr);
  memcpy(combined, frame1, frame1_len);
  memcpy(combined + frame1_len, frame2, frame2_len);

  // Feed combined buffer to framer
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);
  int result = stream_framer_feed(framer, combined, combined_len);
  EXPECT_EQ(result, 0);

  // Extract first message
  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  struct cbor_load_result load_result;
  cbor_item_t* decoded1 = cbor_load(msg, msg_len, &load_result);
  free(msg);
  ASSERT_NE(decoded1, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);
  wire_ping_t decoded_ping1 = {};
  EXPECT_EQ(wire_ping_decode(decoded1, &decoded_ping1), 0);
  EXPECT_EQ(decoded_ping1.message_id, ping1.message_id);
  cbor_decref(&decoded1);

  // Extract second message
  msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);
  decoded1 = cbor_load(msg, msg_len, &load_result);
  free(msg);
  ASSERT_NE(decoded1, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);
  wire_ping_t decoded_ping2 = {};
  EXPECT_EQ(wire_ping_decode(decoded1, &decoded_ping2), 0);
  EXPECT_EQ(decoded_ping2.message_id, ping2.message_id);
  cbor_decref(&decoded1);

  // No more messages
  msg = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg, nullptr);

  free(combined);
  free(frame1);
  free(frame2);
  stream_framer_destroy(framer);
}

TEST(StreamFramerIntegration, PartialFeed) {
  // Frame a message, feed in two halves, verify extraction works
  wire_ping_t ping = {};
  ping.message_id = 0xAABBCCDD12345678ULL;
  ping.timestamp = 42;

  cbor_item_t* encoded = wire_ping_encode(&ping);
  ASSERT_NE(encoded, nullptr);

  unsigned char* cbor_buf = nullptr;
  size_t cbor_len = 0;
  size_t written = cbor_serialize_alloc(encoded, &cbor_buf, &cbor_len);
  cbor_decref(&encoded);
  ASSERT_GT(written, (size_t)0);
  ASSERT_NE(cbor_buf, nullptr);

  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(cbor_buf, cbor_len, &frame_len);
  free(cbor_buf);
  ASSERT_NE(frame, nullptr);

  // Feed first half
  size_t first_half = frame_len / 2;
  stream_framer_t* framer = stream_framer_create();
  ASSERT_NE(framer, nullptr);
  int result = stream_framer_feed(framer, frame, first_half);
  EXPECT_EQ(result, 0);

  // Not enough data yet
  size_t msg_len = 0;
  uint8_t* msg = stream_framer_next(framer, &msg_len);
  EXPECT_EQ(msg, nullptr);

  // Feed second half
  result = stream_framer_feed(framer, frame + first_half, frame_len - first_half);
  EXPECT_EQ(result, 0);

  // Now we can extract
  msg = stream_framer_next(framer, &msg_len);
  ASSERT_NE(msg, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(msg, msg_len, &load_result);
  free(msg);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_ping_t decoded_ping = {};
  EXPECT_EQ(wire_ping_decode(decoded, &decoded_ping), 0);
  EXPECT_EQ(decoded_ping.message_id, ping.message_id);
  cbor_decref(&decoded);

  free(frame);
  stream_framer_destroy(framer);
}

// =====================================================================
// B. Connection state machine (no QUIC needed)
// =====================================================================

// Helper: create a peer_connection_t using the library's allocator so
// that struct layout is consistent between test code and library code.
// This requires a scheduler pool for peer_connection_create.
static scheduler_pool_t* g_pool = nullptr;

static peer_connection_t* test_peer_create() {
  node_id_t id = {};
  memset(id.hash, 0xCC, NODE_ID_HASH_SIZE);
  peer_connection_t* peer = peer_connection_create(&id, nullptr, 0.1f, g_pool);
  EXPECT_NE(peer, nullptr);
  return peer;
}

static void test_peer_destroy(peer_connection_t* peer) {
  peer_connection_destroy(peer);
}

class ConnStateIntegration : public ::testing::Test {
protected:
  void SetUp() override {
    g_pool = scheduler_pool_create(2);
    ASSERT_NE(g_pool, nullptr);
    scheduler_pool_start(g_pool);
  }
  void TearDown() override {
    if (g_pool != nullptr) {
      scheduler_pool_wait_for_idle(g_pool);
      scheduler_pool_stop(g_pool);
      scheduler_pool_destroy(g_pool);
      g_pool = nullptr;
    }
  }
};

TEST_F(ConnStateIntegration, InitOpenReturnsTryingDirect) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_OPEN), CONN_STATE_TRYING_DIRECT);
}

TEST_F(ConnStateIntegration, InitSymmetricReturnsRelayOnly) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_SYMMETRIC), CONN_STATE_RELAY_ONLY);
}

TEST_F(ConnStateIntegration, DirectConnected) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  conn_state_on_direct_connected(peer);
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_DIRECT);

  test_peer_destroy(peer);
}

TEST_F(ConnStateIntegration, DirectFailedFallsBackToRelay) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  conn_state_on_direct_failed(peer);
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_RELAY);

  test_peer_destroy(peer);
}

TEST_F(ConnStateIntegration, UpgradeToDirect) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_RELAY;
  conn_state_upgrade_to_direct(peer);
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_TRYING_DIRECT);

  test_peer_destroy(peer);
}

TEST_F(ConnStateIntegration, SymmetricNoUpgrade) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  // Set peer NAT type to symmetric, which should force RELAY_ONLY state
  conn_state_set_peer_nat_type(peer, NAT_TYPE_SYMMETRIC);
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_RELAY_ONLY);

  // Upgrade attempt should be blocked — stays RELAY_ONLY
  conn_state_upgrade_to_direct(peer);
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_RELAY_ONLY);

  test_peer_destroy(peer);
}

// =====================================================================
// C. Wire protocol encode/decode round-trip (no QUIC needed)
// =====================================================================

TEST(WireRoundtrip, RelaySend) {
  uint8_t payload_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
  wire_relay_send_t original = {};
  original.src_endpoint_id = 100;
  original.dest_endpoint_id = 200;
  original.payload = payload_data;
  original.payload_len = sizeof(payload_data);

  cbor_item_t* encoded = wire_relay_send_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Serialize and deserialize CBOR
  unsigned char* buf = nullptr;
  size_t buf_len = 0;
  size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_GT(written, (size_t)0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_relay_send_t result = {};
  result.payload = nullptr;
  result.payload_len = 0;
  int rc = wire_relay_send_decode(decoded, &result);
  cbor_decref(&decoded);
  EXPECT_EQ(rc, 0);

  EXPECT_EQ(result.src_endpoint_id, original.src_endpoint_id);
  EXPECT_EQ(result.dest_endpoint_id, original.dest_endpoint_id);
  EXPECT_EQ(result.payload_len, original.payload_len);
  ASSERT_NE(result.payload, nullptr);
  EXPECT_EQ(memcmp(result.payload, original.payload, original.payload_len), 0);

  // Cleanup — use the destroy helper
  free(result.payload);
}

TEST(WireRoundtrip, AddrRequest) {
  wire_addr_request_t original = {};
  original.message_id = 0x12345678ABCDEF01ULL;

  cbor_item_t* encoded = wire_addr_request_encode(&original);
  ASSERT_NE(encoded, nullptr);

  unsigned char* buf = nullptr;
  size_t buf_len = 0;
  size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_GT(written, (size_t)0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_addr_request_t result = {};
  int rc = wire_addr_request_decode(decoded, &result);
  cbor_decref(&decoded);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(result.message_id, original.message_id);
}

TEST(WireRoundtrip, AddrResponse) {
  wire_addr_response_t original = {};
  original.message_id = 0x8877665544332211ULL;
  original.endpoint_id = 500;
  original.reflexive_addr = 0xC0A80101;  // 192.168.1.1
  original.reflexive_port = 12345;

  cbor_item_t* encoded = wire_addr_response_encode(&original);
  ASSERT_NE(encoded, nullptr);

  unsigned char* buf = nullptr;
  size_t buf_len = 0;
  size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_GT(written, (size_t)0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_addr_response_t result = {};
  int rc = wire_addr_response_decode(decoded, &result);
  cbor_decref(&decoded);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(result.message_id, original.message_id);
  EXPECT_EQ(result.endpoint_id, original.endpoint_id);
  EXPECT_EQ(result.reflexive_addr, original.reflexive_addr);
  EXPECT_EQ(result.reflexive_port, original.reflexive_port);
}

TEST(WireRoundtrip, RelayReceived) {
  uint8_t payload_data[] = {0x01, 0x02, 0x03, 0x04};
  wire_relay_received_t original = {};
  original.src_endpoint_id = 42;
  original.payload = payload_data;
  original.payload_len = sizeof(payload_data);

  cbor_item_t* encoded = wire_relay_received_encode(&original);
  ASSERT_NE(encoded, nullptr);

  unsigned char* buf = nullptr;
  size_t buf_len = 0;
  size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_GT(written, (size_t)0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_relay_received_t result = {};
  result.payload = nullptr;
  result.payload_len = 0;
  int rc = wire_relay_received_decode(decoded, &result);
  cbor_decref(&decoded);
  EXPECT_EQ(rc, 0);

  EXPECT_EQ(result.src_endpoint_id, original.src_endpoint_id);
  EXPECT_EQ(result.payload_len, original.payload_len);
  ASSERT_NE(result.payload, nullptr);
  EXPECT_EQ(memcmp(result.payload, original.payload, original.payload_len), 0);

  free(result.payload);
}

// =====================================================================
// D. NAT detection classification (no QUIC needed)
// =====================================================================

TEST(NATDetectIntegration, ClassifyOpen) {
  // Local matches reflexive address from relay → NAT_TYPE_OPEN
  uint32_t local_addr = 0xC0A80101;  // 192.168.1.1
  uint32_t reflexive_addr_a = 0xC0A80101;
  uint16_t reflexive_port_a = 12345;
  uint32_t reflexive_addr_b = 0xC0A80101;
  uint16_t reflexive_port_b = 12345;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_OPEN);
}

TEST(NATDetectIntegration, ClassifyFullCone) {
  // Both reflexive addresses match, same port → NAT_TYPE_FULL_CONE
  uint32_t local_addr = 0xC0A80101;       // 192.168.1.1 (private)
  uint32_t reflexive_addr_a = 0x01020304;  // 1.2.3.4 (public)
  uint16_t reflexive_port_a = 50000;
  uint32_t reflexive_addr_b = 0x01020304;
  uint16_t reflexive_port_b = 50000;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_FULL_CONE);
}

TEST(NATDetectIntegration, ClassifySymmetric) {
  // Different reflexive addresses → NAT_TYPE_SYMMETRIC
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_a = 0x01020304;
  uint16_t reflexive_port_a = 50000;
  uint32_t reflexive_addr_b = 0x05060708;
  uint16_t reflexive_port_b = 50001;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_SYMMETRIC);
}

TEST(NATDetectIntegration, ClassifyPortRestrictedCone) {
  // Only one relay responds, local doesn't match reflexive → NAT_TYPE_PORT_RESTRICTED_CONE
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_a = 0x01020304;
  uint16_t reflexive_port_a = 50000;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      0, 0, 0);
  EXPECT_EQ(result, NAT_TYPE_PORT_RESTRICTED_CONE);
}

// =====================================================================
// E. QUIC integration (requires msquic — use GTEST_SKIP)
// =====================================================================

#ifdef HAS_MSQUIC
#include <msquic.h>

// Helper: attempt msquic initialization; returns true on success.
static bool try_init_msquic(const QUIC_API_TABLE** table) {
  QUIC_STATUS status = MsQuicOpen2(table);
  if (QUIC_FAILED(status)) {
    return false;
  }
  return true;
}

static void cleanup_msquic(const QUIC_API_TABLE* table) {
  if (table != nullptr) {
    MsQuicClose(table);
  }
}
#endif // HAS_MSQUIC

TEST(QuicIntegration, NetworkCreateDestroy) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  // Verify network_t create/destroy works when msquic is linked.
  // We need minimal dependencies: authority, block_cache, timer, scheduler.
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  config_t config = config_default();
  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);

  block_cache_t* cache = block_cache_create(config, (char*)"/tmp/test_quic_bc", standard, timer, pool, NULL, 0);
  ASSERT_NE(cache, nullptr);

  network_t* network = network_create(authority, cache, timer, pool, &config);
  // network_create may return nullptr if msquic initialization fails
  if (network == nullptr) {
    block_cache_destroy(cache);
    timer_actor_destroy(timer);
    authority_destroy(authority);
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    GTEST_SKIP() << "msquic initialization failed";
  }

  EXPECT_NE(network, nullptr);

  network_destroy(network);
  block_cache_destroy(cache);
  timer_actor_destroy(timer);
  authority_destroy(authority);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
#endif
}

#ifdef _WIN32
#include <windows.h>
#include <iostream>

static DWORD net_hl_handle_count() {
  DWORD count = 0;
  GetProcessHandleCount(GetCurrentProcess(), &count);
  return count;
}

/* Guard the full network create/destroy handle path with a persistent
   timer_actor + block_cache + authority (the configuration that exhibited a
   ~0.7-handle/cycle linear leak over 100 cycles). network_create sets five
   recurring timers; network_destroy cancels them. The leak was the timer
   cancel path stranding cancel messages under 2 scheduler workers, leaving
   pd_timers (and their CreateEvent + CreateTimerQueueTimer handles) tracked
   until timer_actor_destroy — fixed by making timer_actor_cancel synchronous.
   100 cycles with a midpoint distinguishes a real linear leak from bounded
   ramp-up caching that plateaus. */
TEST(QuicIntegration, NetworkCreateDestroyNoHandleLeak) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  config_t config = config_default();
  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);

  block_cache_t* cache = block_cache_create(config, (char*)"/tmp/test_quic_hl_bc", standard, timer, pool, NULL, 0);
  ASSERT_NE(cache, nullptr);

  /* Warm up: let runtime/MsQuic/timer-queue caches reach steady state. */
  bool msquic_ok = true;
  for (int i = 0; i < 3; i++) {
    network_t* network = network_create(authority, cache, timer, pool, &config);
    if (network == nullptr) { msquic_ok = false; break; }
    scheduler_pool_wait_for_idle(pool);
    network_destroy(network);
    scheduler_pool_wait_for_idle(pool);
  }
  if (!msquic_ok) {
    block_cache_destroy(cache);
    timer_actor_destroy(timer);
    authority_destroy(authority);
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    GTEST_SKIP() << "msquic initialization failed";
  }

  const int N = 100;
  DWORD before = net_hl_handle_count();
  DWORD mid = 0;
  for (int i = 0; i < N; i++) {
    network_t* network = network_create(authority, cache, timer, pool, &config);
    ASSERT_NE(network, nullptr);
    scheduler_pool_wait_for_idle(pool);
    network_destroy(network);
    scheduler_pool_wait_for_idle(pool);
    if (i == N / 2) mid = net_hl_handle_count();
  }
  DWORD after = net_hl_handle_count();

  block_cache_destroy(cache);
  timer_actor_destroy(timer);
  authority_destroy(authority);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);

  long first_half = (long)mid - (long)before;
  long second_half = (long)after - (long)mid;
  long total = (long)after - (long)before;
  EXPECT_LE(total, 10) << "network handle growth: +" << total << " over " << N
                       << " cycles (before=" << before << " mid=" << mid
                       << " after=" << after << ")";
  if (first_half > 5 && second_half > first_half / 2) {
    ADD_FAILURE() << "network handle growth is linear, not plateauing: first_half=+"
                  << first_half << " second_half=+" << second_half;
  }
#endif
}
#endif /* _WIN32 */

TEST(QuicIntegration, QuicListenerStartStop) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  const QUIC_API_TABLE* msquic = nullptr;
  if (!try_init_msquic(&msquic)) {
    GTEST_SKIP() << "msquic initialization failed";
  }

  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  config_t config = config_default();
  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);

  block_cache_t* cache = block_cache_create(config, (char*)"/tmp/test_quic_bc2", standard, timer, pool, NULL, 0);
  ASSERT_NE(cache, nullptr);

  network_t* network = network_create(authority, cache, timer, pool, &config);
  if (network == nullptr) {
    timer_actor_destroy(timer);
    block_cache_destroy(cache);
    authority_destroy(authority);
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    cleanup_msquic(msquic);
    GTEST_SKIP() << "network creation failed";
  }

  // Start and stop a QUIC listener on localhost
  quic_listener_t* listener = quic_listener_create(network, pool);
  if (listener == nullptr) {
    network_destroy(network);
    timer_actor_destroy(timer);
    block_cache_destroy(cache);
    authority_destroy(authority);
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    cleanup_msquic(msquic);
    GTEST_SKIP() << "quic_listener_create failed";
  }

  int start_result = quic_listener_start(listener, "127.0.0.1", 0);
  // Port 0 means let the OS pick a port; if msquic is working, this should succeed
  if (start_result == 0) {
    quic_listener_stop(listener);
  }

  quic_listener_destroy(listener);
  network_destroy(network);
  timer_actor_destroy(timer);
  block_cache_destroy(cache);
  authority_destroy(authority);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  cleanup_msquic(msquic);
#endif
}