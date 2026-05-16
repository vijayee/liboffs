//
// Created by victor on 5/16/26.
//

#include <gtest/gtest.h>

extern "C" {
#include "Network/conn_state.h"
#include "Network/peer_connection.h"
#include "Network/nat_detect.h"
#include "Actor/actor.h"
#include "Actor/message.h"
#include "Scheduler/scheduler.h"
#include <cbor.h>
}

/* Helper: create a minimal peer_connection_t for testing.
 * We bypass peer_connection_create because it allocates
 * eabf/timing_wheel, which requires a full scheduler. */
static peer_connection_t* test_peer_create() {
  peer_connection_t* peer = (peer_connection_t*)calloc(1, sizeof(peer_connection_t));
  EXPECT_NE(peer, nullptr);
  peer->conn_state = CONN_STATE_RELAY;
  peer->direct_path.active = 0;
  peer->relay_path.active = 0;
  peer->peer_nat_type = NAT_TYPE_UNKNOWN;
  peer->direct_attempts = 0;
  peer->connected = true;
  return peer;
}

static void test_peer_destroy(peer_connection_t* peer) {
  if (peer != nullptr) {
    free(peer);
  }
}

/* === conn_state_init tests === */

TEST(ConnState, InitOpenReturnsTryingDirect) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_OPEN), CONN_STATE_TRYING_DIRECT);
}

TEST(ConnState, InitFullConeReturnsTryingDirect) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_FULL_CONE), CONN_STATE_TRYING_DIRECT);
}

TEST(ConnState, InitRestrictedConeReturnsTryingDirect) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_RESTRICTED_CONE), CONN_STATE_TRYING_DIRECT);
}

TEST(ConnState, InitPortRestrictedConeReturnsTryingDirect) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_PORT_RESTRICTED_CONE), CONN_STATE_TRYING_DIRECT);
}

TEST(ConnState, InitSymmetricReturnsRelayOnly) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_SYMMETRIC), CONN_STATE_RELAY_ONLY);
}

TEST(ConnState, InitUnknownReturnsRelay) {
  EXPECT_EQ(conn_state_init(NAT_TYPE_UNKNOWN), CONN_STATE_RELAY);
}

/* === conn_state_get tests === */

TEST(ConnState, GetReturnsCurrentState) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_DIRECT;
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_DIRECT);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_TRYING_DIRECT);

  peer->conn_state = CONN_STATE_RELAY;
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_RELAY);

  peer->conn_state = CONN_STATE_RELAY_ONLY;
  EXPECT_EQ(conn_state_get(peer), CONN_STATE_RELAY_ONLY);

  test_peer_destroy(peer);
}

TEST(ConnState, GetReturnsRelayOnNull) {
  EXPECT_EQ(conn_state_get(NULL), CONN_STATE_RELAY);
}

/* === conn_state_on_direct_connected tests === */

TEST(ConnState, DirectConnectedFromTryingDirect) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  conn_state_on_direct_connected(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_DIRECT);
  EXPECT_EQ(peer->direct_path.active, 1);

  test_peer_destroy(peer);
}

TEST(ConnState, DirectConnectedFromRelay) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_RELAY;
  conn_state_on_direct_connected(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_DIRECT);
  EXPECT_EQ(peer->direct_path.active, 1);

  test_peer_destroy(peer);
}

TEST(ConnState, DirectConnectedOnNullDoesNotCrash) {
  conn_state_on_direct_connected(NULL);
}

/* === conn_state_on_direct_failed tests === */

TEST(ConnState, DirectFailedFromTryingDirect) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  peer->direct_path.active = 1;
  conn_state_on_direct_failed(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY);
  EXPECT_EQ(peer->direct_path.active, 0);

  test_peer_destroy(peer);
}

TEST(ConnState, DirectFailedFromDirect) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_DIRECT;
  peer->direct_path.active = 1;
  conn_state_on_direct_failed(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY);
  EXPECT_EQ(peer->direct_path.active, 0);

  test_peer_destroy(peer);
}

TEST(ConnState, DirectFailedFromRelayOnlyDoesNotChangeState) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_RELAY_ONLY;
  peer->direct_path.active = 0;
  conn_state_on_direct_failed(peer);
  /* RELAY_ONLY must NOT change — symmetric NAT makes direct impossible */
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY_ONLY);

  test_peer_destroy(peer);
}

TEST(ConnState, DirectFailedOnNullDoesNotCrash) {
  conn_state_on_direct_failed(NULL);
}

/* === conn_state_set_peer_nat_type tests === */

TEST(ConnState, SetPeerNatTypeSymmetricForcesRelayOnly) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  peer->direct_path.active = 1;
  conn_state_set_peer_nat_type(peer, NAT_TYPE_SYMMETRIC);
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY_ONLY);
  EXPECT_EQ(peer->direct_path.active, 0);
  EXPECT_EQ(peer->peer_nat_type, NAT_TYPE_SYMMETRIC);

  test_peer_destroy(peer);
}

TEST(ConnState, SetPeerNatTypeNonSymmetricDoesNotForceRelayOnly) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  conn_state_set_peer_nat_type(peer, NAT_TYPE_FULL_CONE);
  EXPECT_EQ(peer->conn_state, CONN_STATE_TRYING_DIRECT);
  EXPECT_EQ(peer->peer_nat_type, NAT_TYPE_FULL_CONE);

  test_peer_destroy(peer);
}

