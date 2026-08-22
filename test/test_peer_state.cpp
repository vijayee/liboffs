#include <gtest/gtest.h>
extern "C" {
#include "Platform/platform_atomic.h"
#include "Util/allocator.h"
#include "Network/authority.h"
#include "Network/network.h"
#include "Network/hebbian.h"
#include "Network/hebbian_config.h"
#include "Network/rate_limit.h"
#include "Network/connection_manager.h"
#include "Network/node_id.h"
#include "Network/ring_set.h"
}
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

TEST(PlatformAtomicWrite, WritesFileAtomically) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_atomic_test.cbor";
  std::string data = "hello atomic world";
  int rc = platform_file_atomic_write(tmp.c_str(), (const uint8_t*)data.data(), data.size());
  ASSERT_EQ(rc, 0);
  std::ifstream in(tmp, std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(got, data);
  fs::remove(tmp);
}

TEST(PlatformAtomicWrite, OverwritesExistingFile) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_atomic_overwrite.cbor";
  { std::ofstream(tmp) << "old content"; }
  std::string data = "new content that is longer";
  int rc = platform_file_atomic_write(tmp.c_str(), (const uint8_t*)data.data(), data.size());
  ASSERT_EQ(rc, 0);
  std::ifstream in(tmp, std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(got, data);
  fs::remove(tmp);
}

TEST(PlatformAtomicWrite, NoLeftoverTempFile) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_atomic_notmp.cbor";
  std::string data = "no leftover";
  ASSERT_EQ(platform_file_atomic_write(tmp.c_str(), (const uint8_t*)data.data(), data.size()), 0);
  // After a successful write, no .tmp sibling of the target should remain.
  for (auto& entry : fs::directory_iterator(tmp.parent_path())) {
    std::string name = entry.path().filename().string();
    if (name.rfind("liboffs_atomic_notmp", 0) == 0 && name != "liboffs_atomic_notmp.cbor") {
      FAIL() << "Leftover temp file: " << name;
    }
  }
  fs::remove(tmp);
}

// v3 peer-state format persists Hebbian weights alongside the 4 new per-peer
// fields (relay_verified, nat_type, last_seen_ms, bad_blocks_received). The
// minimal-network round-trip below exercises the Hebbian path with
// network->rings == NULL — authority_save_peers must guard against that.
// The peer-record round-trip (12 fields) needs a real ring_set, which is
// covered by integration tests that wire up a full network_t.
TEST(PeerStatePersistence, RoundTripPersistsWeightsAndPeerInfo) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_peerstate_roundtrip.cbor";
  std::string path = tmp.string();

  authority_t auth;
  memset(&auth, 0, sizeof(auth));
  auth.peer_store_path = (char*)path.c_str();
  memset(auth.local_id.hash, 0xAB, NODE_ID_HASH_SIZE);

  network_t net;
  memset(&net, 0, sizeof(net));
  hebbian_config_t hcfg;
  hebbian_config_init(&hcfg);
  connection_manager_init(&net.conn_mgr, 16, &hcfg);
  rate_limit_table_init(&net.rate_limits, 16);
  hebbian_table_init(&net.hebbian, 16, 0.999f);
  // NOTE: net.rings is NULL — authority_save_peers must guard against this.

  node_id_t peer;
  memset(&peer.hash, 0x5C, NODE_ID_HASH_SIZE);
  hebbian_table_set(&net.hebbian, &peer, 0.75f);

  ASSERT_EQ(authority_save_peers(&auth, &net), 0);

  // Load into a fresh network.
  network_t net2;
  memset(&net2, 0, sizeof(net2));
  hebbian_config_t hcfg2;
  hebbian_config_init(&hcfg2);
  connection_manager_init(&net2.conn_mgr, 16, &hcfg2);
  rate_limit_table_init(&net2.rate_limits, 16);
  hebbian_table_init(&net2.hebbian, 16, 0.999f);

  ASSERT_EQ(authority_load_peers(&auth, &net2), 0);
  EXPECT_NEAR(hebbian_table_get(&net2.hebbian, &peer), 0.75f, 0.001f);

  hebbian_table_deinit(&net.hebbian);
  rate_limit_table_deinit(&net.rate_limits);
  connection_manager_deinit(&net.conn_mgr);
  hebbian_table_deinit(&net2.hebbian);
  rate_limit_table_deinit(&net2.rate_limits);
  connection_manager_deinit(&net2.conn_mgr);
  fs::remove(tmp);
}

