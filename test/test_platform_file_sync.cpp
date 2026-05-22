#include <gtest/gtest.h>
extern "C" {
#include "../src/Platform/platform_file.h"
}

TEST(TestPlatformFileSync, SyncOpenFileReturnsZero) {
  platform_file_t* file = platform_file_open("/tmp/test_sync.tmp",
      PLATFORM_O_RDWR | PLATFORM_O_CREAT | PLATFORM_O_TRUNC, 0644);
  ASSERT_NE(file, nullptr);

  const char* data = "hello";
  ssize_t written = platform_file_write(file, data, 5);
  ASSERT_EQ(written, 5);

  int result = platform_file_sync(file);
  EXPECT_EQ(result, 0);

  platform_file_close(file);
  platform_file_unlink("/tmp/test_sync.tmp");
}

TEST(TestPlatformFileSync, SyncNullReturnsNegative) {
  int result = platform_file_sync(NULL);
  EXPECT_EQ(result, -1);
}
