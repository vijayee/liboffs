/* Unit tests for peer_info_from_node (audit #18, NAT Phase A).
 *
 * Exercises the candidate-population logic without a full network fixture:
 * we stack-allocate minimal fake network_t / quic_listener_t / relay_client_t
 * structs (zeroed, only the fields peer_info_from_node reads are populated)
 * and verify HOST/SRFLX/RELAY candidates are emitted with the right types,
 * ports, and the host-byte-order reflexive-addr conversion.
 *
 * The HOST (LAN) count is environment-dependent (depends on the test
 * machine's interfaces), so the LAN-on test only asserts the function
 * returns 0 and doesn't crash — the SRFLX/RELAY tests are the load-bearing
 * ones. */
#include <gtest/gtest.h>

extern "C" {
#include "../src/Network/peer_info.h"
#include "../src/Network/network.h"
#include "../src/Network/quic_listener.h"
#include "../src/Network/relay_client.h"
#include "../src/Util/allocator.h"
#include <string.h>
#include <stdlib.h>
}

namespace {

/* Minimal stand-ins — peer_info_from_node only reads a handful of fields.
 * We allocate via get_clear_memory so the structs are zeroed; any field we
 * don't touch stays NULL/0. */
struct fake_network_t {
  network_t net;
  quic_listener_t listener;
  relay_client_t relay;
};

/* Build a fake network with the QUIC listener bound on listen_port and no
 * relay connected. */
static fake_network_t* _make_network_no_relay(uint16_t listen_port) {
  fake_network_t* fake = (fake_network_t*)get_clear_memory(sizeof(fake_network_t));
  EXPECT_NE(fake, (fake_network_t*)NULL);
  fake->net.quic_listener = &fake->listener;
  fake->listener.listen_port = listen_port;
  fake->net.relay = NULL;
  return fake;
}

/* Build a fake network with a relay connected: reflexive addr/port, relay
 * host/port, local_endpoint_id. The QUIC listener is present so HOST
 * candidates can also be emitted when include_lan is true. */
static fake_network_t* _make_network_with_relay(
    uint16_t listen_port, uint32_t reflexive_addr, uint16_t reflexive_port,
    uint32_t endpoint_id, const char* relay_host, uint16_t relay_port) {
  fake_network_t* fake = (fake_network_t*)get_clear_memory(sizeof(fake_network_t));
  EXPECT_NE(fake, (fake_network_t*)NULL);
  fake->net.quic_listener = &fake->listener;
  fake->listener.listen_port = listen_port;
  fake->net.relay = &fake->relay;
  fake->relay.reflexive_addr = reflexive_addr;
  fake->relay.reflexive_port = reflexive_port;
  fake->relay.local_endpoint_id = endpoint_id;
  /* relay_host is heap-allocated in the real client; strdup here and free
   * in the fixture tear-down. */
  fake->relay.relay_host = relay_host ? strdup(relay_host) : NULL;
  fake->relay.relay_port = relay_port;
  return fake;
}

static void _free_fake_network(fake_network_t* fake) {
  if (fake == NULL) return;
  if (fake->relay.relay_host != NULL) {
    free(fake->relay.relay_host);
    fake->relay.relay_host = NULL;
  }
  free(fake);
}

/* Find the first address of the given type, or NULL. */
static peer_address_t* _find_addr(peer_info_t* info, peer_addr_type_e type) {
  for (size_t index = 0; index < info->address_count; index++) {
    if (info->addresses[index].type == type) return &info->addresses[index];
  }
  return NULL;
}

}  // namespace

/* No relay, no LAN: the function returns 0 and produces zero candidates. */
TEST(TestPeerInfoFromNode, NoRelayNoLanProducesNoCandidates) {
  fake_network_t* fake = _make_network_no_relay(0);
  ASSERT_NE(fake, (fake_network_t*)NULL);

  peer_info_t info;
  memset(&info, 0, sizeof(info));
  int rc = peer_info_from_node(&info, &fake->net, /*include_lan=*/false);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(info.address_count, 0u);
  EXPECT_EQ(info.addresses, (peer_address_t*)NULL);

  peer_info_destroy(&info);
  _free_fake_network(fake);
}

/* include_lan=false with a relay connected: only SRFLX + RELAY candidates
 * are emitted — never HOST. The reflexive address is rendered from host
 * byte order: 0xC0A80101 -> "192.168.1.1". */
