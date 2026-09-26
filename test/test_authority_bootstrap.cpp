// Bootstrap list helpers and peer-store round trip (index 6).
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "Configuration/config.h"
#include "Network/authority.h"
#include "Network/endpoint.h"
#include "Network/network.h"
#include "Network/hebbian.h"
#include "Network/hebbian_config.h"
#include "Network/rate_limit.h"
#include "Network/connection_manager.h"
#include "Network/node_id.h"
#include "Network/ring_set.h"
}

namespace fs = std::filesystem;

// The authority keeps authority->config alive for the whole test by holding
// the config_t on the enclosing test's stack (mirrors the minimal-network
// fixture style of test_peer_state.cpp).
static authority_t* make_authority(const char* path, config_t* config_storage) {
  memset(config_storage, 0, sizeof(*config_storage));
  authority_t* authority = authority_create(config_storage);
  if (authority != NULL) {
    authority->peer_store_path = strdup(path);
  }
  return authority;
}

TEST(AuthorityBootstrap, AddValidatesAndDedups) {
  config_t config;
  authority_t* authority = make_authority("/tmp/liboffs_test_bootstrap_store.cbor", &config);
  ASSERT_NE(authority, nullptr);

  EXPECT_EQ(0, authority_bootstrap_add(authority, "10.0.0.1:8080"));
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  EXPECT_EQ(-2, authority_bootstrap_add(authority, "10.0.0.1:8080"));  // dup
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  EXPECT_EQ(-1, authority_bootstrap_add(authority, "not-an-endpoint"));
  EXPECT_EQ(-1, authority_bootstrap_add(authority, "2001:db8::1:9090"));  // bare v6
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  // Config-seeded entries count as duplicates too.
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "10.0.0.1:8080"));
  EXPECT_EQ(-2, authority_bootstrap_add(authority, "10.0.0.1:8080"));  // dup vs config
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);
  EXPECT_EQ(1u, authority->bootstrap_peer_count);

  authority_destroy(authority);
}

TEST(AuthorityBootstrap, RemoveKnownAndUnknown) {
  config_t config;
  authority_t* authority = make_authority("/tmp/liboffs_test_bootstrap_store.cbor", &config);
  ASSERT_NE(authority, nullptr);
  // Config-seeded source entry; managed entry added by the operator.
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "10.0.0.1:8080"));
  ASSERT_EQ(0, authority_bootstrap_add(authority, "10.0.0.2:8080"));

  // Config-source entry: conflict, and neither list is modified.
  EXPECT_EQ(-2, authority_bootstrap_remove(authority, "10.0.0.1:8080"));
  EXPECT_EQ(1u, authority->bootstrap_peer_count);
  EXPECT_EQ(1u, authority->managed_bootstrap_peer_count);

  // Managed copy is removable.
  EXPECT_EQ(0, authority_bootstrap_remove(authority, "10.0.0.2:8080"));
  EXPECT_EQ(0u, authority->managed_bootstrap_peer_count);
  EXPECT_EQ(-1, authority_bootstrap_remove(authority, "10.0.0.2:8080"));  // now unknown
  EXPECT_EQ(-1, authority_bootstrap_remove(authority, "999.1.1.1:1"));    // unknown

  authority_destroy(authority);
}

TEST(AuthorityBootstrap, SetBootstrapPeersFailsAtomicallyOnInvalidCsv) {
  config_t config;
  authority_t* authority = make_authority("/tmp/liboffs_test_bootstrap_store.cbor", &config);
  ASSERT_NE(authority, nullptr);
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "10.0.0.1:8080,10.0.0.2:9090"));

  // An invalid mid-CSV token fails the whole call and leaves the previous
  // list untouched (no partial seed).
  EXPECT_EQ(-1, authority_set_bootstrap_peers(authority, "10.0.0.3:7070,garbage"));
  EXPECT_EQ(2u, authority->bootstrap_peer_count);
  EXPECT_STREQ("10.0.0.1", authority->bootstrap_peers[0].info->addresses[0].host);
  EXPECT_EQ(8080, authority->bootstrap_peers[0].info->addresses[0].port);
  EXPECT_STREQ("10.0.0.2", authority->bootstrap_peers[1].info->addresses[0].host);
  EXPECT_EQ(9090, authority->bootstrap_peers[1].info->addresses[0].port);

  authority_destroy(authority);
}

