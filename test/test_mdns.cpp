//
// Created by victor on 9/25/26.
//

// Tests for the mDNS packet layer (src/Network/mdns.c): the A/AAAA announce
// builders and the dual-family response parser, exercised through the
// test-only wrappers declared in mdns.h.
#include <gtest/gtest.h>
#include <string.h>

extern "C" {
#include "../src/Network/mdns.h"
#include "../src/Network/node_id.h"
}

TEST(TestMdnsAnnounce, V4BuilderLayoutUnchanged) {
  uint8_t buf[512];
  int len = mdns_build_announce_v4_for_test("abc", 0x0A000001, 23401,
                                            buf, sizeof(buf));
  ASSERT_GT(len, 0);
  /* Flags live at bytes 2-3 big-endian: 0x8400 → buf[2]=0x84 carries the
     QR (response) bit. */
  EXPECT_EQ(buf[2] & 0x80, 0x80);  /* QR flag */
  uint16_t ancount = (uint16_t)((buf[6] << 8) | buf[7]);
  EXPECT_EQ(ancount, 2);
  /* The A record's 4-byte rdata must contain 10.0.0.1 in network order. */
  bool found_a = false;
  for (size_t i = 0; i + 10 <= (size_t)len; i++) {
    if (buf[i] == 0x0A && buf[i + 1] == 0x00 && buf[i + 2] == 0x00 &&
        buf[i + 3] == 0x01) {
      if (buf[i - 2] == 0x00 && buf[i - 1] == 0x04) { found_a = true; break; }
    }
  }
  EXPECT_TRUE(found_a);
}

TEST(TestMdnsAnnounce, V6BuilderEmitsAAAAPlusSrv) {
  uint8_t buf[512];
  uint8_t v6[16];
  memset(v6, 0x2A, sizeof(v6));
  int len = mdns_build_announce_v6_for_test("abc", v6, 23401, buf, sizeof(buf));
  ASSERT_GT(len, 0);
  uint16_t ancount = (uint16_t)((buf[6] << 8) | buf[7]);
  EXPECT_EQ(ancount, 2);
  /* The 16-byte AAAA rdata must appear, preceded by RDLENGTH 00 10. */
  bool found_aaaa = false;
  for (size_t i = 0; i + 16 <= (size_t)len; i++) {
    if (buf[i] == 0x2A && memcmp(buf + i, v6, 16) == 0) {
      if (buf[i - 2] == 0x00 && buf[i - 1] == 0x10) { found_aaaa = true; break; }
    }
  }
  EXPECT_TRUE(found_aaaa);
}

TEST(TestMdnsParse, V4ResponseStillParses) {
  uint8_t buf[512];
  int len = mdns_build_announce_v4_for_test("peerid", 0x0A000001, 23401,
                                            buf, sizeof(buf));
  ASSERT_GT(len, 0);
  char b58[256];
  uint32_t ip = 0;
  uint16_t port = 0;
  uint8_t addr6[16];
  int have_v6 = 0;
  ASSERT_EQ(mdns_parse_response_for_test(buf, (size_t)len, b58, sizeof(b58),
                                         &ip, &port, addr6, &have_v6), 0);
  EXPECT_STREQ(b58, "peerid");
  EXPECT_EQ(ip, 0x0A000001u);
  EXPECT_EQ(port, 23401);
  EXPECT_EQ(have_v6, 0);
}

TEST(TestMdnsParse, V6ResponseParsesAAAA) {
  uint8_t buf[512];
  uint8_t v6[16];
  memset(v6, 0x2B, sizeof(v6));
  int len = mdns_build_announce_v6_for_test("peerid6", v6, 23401,
                                            buf, sizeof(buf));
  ASSERT_GT(len, 0);
  char b58[256];
  uint32_t ip = 0;
  uint16_t port = 0;
  uint8_t addr6[16];
  int have_v6 = 0;
  ASSERT_EQ(mdns_parse_response_for_test(buf, (size_t)len, b58, sizeof(b58),
                                         &ip, &port, addr6, &have_v6), 0);
  EXPECT_STREQ(b58, "peerid6");
  EXPECT_EQ(have_v6, 1);
  EXPECT_EQ(memcmp(addr6, v6, 16), 0);
  EXPECT_EQ(ip, 0u);
  EXPECT_EQ(port, 23401);
}

TEST(TestMdnsParse, MalformedAAAARejected) {
  /* AAAA with rdlength != 16 must be ignored (no usable addr → parse fails). */
  uint8_t buf[512];
  const uint8_t zero_v6[16] = {0};
  int len = mdns_build_announce_v6_for_test("peerid6", zero_v6, 23401,
                                            buf, sizeof(buf));
  ASSERT_GT(len, 0);
  ASSERT_GT(len, 30);
  /* Find the AAAA record: search for type 28 big-endian (0x00 0x1C). */
  for (size_t i = 12; i + 10 <= (size_t)len; i++) {
    if (buf[i] == 0x00 && buf[i + 1] == 0x1C) {
      buf[i + 8] = 0x00;  /* RDLENGTH high byte */
      buf[i + 9] = 0x04;  /* RDLENGTH low byte — wrong for AAAA */
      break;
    }
  }
  char b58[256];
  uint32_t ip = 0;
  uint16_t port = 0;
  uint8_t addr6[16];
  int have_v6 = 0;
  EXPECT_NE(mdns_parse_response_for_test(buf, (size_t)len, b58, sizeof(b58),
                                         &ip, &port, addr6, &have_v6), 0);
}