TEST(TestPeerInfoFromNode, RelayOnlySkipsLanCandidates) {
  /* 192.168.1.1 in host byte order (most-significant octet first). */
  uint32_t reflexive_addr = 0xC0A80101u;
  fake_network_t* fake = _make_network_with_relay(
      /*listen_port=*/4242, reflexive_addr, /*reflexive_port=*/7777,
      /*endpoint_id=*/42, /*relay_host=*/"relay.example.com",
      /*relay_port=*/443);
  ASSERT_NE(fake, (fake_network_t*)NULL);

  peer_info_t info;
  memset(&info, 0, sizeof(info));
  int rc = peer_info_from_node(&info, &fake->net, /*include_lan=*/false);
  EXPECT_EQ(rc, 0);

  /* No HOST candidates when include_lan is false. */
  EXPECT_EQ(_find_addr(&info, PEER_ADDR_HOST), (peer_address_t*)NULL);

  /* SRFLX candidate carries the reflexive port and the dotted-quad string. */
  peer_address_t* srflx = _find_addr(&info, PEER_ADDR_SRFLX);
  ASSERT_NE(srflx, (peer_address_t*)NULL);
  EXPECT_EQ(srflx->port, 7777u);
  ASSERT_NE(srflx->host, (char*)NULL);
  EXPECT_STREQ(srflx->host, "192.168.1.1");
  EXPECT_EQ(srflx->relay_id, 0u);

  /* RELAY candidate carries the relay host/port + local endpoint id. */
  peer_address_t* relay = _find_addr(&info, PEER_ADDR_RELAY);
  ASSERT_NE(relay, (peer_address_t*)NULL);
  EXPECT_EQ(relay->port, 443u);
  ASSERT_NE(relay->host, (char*)NULL);
  EXPECT_STREQ(relay->host, "relay.example.com");
  EXPECT_EQ(relay->relay_id, 42u);

  peer_info_destroy(&info);
  _free_fake_network(fake);
}

/* include_lan=true: HOST candidates may be emitted (count depends on the
 * test machine's interfaces — at minimum the call must succeed and not
 * crash). SRFLX + RELAY are still emitted alongside. */
TEST(TestPeerInfoFromNode, IncludeLanEmitsHostCandidatesAlongsideRelay) {
  uint32_t reflexive_addr = 0x0A000001u;  /* 10.0.0.1 host byte order */
  fake_network_t* fake = _make_network_with_relay(
      /*listen_port=*/4242, reflexive_addr, /*reflexive_port=*/7777,
      /*endpoint_id=*/7, /*relay_host=*/"relay.offops.io",
      /*relay_port=*/9001);
  ASSERT_NE(fake, (fake_network_t*)NULL);

  peer_info_t info;
  memset(&info, 0, sizeof(info));
  int rc = peer_info_from_node(&info, &fake->net, /*include_lan=*/true);
  EXPECT_EQ(rc, 0);

  /* SRFLX + RELAY are always present when the relay is connected. */
  peer_address_t* srflx = _find_addr(&info, PEER_ADDR_SRFLX);
  ASSERT_NE(srflx, (peer_address_t*)NULL);
  EXPECT_EQ(srflx->port, 7777u);
  ASSERT_NE(srflx->host, (char*)NULL);
  EXPECT_STREQ(srflx->host, "10.0.0.1");

  peer_address_t* relay = _find_addr(&info, PEER_ADDR_RELAY);
  ASSERT_NE(relay, (peer_address_t*)NULL);
  EXPECT_EQ(relay->port, 9001u);
  EXPECT_EQ(relay->relay_id, 7u);

  /* Any HOST candidates that were emitted must carry the listener port. */
  for (size_t index = 0; index < info.address_count; index++) {
    if (info.addresses[index].type == PEER_ADDR_HOST) {
      EXPECT_EQ(info.addresses[index].port, 4242u);
      EXPECT_EQ(info.addresses[index].relay_id, 0u);
    }
  }

  peer_info_destroy(&info);
  _free_fake_network(fake);
}

/* Missing reflexive addr/port (relay connected but ADDR_RESPONSE not yet
 * received): no SRFLX candidate. RELAY still emitted if endpoint id is set. */
TEST(TestPeerInfoFromNode, NoReflexiveAddrSkipsSrflx) {
  fake_network_t* fake = _make_network_with_relay(
      /*listen_port=*/4242, /*reflexive_addr=*/0, /*reflexive_port=*/0,
      /*endpoint_id=*/99, /*relay_host=*/"relay.offops.io",
      /*relay_port=*/9001);
  ASSERT_NE(fake, (fake_network_t*)NULL);

  peer_info_t info;
  memset(&info, 0, sizeof(info));
  int rc = peer_info_from_node(&info, &fake->net, /*include_lan=*/false);
  EXPECT_EQ(rc, 0);

  EXPECT_EQ(_find_addr(&info, PEER_ADDR_SRFLX), (peer_address_t*)NULL);
  peer_address_t* relay = _find_addr(&info, PEER_ADDR_RELAY);
  ASSERT_NE(relay, (peer_address_t*)NULL);
  EXPECT_EQ(relay->relay_id, 99u);

  peer_info_destroy(&info);
  _free_fake_network(fake);
}

/* NULL inputs are rejected. */
TEST(TestPeerInfoFromNode, NullInputsRejected) {
  peer_info_t info;
  memset(&info, 0, sizeof(info));
  EXPECT_EQ(peer_info_from_node(&info, NULL, false), -1);

  fake_network_t* fake = _make_network_no_relay(0);
  ASSERT_NE(fake, (fake_network_t*)NULL);
  EXPECT_EQ(peer_info_from_node(NULL, &fake->net, false), -1);
  _free_fake_network(fake);
}