// Full v3 peer-record round-trip: persist a peer with all 12 fields, reload
// into a fresh network with a real ring_set, and verify the 4 new v3 fields
// (relay_verified, nat_type, last_seen_ms, bad_blocks_received) survive.
TEST(PeerStatePersistence, RoundTripPersistsV3PeerFields) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_peerstate_v3_peer.cbor";
  std::string path = tmp.string();

  authority_t auth;
  memset(&auth, 0, sizeof(auth));
  auth.peer_store_path = (char*)path.c_str();
  memset(auth.local_id.hash, 0xAB, NODE_ID_HASH_SIZE);

  network_t net;
  memset(&net, 0, sizeof(net));
  hebbian_config_t hcfg;
  hebbian_config_init(&hcfg);
  connection_manager_init(&net.conn_mgr, 16, &hcfg);
  rate_limit_table_init(&net.rate_limits, 16);
  hebbian_table_init(&net.hebbian, 16, 0.999f);
  net.rings = ring_set_create(8, 4, 2);

  node_id_t peer_id;
  memset(&peer_id.hash, 0x77, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&peer_id, 0x0A0000FFu, 8080);
  node->latency_ms = 12.5f;
  node->weight = 0.5f;
  node->capacity = 0.8f;
  node->phase = NODE_PHASE_EXHALE;
  node->availability = 0.9f;
  node->relay_verified = true;
  node->nat_type = NAT_TYPE_FULL_CONE;
  node->last_seen_ms = 0x1234567890ABCDEFULL;
  node->bad_blocks_received = 42;
  ASSERT_EQ(ring_set_insert(net.rings, node, 12500), 0);

  ASSERT_EQ(authority_save_peers(&auth, &net), 0);

  // Load into a fresh network with a fresh ring_set.
  network_t net2;
  memset(&net2, 0, sizeof(net2));
  hebbian_config_t hcfg2;
  hebbian_config_init(&hcfg2);
  connection_manager_init(&net2.conn_mgr, 16, &hcfg2);
  rate_limit_table_init(&net2.rate_limits, 16);
  hebbian_table_init(&net2.hebbian, 16, 0.999f);
  net2.rings = ring_set_create(8, 4, 2);

  ASSERT_EQ(authority_load_peers(&auth, &net2), 0);
  net_node_t* restored = ring_set_find_by_id(net2.rings, &peer_id);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->addr, 0x0A0000FFu);
  EXPECT_EQ(restored->port, 8080);
  EXPECT_NEAR(restored->latency_ms, 12.5f, 0.001f);
  EXPECT_NEAR(restored->capacity, 0.8f, 0.001f);
  EXPECT_EQ(restored->phase, NODE_PHASE_EXHALE);
  EXPECT_NEAR(restored->availability, 0.9f, 0.001f);
  // v3 fields
  EXPECT_TRUE(restored->relay_verified);
  EXPECT_EQ(restored->nat_type, NAT_TYPE_FULL_CONE);
  EXPECT_EQ(restored->last_seen_ms, 0x1234567890ABCDEFULL);
  EXPECT_EQ(restored->bad_blocks_received, (uint64_t)42);

  ring_set_clear_nodes(net.rings);
  ring_set_destroy(net.rings);
  ring_set_clear_nodes(net2.rings);
  ring_set_destroy(net2.rings);
  hebbian_table_deinit(&net.hebbian);
  rate_limit_table_deinit(&net.rate_limits);
  connection_manager_deinit(&net.conn_mgr);
  hebbian_table_deinit(&net2.hebbian);
  rate_limit_table_deinit(&net2.rate_limits);
  connection_manager_deinit(&net2.conn_mgr);
  fs::remove(tmp);
}