TEST(AuthorityBootstrap, SetBootstrapPeersParsesCsvAndNormalizes) {
  config_t config;
  authority_t* authority = make_authority("/tmp/liboffs_test_bootstrap_store.cbor", &config);
  ASSERT_NE(authority, nullptr);
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "10.0.0.1:8080,[2001:db8::1]:9090"));
  ASSERT_EQ(2u, authority->bootstrap_peer_count);
  EXPECT_STREQ("10.0.0.1", authority->bootstrap_peers[0].info->addresses[0].host);
  EXPECT_EQ(8080, authority->bootstrap_peers[0].info->addresses[0].port);
  EXPECT_STREQ("2001:db8::1", authority->bootstrap_peers[1].info->addresses[0].host);

  // Whitespace tolerated (endpoint_parse trims).
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, " 10.0.0.2:8080 , 10.0.0.3:9090"));
  ASSERT_EQ(2u, authority->bootstrap_peer_count);

  // 5-digit port on an IPv6 literal must not be truncated by normalization.
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "[::1]:65535"));
  ASSERT_EQ(1u, authority->bootstrap_peer_count);
  EXPECT_STREQ("::1", authority->bootstrap_peers[0].info->addresses[0].host);

  EXPECT_EQ(-1, authority_set_bootstrap_peers(authority, "10.0.0.1:8080,garbage"));
  EXPECT_EQ(0, authority_set_bootstrap_peers(authority, NULL));
  EXPECT_EQ(0u, authority->bootstrap_peer_count);

  authority_destroy(authority);
}

// Peer-store round trip through index 6: operator-managed entries must
// survive save/load while the config-seeded list stays untouched by the
// load path (config seeding happens after load via authority_set_bootstrap_peers).
TEST(AuthorityBootstrap, PeerStoreRoundTripIndex6) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_bootstrap_roundtrip.cbor";
  std::string path = tmp.string();

  config_t config;
  authority_t* authority = make_authority(path.c_str(), &config);
  ASSERT_NE(authority, nullptr);
  ASSERT_EQ(0, authority_bootstrap_add(authority, "10.0.0.1:8080"));
  ASSERT_EQ(0, authority_bootstrap_add(authority, "[2001:db8::1]:9090"));
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "10.0.0.3:7070"));

  network_t net;
  memset(&net, 0, sizeof(net));
  hebbian_config_t hcfg;
  hebbian_config_init(&hcfg);
  connection_manager_init(&net.conn_mgr, 16, &hcfg);
  rate_limit_table_init(&net.rate_limits, 16);
  hebbian_table_init(&net.hebbian, 16, 0.999f);
  // NOTE: net.rings stays NULL — authority_save_peers must guard against it.

  ASSERT_EQ(authority_save_peers(authority, &net), 0);

  // Fresh authority + fresh network over the same peer_store_path.
  config_t config2;
  authority_t* loaded = make_authority(path.c_str(), &config2);
  ASSERT_NE(loaded, nullptr);
  // Seed a distinct config list so the load path can be proven not to touch it.
  ASSERT_EQ(0, authority_set_bootstrap_peers(loaded, "9.9.9.9:1"));

  network_t net2;
  memset(&net2, 0, sizeof(net2));
  hebbian_config_t hcfg2;
  hebbian_config_init(&hcfg2);
  connection_manager_init(&net2.conn_mgr, 16, &hcfg2);
  rate_limit_table_init(&net2.rate_limits, 16);
  hebbian_table_init(&net2.hebbian, 16, 0.999f);

  ASSERT_EQ(authority_load_peers(loaded, &net2), 0);
  // Managed list round-tripped (2 entries, exact normalized strings).
  ASSERT_EQ(2u, loaded->managed_bootstrap_peer_count);
  EXPECT_STREQ("10.0.0.1:8080", loaded->managed_bootstrap_peers[0]);
  EXPECT_STREQ("[2001:db8::1]:9090", loaded->managed_bootstrap_peers[1]);
  // Config-seeded list is NOT loaded from the store — stays as seeded above.
  ASSERT_EQ(1u, loaded->bootstrap_peer_count);
  EXPECT_STREQ("9.9.9.9", loaded->bootstrap_peers[0].info->addresses[0].host);
  EXPECT_EQ(1, loaded->bootstrap_peers[0].info->addresses[0].port);

  authority_destroy(loaded);
  authority_destroy(authority);
  hebbian_table_deinit(&net.hebbian);
  rate_limit_table_deinit(&net.rate_limits);
  connection_manager_deinit(&net.conn_mgr);
  hebbian_table_deinit(&net2.hebbian);
  rate_limit_table_deinit(&net2.rate_limits);
  connection_manager_deinit(&net2.conn_mgr);
  fs::remove(tmp);
}

