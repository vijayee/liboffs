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

class TestMdns : public ::testing::Test {
 protected:
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "mdns is stubbed on Windows — packet builders return -1";
#endif
  }
};

TEST_F(TestMdns, V4BuilderLayoutUnchanged) {
  uint8_t buf[512];
  int len = mdns_build_announce_v4_for_test("abc", 0x0A000001, 23401,
                                            buf, sizeof(buf));
  ASSERT_GT(len, 0);
  /* Flags live at bytes 2-3 big-endian: 0x8400 → buf[2]=0x84 carries the
     QR (response) bit. */
  EXPECT_EQ(buf[2] & 0x80, 0x80);  /* QR flag */
  uint16_t ancount = (uint16_t)((buf[6] << 8) | buf[7]);
  EXPECT_EQ(ancount, 2);
  /* The A record's 4-byte rdata must contain 10.0.0.1 in network order,
     preceded by TYPE 0x00 0x01 (A) and RDLENGTH 0x00 0x04. */
  bool found_a = false;
  for (size_t i = 0; i + 10 <= (size_t)len; i++) {
    if (buf[i] == 0x0A && buf[i + 1] == 0x00 && buf[i + 2] == 0x00 &&
        buf[i + 3] == 0x01) {
      if (buf[i - 10] == 0x00 && buf[i - 9] == 0x01 &&
          buf[i - 2] == 0x00 && buf[i - 1] == 0x04) { found_a = true; break; }
    }
  }
  EXPECT_TRUE(found_a);
}

TEST_F(TestMdns, V6BuilderEmitsAAAAPlusSrv) {
  uint8_t buf[512];
  uint8_t v6[16];
  memset(v6, 0x2A, sizeof(v6));
  int len = mdns_build_announce_v6_for_test("abc", v6, 23401, buf, sizeof(buf));
  ASSERT_GT(len, 0);
  uint16_t ancount = (uint16_t)((buf[6] << 8) | buf[7]);
  EXPECT_EQ(ancount, 2);
  /* The 16-byte AAAA rdata must appear, preceded by TYPE 0x00 0x1C (AAAA)
     and RDLENGTH 00 10. */
  bool found_aaaa = false;
  for (size_t i = 0; i + 16 <= (size_t)len; i++) {
    if (buf[i] == 0x2A && memcmp(buf + i, v6, 16) == 0) {
      if (buf[i - 10] == 0x00 && buf[i - 9] == 0x1C &&
          buf[i - 2] == 0x00 && buf[i - 1] == 0x10) { found_aaaa = true; break; }
    }
  }
  EXPECT_TRUE(found_aaaa);
}

