#include <gtest/gtest.h>
extern "C" {
#include "../src/Version/version.h"
}

TEST(TestVersion, ParseStable) {
  version_t v;
  ASSERT_TRUE(version_parse("v1.2.3", &v));
  EXPECT_EQ(v.major, 1);
  EXPECT_EQ(v.minor, 2);
  EXPECT_EQ(v.patch, 3);
  EXPECT_STREQ(v.prerelease, "");
}

TEST(TestVersion, ParsePrerelease) {
  version_t v;
  ASSERT_TRUE(version_parse("v2.0.0-rc.1", &v));
  EXPECT_EQ(v.major, 2);
  EXPECT_EQ(v.minor, 0);
  EXPECT_EQ(v.patch, 0);
  EXPECT_STREQ(v.prerelease, "rc.1");
}

TEST(TestVersion, ParseDev) {
  version_t v;
  ASSERT_TRUE(version_parse("v0.9.0-dev.5", &v));
  EXPECT_EQ(v.major, 0);
  EXPECT_EQ(v.minor, 9);
  EXPECT_EQ(v.patch, 0);
  EXPECT_STREQ(v.prerelease, "dev.5");
}

TEST(TestVersion, ParseWithoutV) {
  version_t v;
  ASSERT_TRUE(version_parse("3.4.5", &v));
  EXPECT_EQ(v.major, 3);
  EXPECT_EQ(v.minor, 4);
  EXPECT_EQ(v.patch, 5);
}

TEST(TestVersion, ParseInvalid) {
  version_t v;
  EXPECT_FALSE(version_parse(NULL, &v));
  EXPECT_FALSE(version_parse("not-a-version", &v));
  EXPECT_FALSE(version_parse("v1.2", &v));
}

TEST(TestVersion, CompareEqual) {
  version_t a, b;
  version_parse("v1.0.0", &a);
  version_parse("v1.0.0", &b);
  EXPECT_EQ(version_compare(&a, &b), 0);
}

TEST(TestVersion, CompareMajor) {
  version_t a, b;
  version_parse("v2.0.0", &a);
  version_parse("v1.9.9", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
  EXPECT_EQ(version_compare(&b, &a), -1);
}

TEST(TestVersion, CompareMinor) {
  version_t a, b;
  version_parse("v1.3.0", &a);
  version_parse("v1.2.9", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
}

TEST(TestVersion, ComparePatch) {
  version_t a, b;
  version_parse("v1.0.2", &a);
  version_parse("v1.0.1", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
}

TEST(TestVersion, StableOverPrerelease) {
  version_t a, b;
  version_parse("v1.0.0", &a);
  version_parse("v1.0.0-rc.1", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
  EXPECT_EQ(version_compare(&b, &a), -1);
}

TEST(TestVersion, ComparePrerelease) {
  version_t a, b;
  version_parse("v1.0.0-rc.2", &a);
  version_parse("v1.0.0-rc.1", &b);
  EXPECT_EQ(version_compare(&a, &b), 1);
}

TEST(TestVersion, ToStringStable) {
  version_t v;
  version_parse("v1.2.3", &v);
  char buf[64];
  version_to_string(&v, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1.2.3");
}

TEST(TestVersion, ToStringPrerelease) {
  version_t v;
  version_parse("v1.2.3-rc.1", &v);
  char buf[64];
  version_to_string(&v, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1.2.3-rc.1");
}

TEST(TestVersion, ChannelDetection) {
  version_t stable, rc, dev;
  version_parse("v1.0.0", &stable);
  version_parse("v1.0.0-rc.1", &rc);
  version_parse("v1.0.0-dev.3", &dev);

  EXPECT_EQ(version_channel(&stable), channel_stable);
  EXPECT_EQ(version_channel(&rc), channel_rc);
  EXPECT_EQ(version_channel(&dev), channel_dev);
}

TEST(TestVersion, OFFS_VERSION_defined) {
  EXPECT_STRNE(OFFS_VERSION, "");
}