// A config token that is not an endpoint is accepted as a base58 peer_info
// (the /peer/info interchange form): the entry carries the full candidate
// list and a pinned node_id, unlike endpoint tokens (trust-on-first-use).
TEST(AuthorityBootstrap, SetBootstrapPeersAcceptsBase58PeerInfo) {
  config_t config;
  authority_t* authority = make_authority("/tmp/liboffs_test_bootstrap_store.cbor", &config);
  ASSERT_NE(authority, nullptr);

  peer_info_t original;
  memset(&original, 0, sizeof(original));
  node_id_generate(&original.node_id);
  original.public_key = static_cast<uint8_t*>(malloc(32));
  ASSERT_NE(original.public_key, nullptr);
  for (size_t index = 0; index < 32; index++) original.public_key[index] = (uint8_t)index;
  original.public_key_len = 32;
  original.addresses = static_cast<peer_address_t*>(calloc(2, sizeof(peer_address_t)));
  ASSERT_NE(original.addresses, nullptr);
  original.addresses[0].type = PEER_ADDR_HOST;
  original.addresses[0].host = strdup("172.178.8.253");
  original.addresses[0].port = 23401;
  original.addresses[1].type = PEER_ADDR_HOST;
  original.addresses[1].host = strdup("[2001:db8::9]");
  original.addresses[1].port = 23401;
  original.address_count = 2;

  char* b58 = peer_info_to_base58(&original);
  ASSERT_NE(b58, nullptr);

  char csv[2048];
  snprintf(csv, sizeof(csv), "%s", b58);
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, csv));
  ASSERT_EQ(1u, authority->bootstrap_peer_count);

  peer_info_t* entry = authority->bootstrap_peers[0].info;
  EXPECT_TRUE(node_id_equals(&entry->node_id, &original.node_id));
  ASSERT_EQ(2u, entry->address_count);
  EXPECT_STREQ("172.178.8.253", entry->addresses[0].host);
  EXPECT_EQ(23401, entry->addresses[0].port);
  EXPECT_STREQ("[2001:db8::9]", entry->addresses[1].host);
  // Pinned — the salutation must confirm this node_id.
  EXPECT_EQ(1, authority->bootstrap_peers[0].pinned);

  // Mixed CSV: base58 peer_info and endpoint token coexist.
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "10.0.0.5:7070"));
  EXPECT_EQ(1u, authority->bootstrap_peer_count);  // peer_info replaced
  snprintf(csv, sizeof(csv), "%s,10.0.0.5:7070", b58);
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, csv));
  ASSERT_EQ(2u, authority->bootstrap_peer_count);
  EXPECT_EQ(1, authority->bootstrap_peers[0].pinned);
  EXPECT_EQ(0, authority->bootstrap_peers[1].pinned);
  EXPECT_STREQ("10.0.0.5", authority->bootstrap_peers[1].info->addresses[0].host);

  free(b58);
  peer_info_destroy(&original);
  authority_destroy(authority);
}