TEST_F(TestMdns, V4ResponseStillParses) {
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

TEST_F(TestMdns, V6ResponseParsesAAAA) {
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

TEST_F(TestMdns, MalformedAAAARejected) {
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

/* Encode a dotted DNS name the same way _dns_encode_name does in mdns.c:
   each label preceded by its length byte, terminated by 0x00. Returns the
   new offset. */
static size_t test_encode_name(const char* name, uint8_t* buf, size_t offset) {
  const char* cursor = name;
  while (*cursor != '\0') {
    const char* dot = strchr(cursor, '.');
    size_t label_len = (dot != NULL) ? (size_t)(dot - cursor) : strlen(cursor);
    buf[offset] = (uint8_t)label_len;
    offset++;
    memcpy(buf + offset, cursor, label_len);
    offset += label_len;
    if (dot == NULL) break;
    cursor = dot + 1;
  }
  buf[offset] = 0x00;
  offset++;
  return offset;
}

TEST_F(TestMdns, DualFamilyResponseParsesBoth) {
  /* Hand-build a response carrying BOTH an A and an AAAA record:
     header(12, ANCOUNT=3) + name + A(4) + name + AAAA(16) + name + SRV.
     The C-side builders only emit one address record, so this exercises the
     parser filling both out params from a single announce. */
  const char* b58 = "dualpeer";
  const uint32_t expected_ip = 0xC0A80114;  /* 192.168.1.20 */
  const uint8_t expected_v6[16] = {
      0xFD, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
      0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  const uint16_t expected_port = 23401;

  uint8_t buf[512];
  memset(buf, 0, sizeof(buf));
  char name[256];
  snprintf(name, sizeof(name), "%s.offs._tcp.local", b58);

  /* Header: QR=1, AA=1, ANCOUNT=3 (A + AAAA + SRV). */
  buf[2] = 0x84;
  buf[3] = 0x00;
  buf[6] = 0x00;
  buf[7] = 0x03;
  size_t offset = 12;

  /* Record header: TYPE + CLASS(IN|cache-flush) + TTL(120) + RDLENGTH. */
  auto write_record_header = [&](uint16_t record_type, uint16_t rdlength) {
    buf[offset] = (uint8_t)(record_type >> 8);
    buf[offset + 1] = (uint8_t)(record_type & 0xFF);
    offset += 2;
    buf[offset] = 0x80;  /* CLASS IN | cache-flush */
    buf[offset + 1] = 0x01;
    offset += 2;
    buf[offset] = 0x00;  /* TTL 120 big-endian */
    buf[offset + 1] = 0x00;
    buf[offset + 2] = 0x00;
    buf[offset + 3] = 120;
    offset += 4;
    buf[offset] = (uint8_t)(rdlength >> 8);
    buf[offset + 1] = (uint8_t)(rdlength & 0xFF);
    offset += 2;
  };

  /* A record (TYPE 1, RDLENGTH 4). */
  offset = test_encode_name(name, buf, offset);
  write_record_header(1, 4);
  buf[offset] = (uint8_t)(expected_ip >> 24);
  buf[offset + 1] = (uint8_t)(expected_ip >> 16);
  buf[offset + 2] = (uint8_t)(expected_ip >> 8);
  buf[offset + 3] = (uint8_t)(expected_ip & 0xFF);
  offset += 4;

  /* AAAA record (TYPE 28, RDLENGTH 16). */
  offset = test_encode_name(name, buf, offset);
  write_record_header(28, 16);
  memcpy(buf + offset, expected_v6, 16);
  offset += 16;

  /* SRV record (TYPE 33): RDLENGTH filled in after the rdata is written.
     RDATA: priority(2) + weight(2) + port(2) + target(name). */
  offset = test_encode_name(name, buf, offset);
  size_t srv_header_offset = offset;
  write_record_header(33, 0);
  buf[offset] = 0x00;  /* priority */
  buf[offset + 1] = 0x00;
  buf[offset + 2] = 0x00;  /* weight */
  buf[offset + 3] = 0x00;
  buf[offset + 4] = (uint8_t)(expected_port >> 8);
  buf[offset + 5] = (uint8_t)(expected_port & 0xFF);
  offset += 6;
  size_t srv_rdata_start = offset;
  offset = test_encode_name(name, buf, offset);
  uint16_t srv_rdlength = (uint16_t)(offset - srv_rdata_start + 6);
  /* RDLENGTH sits 8 bytes into the record header (TYPE 2 + CLASS 2 + TTL 4). */
  buf[srv_header_offset + 8] = (uint8_t)(srv_rdlength >> 8);
  buf[srv_header_offset + 9] = (uint8_t)(srv_rdlength & 0xFF);

  char b58_out[256];
  uint32_t ip = 0;
  uint16_t port = 0;
  uint8_t addr6[16];
  int have_v6 = 0;
  ASSERT_EQ(mdns_parse_response_for_test(buf, offset, b58_out, sizeof(b58_out),
                                         &ip, &port, addr6, &have_v6), 0);
  EXPECT_STREQ(b58_out, b58);
  EXPECT_EQ(ip, expected_ip);
  EXPECT_EQ(have_v6, 1);
  EXPECT_EQ(memcmp(addr6, expected_v6, 16), 0);
  EXPECT_EQ(port, expected_port);
}
