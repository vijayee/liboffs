#include <gtest/gtest.h>
extern "C" {
#include "../src/Util/validation.h"
}

TEST(TestValidation, ContentTypeNullRejected) {
  EXPECT_NE(validate_content_type(NULL), 0);
}

TEST(TestValidation, ContentTypeEmptyRejected) {
  EXPECT_NE(validate_content_type(""), 0);
}

TEST(TestValidation, ContentTypeTooLongRejected) {
  char long_type[258];
  memset(long_type, 'a', 257);
  long_type[257] = '\0';
  EXPECT_NE(validate_content_type(long_type), 0);
}

TEST(TestValidation, ContentTypeMaxLengthPasses) {
  char max_type[257];
  memset(max_type, 'a', 256);
  max_type[256] = '\0';
  EXPECT_EQ(validate_content_type(max_type), 0);
}

TEST(TestValidation, ContentTypeTypicalPasses) {
  EXPECT_EQ(validate_content_type("application/octet-stream"), 0);
  EXPECT_EQ(validate_content_type("text/plain"), 0);
  EXPECT_EQ(validate_content_type("image/png"), 0);
}

TEST(TestValidation, ContentTypeWithPlusDotDashPasses) {
  EXPECT_EQ(validate_content_type("application/vnd.api+json"), 0);
  EXPECT_EQ(validate_content_type("application/x.whatever"), 0);
  EXPECT_EQ(validate_content_type("type-subtype"), 0);
}

TEST(TestValidation, ContentTypeNonPrintableRejected) {
  EXPECT_NE(validate_content_type("text/\x01plain"), 0);
}

TEST(TestValidation, FileNameNullRejected) {
  EXPECT_NE(validate_file_name(NULL), 0);
}

TEST(TestValidation, FileNameEmptyRejected) {
  EXPECT_NE(validate_file_name(""), 0);
}

TEST(TestValidation, FileNameTooLongRejected) {
  char long_name[1026];
  memset(long_name, 'a', 1025);
  long_name[1025] = '\0';
  EXPECT_NE(validate_file_name(long_name), 0);
}

TEST(TestValidation, FileNameMaxLengthPasses) {
  char max_name[1025];
  memset(max_name, 'a', 1024);
  max_name[1024] = '\0';
  EXPECT_EQ(validate_file_name(max_name), 0);
}

TEST(TestValidation, FileNameTypicalPasses) {
  EXPECT_EQ(validate_file_name("document.pdf"), 0);
  EXPECT_EQ(validate_file_name("image.jpeg"), 0);
  EXPECT_EQ(validate_file_name("readme.txt"), 0);
}

TEST(TestValidation, FileNameDotDotRejected) {
  EXPECT_NE(validate_file_name("../etc/passwd"), 0);
  EXPECT_NE(validate_file_name("..hidden"), 0);
}

TEST(TestValidation, FileNameSlashRejected) {
  EXPECT_NE(validate_file_name("path/file.txt"), 0);
}

TEST(TestValidation, FileNameNonPrintableRejected) {
  EXPECT_NE(validate_file_name("file\x01.txt"), 0);
}

TEST(TestValidation, OriStringNullRejected) {
  EXPECT_NE(validate_ori_string(NULL), 0);
}

TEST(TestValidation, OriStringEmptyRejected) {
  EXPECT_NE(validate_ori_string(""), 0);
}

TEST(TestValidation, OriStringTooLongRejected) {
  char long_ori[2050];
  memset(long_ori, 'a', 2049);
  long_ori[2049] = '\0';
  EXPECT_NE(validate_ori_string(long_ori), 0);
}

TEST(TestValidation, OriStringMissingOffsystemRejected) {
  EXPECT_NE(validate_ori_string("https://example.com/file"), 0);
}

TEST(TestValidation, OriStringMissingPrefixRejected) {
  EXPECT_NE(validate_ori_string("/offsystem/v3/hash"), 0);
  EXPECT_NE(validate_ori_string("ftp://example.com/offsystem/v3/hash"), 0);
}

TEST(TestValidation, OriStringHttpsPasses) {
  EXPECT_EQ(validate_ori_string(
      "https://localhost:23402/offsystem/v3/text/plain/100/hash1/hash2/file.txt"), 0);
}

TEST(TestValidation, OriStringHttpPasses) {
  EXPECT_EQ(validate_ori_string(
      "http://localhost:23402/offsystem/v3/text/plain/100/hash/hash/file.txt"), 0);
}
