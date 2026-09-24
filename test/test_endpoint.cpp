//
// Created by victor on 9/24/26.
//

// Tests for the bracket-aware endpoint parser (src/Network/endpoint.c).
#include <gtest/gtest.h>

extern "C" {
#include "Network/endpoint.h"
}

TEST(EndpointTest, ParsesHostPort) {
  char host[64];
  uint16_t port = 0;
  ASSERT_EQ(0, endpoint_parse("10.0.0.1:8080", host, sizeof(host), &port));
  EXPECT_STREQ("10.0.0.1", host);
  EXPECT_EQ(8080, port);
}

TEST(EndpointTest, ParsesHostnamePort) {
  char host[256];
  uint16_t port = 0;
  ASSERT_EQ(0, endpoint_parse("bootstrap.example.com:443", host, sizeof(host), &port));
  EXPECT_STREQ("bootstrap.example.com", host);
  EXPECT_EQ(443, port);
}

TEST(EndpointTest, ParsesBracketedIpv6) {
  char host[64];
  uint16_t port = 0;
  ASSERT_EQ(0, endpoint_parse("[2001:db8::1]:8080", host, sizeof(host), &port));
  EXPECT_STREQ("2001:db8::1", host);
  EXPECT_EQ(8080, port);
}

TEST(EndpointTest, RejectsBareIpv6Literal) {
  // Unbracketed IPv6 literals stay unsupported until the IPv6 cycle.
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("2001:db8::1:8080", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsMissingPort) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("10.0.0.1", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsBracketWithoutPort) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("[2001:db8::1]", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsUnclosedBracket) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("[2001:db8::1:8080", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsEmptyInput) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("", host, sizeof(host), &port));
  EXPECT_NE(0, endpoint_parse(NULL, host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsBadPort) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("10.0.0.1:notaport", host, sizeof(host), &port));
  EXPECT_NE(0, endpoint_parse("10.0.0.1:70000", host, sizeof(host), &port));
  EXPECT_NE(0, endpoint_parse("10.0.0.1:0", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsEmptyBracketHost) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("[]:8080", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsHostTooLongForBuffer) {
  char host[8];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("bootstrap.example.com:443", host, sizeof(host), &port));
}

TEST(EndpointTest, RejectsWhitespaceOnlyInput) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("   ", host, sizeof(host), &port));
}

TEST(EndpointTest, AcceptsTrailingWhitespace) {
  char host[64];
  uint16_t port = 0;
  ASSERT_EQ(0, endpoint_parse("10.0.0.1:8080  ", host, sizeof(host), &port));
  EXPECT_STREQ("10.0.0.1", host);
  EXPECT_EQ(8080, port);
}

TEST(EndpointTest, RejectsSignedAndSpacePrefixedPort) {
  char host[64];
  uint16_t port = 0;
  EXPECT_NE(0, endpoint_parse("10.0.0.1:+8080", host, sizeof(host), &port));
  EXPECT_NE(0, endpoint_parse("10.0.0.1: 8080", host, sizeof(host), &port));
}
