//
// Created by victor on 5/16/26.
//

#include <gtest/gtest.h>

extern "C" {
#include "Network/nat_detect.h"
}

// === NAT classification logic tests ===
// These test the pure classification function directly without needing QUIC connections.

TEST(NatDetect, ClassifyOpenNotBehindNat) {
  // local matches reflexive address from relay A
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

TEST(NatDetect, ClassifyOpenSingleRelay) {
  // Only one relay responds, and local matches reflexive
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_a = 0xC0A80101;
  uint16_t reflexive_port_a = 12345;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      0, 0, 0);
  EXPECT_EQ(result, NAT_TYPE_OPEN);
}

TEST(NatDetect, ClassifyFullCone) {
  // Both reflexive addresses match, same port
  uint32_t local_addr = 0xC0A80101;      // 192.168.1.1
  uint32_t reflexive_addr_a = 0x01020304;  // 1.2.3.4
  uint16_t reflexive_port_a = 50000;
  uint32_t reflexive_addr_b = 0x01020304;
  uint16_t reflexive_port_b = 50000;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_FULL_CONE);
}

TEST(NatDetect, ClassifySymmetricDifferentAddr) {
  // Different reflexive addresses from two relays
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_a = 0x01020304;  // 1.2.3.4
  uint16_t reflexive_port_a = 50000;
  uint32_t reflexive_addr_b = 0x05060708;  // 5.6.7.8
  uint16_t reflexive_port_b = 50001;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_SYMMETRIC);
}

TEST(NatDetect, ClassifySymmetricSameAddrDifferentPort) {
  // Same reflexive address but different port — symmetric NAT
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_a = 0x01020304;
  uint16_t reflexive_port_a = 50000;
  uint32_t reflexive_addr_b = 0x01020304;
  uint16_t reflexive_port_b = 50001;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_SYMMETRIC);
}

TEST(NatDetect, ClassifyPortRestrictedConeSingleRelay) {
  // Only one relay responds, local doesn't match reflexive
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_a = 0x01020304;
  uint16_t reflexive_port_a = 50000;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      0, 0, 0);
  EXPECT_EQ(result, NAT_TYPE_PORT_RESTRICTED_CONE);
}

TEST(NatDetect, ClassifyPortRestrictedConeRelayBOnly) {
  // Only relay B responds, local doesn't match reflexive
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_b = 0x01020304;
  uint16_t reflexive_port_b = 50000;

  nat_type_e result = nat_detect_classify(
      local_addr,
      0, 0, 0,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_PORT_RESTRICTED_CONE);
}

TEST(NatDetect, ClassifyUnknownNoResponses) {
  // No relay responses at all
  nat_type_e result = nat_detect_classify(
      0xC0A80101,
      0, 0, 0,
      0, 0, 0);
  EXPECT_EQ(result, NAT_TYPE_UNKNOWN);
}

TEST(NatDetect, ClassifyOpenOnlyRelayB) {
  // Only relay B responds, and it matches local
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_b = 0xC0A80101;

  nat_type_e result = nat_detect_classify(
      local_addr,
      0, 0, 0,
      reflexive_addr_b, 12345, 1);
  EXPECT_EQ(result, NAT_TYPE_OPEN);
}

TEST(NatDetect, CreateDestroyNoLeak) {
  // Verify create/destroy doesn't crash or leak
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  nat_detect_t* detect = nat_detect_create(NULL, pool);
  // Without QUIC, create returns NULL — that's expected in stub mode
  // But the function shouldn't crash
  nat_detect_destroy(detect);

  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(NatDetect, GetTypeReturnsUnknownOnNull) {
  nat_type_e result = nat_detect_get_type(NULL);
  EXPECT_EQ(result, NAT_TYPE_UNKNOWN);
}

TEST(NatDetect, StartReturnsErrorOnNull) {
  int result = nat_detect_start(NULL, "127.0.0.1", 7000, "127.0.0.1", 7001);
  EXPECT_EQ(result, -1);
}

TEST(NatDetect, ClassifyFullConeDistinctPorts) {
  // Verify that same address + same port is FULL_CONE even if different from expected
  uint32_t local_addr = 0xC0A80101;
  uint32_t reflexive_addr_a = 0x01020304;
  uint16_t reflexive_port_a = 50000;
  uint32_t reflexive_addr_b = 0x01020304;
  uint16_t reflexive_port_b = 50000;

  nat_type_e result = nat_detect_classify(
      local_addr,
      reflexive_addr_a, reflexive_port_a, 1,
      reflexive_addr_b, reflexive_port_b, 1);
  EXPECT_EQ(result, NAT_TYPE_FULL_CONE);
}