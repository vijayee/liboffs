//
// Created by victor on 9/24/26.
//

#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "../src/Network/net_node.h"
#include "../src/Util/allocator.h"
}

TEST(TestNetNodeAddr, SetPlatformAddrV4StoresHostByteOrder) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET;
  addr.inet.addr = 0x0100007F;  /* 127.0.0.1, network byte order */
  addr.inet.port = 23401;
  net_node_set_platform_addr(node, &addr);
  EXPECT_EQ(node->addr_family, PLATFORM_AF_INET);
  EXPECT_EQ(node->addr, 0x7F000001u);  /* host byte order, matches existing call sites */
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, SetPlatformAddrV4MappedCollapsesToV4) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET6;
  addr.inet6.addr[10] = 0xFF;
  addr.inet6.addr[11] = 0xFF;
  addr.inet6.addr[12] = 192;
  addr.inet6.addr[15] = 2;
  net_node_set_platform_addr(node, &addr);
  EXPECT_EQ(node->addr_family, PLATFORM_AF_INET);
  EXPECT_EQ(node->addr, 0xC0000002u);  /* 192.0.0.2 host byte order */
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, SetPlatformAddrV6StoresBytes) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET6;
  for (int i = 0; i < 16; i++) addr.inet6.addr[i] = (uint8_t)(i + 1);
  net_node_set_platform_addr(node, &addr);
  EXPECT_EQ(node->addr_family, PLATFORM_AF_INET6);
  EXPECT_EQ(node->addr, 0u);
  EXPECT_EQ(memcmp(node->addr6, addr.inet6.addr, 16), 0);
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, RoundTripToPlatformAddr) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t in;
  memset(&in, 0, sizeof(in));
  in.family = PLATFORM_AF_INET6;
  for (int i = 0; i < 16; i++) in.inet6.addr[i] = (uint8_t)(0x20 + i);
  net_node_set_platform_addr(node, &in);
  platform_address_t out;
  net_node_get_platform_addr(node, &out);
  EXPECT_EQ(out.family, PLATFORM_AF_INET6);
  EXPECT_EQ(memcmp(out.inet6.addr, in.inet6.addr, 16), 0);
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, AddrStringV6BracketedAndBare) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t in;
  memset(&in, 0, sizeof(in));
  in.family = PLATFORM_AF_INET6;
  in.inet6.addr[15] = 1;  /* ::1 */
  net_node_set_platform_addr(node, &in);
  char buf[64];
  EXPECT_EQ(net_node_addr_string(node, true, buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "[::1]");
  EXPECT_EQ(net_node_addr_string(node, false, buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "::1");
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, AddrStringV4) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t in;
  memset(&in, 0, sizeof(in));
  in.family = PLATFORM_AF_INET;
  in.inet.addr = 0x0100007F;  /* network byte order 127.0.0.1 */
  net_node_set_platform_addr(node, &in);
  char buf[64];
  EXPECT_EQ(net_node_addr_string(node, false, buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "127.0.0.1");
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, SetRendvPlatformV4MappedCollapses) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET6;
  addr.inet6.addr[10] = 0xFF;
  addr.inet6.addr[11] = 0xFF;
  addr.inet6.addr[12] = 192;
  addr.inet6.addr[15] = 2;
  net_node_set_rendv_platform(node, &addr);
  EXPECT_EQ(node->rendv_family, PLATFORM_AF_INET);
  EXPECT_EQ(node->rendv_addr, 0xC0000002u);  /* 192.0.0.2, host byte order */
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, SetRendvPlatformV6StoresBytes) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET6;
  for (int i = 0; i < 16; i++) addr.inet6.addr[i] = (uint8_t)(i + 1);
  net_node_set_rendv_platform(node, &addr);
  EXPECT_EQ(node->rendv_family, PLATFORM_AF_INET6);
  EXPECT_EQ(memcmp(node->rendv6, addr.inet6.addr, 16), 0);
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, AddrlessNodeStringFails) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  char buf[64];
  EXPECT_NE(net_node_addr_string(node, false, buf, sizeof(buf)), 0);
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, V4MappedRoundTripsToCorrectString) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t in;
  memset(&in, 0, sizeof(in));
  in.family = PLATFORM_AF_INET6;
  in.inet6.addr[10] = 0xFF;
  in.inet6.addr[11] = 0xFF;
  in.inet6.addr[12] = 192;
  in.inet6.addr[15] = 2;
  net_node_set_platform_addr(node, &in);
  char buf[64];
  EXPECT_EQ(net_node_addr_string(node, false, buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "192.0.0.2");
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, ScopedV6AddressCarriesScope) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t in;
  memset(&in, 0, sizeof(in));
  in.family = PLATFORM_AF_INET6;
  in.inet6.addr[0] = 0xFE;
  in.inet6.addr[1] = 0x80;
  in.inet6.addr[15] = 1;
  in.inet6.scope_id = 7;
  net_node_set_platform_addr(node, &in);
  EXPECT_EQ(node->scope_valid, 1);
  EXPECT_EQ(node->scope_id, in.inet6.scope_id);
  platform_address_t out;
  net_node_get_platform_addr(node, &out);
  EXPECT_EQ(out.inet6.scope_id, in.inet6.scope_id);
  char buf[128];
  EXPECT_EQ(net_node_addr_string(node, false, buf, sizeof(buf)), 0);
  /* Emission carries the %zone (name or numeric fallback). */
  EXPECT_EQ(strncmp(buf, "fe80::1%", 8), 0);
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, UnscopedV6HasNoScope) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t in;
  memset(&in, 0, sizeof(in));
  in.family = PLATFORM_AF_INET6;
  in.inet6.addr[15] = 1;  /* ::1 */
  net_node_set_platform_addr(node, &in);
  EXPECT_EQ(node->scope_valid, 0);
  EXPECT_EQ(node->scope_id, 0u);
  char buf[64];
  EXPECT_EQ(net_node_addr_string(node, false, buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "::1");
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, ScopedThenUnscopedClearsScope) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t scoped;
  memset(&scoped, 0, sizeof(scoped));
  scoped.family = PLATFORM_AF_INET6;
  scoped.inet6.addr[0] = 0xFE;
  scoped.inet6.addr[1] = 0x80;
  scoped.inet6.addr[15] = 1;
  scoped.inet6.scope_id = 7;
  net_node_set_platform_addr(node, &scoped);
  ASSERT_EQ(node->scope_valid, 1);

  platform_address_t unscoped;
  memset(&unscoped, 0, sizeof(unscoped));
  unscoped.family = PLATFORM_AF_INET6;
  unscoped.inet6.addr[15] = 1;  /* ::1 */
  net_node_set_platform_addr(node, &unscoped);
  EXPECT_EQ(node->scope_valid, 0);
  EXPECT_EQ(node->scope_id, 0u);

  /* And a switch back to v4 also clears. */
  platform_address_t v4;
  memset(&v4, 0, sizeof(v4));
  v4.family = PLATFORM_AF_INET;
  v4.inet.addr = 0x0100007F;
  net_node_set_platform_addr(node, &v4);
  EXPECT_EQ(node->scope_valid, 0);
  EXPECT_EQ(node->scope_id, 0u);
  net_node_destroy(node);
}

TEST(TestNetNodeAddr, RendvScopeStored) {
  net_node_t* node = net_node_create_unidentified(0, 0);
  platform_address_t in;
  memset(&in, 0, sizeof(in));
  in.family = PLATFORM_AF_INET6;
  in.inet6.addr[0] = 0xFE;
  in.inet6.addr[1] = 0x80;
  in.inet6.scope_id = 7;
  net_node_set_rendv_platform(node, &in);
  EXPECT_EQ(node->rendv_scope_valid, 1);
  EXPECT_EQ(node->rendv_scope_id, 7u);
  net_node_destroy(node);
}