TEST(ConnState, SetPeerNatTypeOnNullDoesNotCrash) {
  conn_state_set_peer_nat_type(NULL, NAT_TYPE_SYMMETRIC);
}

/* === conn_state_upgrade_to_direct tests === */

TEST(ConnState, UpgradeFromRelayToTryingDirect) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_RELAY;
  conn_state_upgrade_to_direct(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_TRYING_DIRECT);

  test_peer_destroy(peer);
}

TEST(ConnState, UpgradeFromRelayOnlyDoesNotChange) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_RELAY_ONLY;
  conn_state_upgrade_to_direct(peer);
  /* RELAY_ONLY must NEVER upgrade — symmetric NAT */
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY_ONLY);

  test_peer_destroy(peer);
}

TEST(ConnState, UpgradeFromDirectDoesNotChange) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_DIRECT;
  conn_state_upgrade_to_direct(peer);
  /* Already direct, no change needed */
  EXPECT_EQ(peer->conn_state, CONN_STATE_DIRECT);

  test_peer_destroy(peer);
}

TEST(ConnState, UpgradeFromTryingDirectDoesNotChange) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  conn_state_upgrade_to_direct(peer);
  /* Already trying direct, no change needed */
  EXPECT_EQ(peer->conn_state, CONN_STATE_TRYING_DIRECT);

  test_peer_destroy(peer);
}

TEST(ConnState, UpgradeOnNullDoesNotCrash) {
  conn_state_upgrade_to_direct(NULL);
}

/* === conn_state_should_try_direct tests === */

TEST(ConnState, ShouldTryDirectOnlyWhenTryingDirect) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  EXPECT_EQ(conn_state_should_try_direct(peer), 1);

  peer->conn_state = CONN_STATE_DIRECT;
  EXPECT_EQ(conn_state_should_try_direct(peer), 0);

  peer->conn_state = CONN_STATE_RELAY;
  EXPECT_EQ(conn_state_should_try_direct(peer), 0);

  peer->conn_state = CONN_STATE_RELAY_ONLY;
  EXPECT_EQ(conn_state_should_try_direct(peer), 0);

  test_peer_destroy(peer);
}

TEST(ConnState, ShouldTryDirectOnNullReturnsZero) {
  EXPECT_EQ(conn_state_should_try_direct(NULL), 0);
}

/* === conn_state_send null-safety tests === */

TEST(ConnState, SendReturnsErrorOnNullPeer) {
  network_t network = {};
  cbor_item_t* msg = cbor_build_uint8(42);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(conn_state_send(&network, NULL, msg), -1);
  cbor_decref(&msg);
}

TEST(ConnState, SendReturnsErrorOnNullCbor) {
  network_t network = {};
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);
  EXPECT_EQ(conn_state_send(&network, peer, NULL), -1);
  test_peer_destroy(peer);
}

TEST(ConnState, SendReturnsErrorOnNullNetwork) {
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);
  cbor_item_t* msg = cbor_build_uint8(42);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(conn_state_send(NULL, peer, msg), -1);
  cbor_decref(&msg);
  test_peer_destroy(peer);
}

/* === State transition sequence test === */

TEST(ConnState, FullTransitionSequence) {
  /* Simulate a full lifecycle:
   * 1. Start in RELAY (unknown NAT)
   * 2. NAT detect completes → upgrade to TRYING_DIRECT
   * 3. Direct connection succeeds → DIRECT
   * 4. Direct connection fails → RELAY
   * 5. Re-attempt → TRYING_DIRECT
   * 6. Direct succeeds again → DIRECT
   */
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  /* 1. Start in RELAY */
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY);

  /* 2. Upgrade to TRYING_DIRECT */
  conn_state_upgrade_to_direct(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_TRYING_DIRECT);
  EXPECT_EQ(conn_state_should_try_direct(peer), 1);

  /* 3. Direct connection succeeds */
  conn_state_on_direct_connected(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_DIRECT);
  EXPECT_EQ(peer->direct_path.active, 1);
  EXPECT_EQ(conn_state_should_try_direct(peer), 0);

  /* 4. Direct connection fails */
  conn_state_on_direct_failed(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY);
  EXPECT_EQ(peer->direct_path.active, 0);

  /* 5. Re-attempt direct */
  conn_state_upgrade_to_direct(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_TRYING_DIRECT);

  /* 6. Direct succeeds again */
  conn_state_on_direct_connected(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_DIRECT);

  test_peer_destroy(peer);
}

TEST(ConnState, SymmetricNatBlocksAllDirectAttempts) {
  /* Once a peer is detected as symmetric NAT, all direct attempts are blocked */
  peer_connection_t* peer = test_peer_create();
  ASSERT_NE(peer, nullptr);

  peer->conn_state = CONN_STATE_TRYING_DIRECT;
  peer->direct_path.active = 1;

  /* Remote peer detected as symmetric NAT */
  conn_state_set_peer_nat_type(peer, NAT_TYPE_SYMMETRIC);
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY_ONLY);
  EXPECT_EQ(peer->direct_path.active, 0);

  /* Upgrade attempt is blocked */
  conn_state_upgrade_to_direct(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY_ONLY);

  /* Direct failure doesn't change state */
  conn_state_on_direct_failed(peer);
  EXPECT_EQ(peer->conn_state, CONN_STATE_RELAY_ONLY);

  /* Should not try direct */
  EXPECT_EQ(conn_state_should_try_direct(peer), 0);

  test_peer_destroy(peer);
}