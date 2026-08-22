#include <gtest/gtest.h>
extern "C" {
#include "Platform/platform_atomic.h"
#include "Util/allocator.h"
}
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

TEST(PlatformAtomicWrite, WritesFileAtomically) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_atomic_test.cbor";
  std::string data = "hello atomic world";
  int rc = platform_file_atomic_write(tmp.c_str(), (const uint8_t*)data.data(), data.size());
  ASSERT_EQ(rc, 0);
  std::ifstream in(tmp, std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(got, data);
  fs::remove(tmp);
}

TEST(PlatformAtomicWrite, OverwritesExistingFile) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_atomic_overwrite.cbor";
  { std::ofstream(tmp) << "old content"; }
  std::string data = "new content that is longer";
  int rc = platform_file_atomic_write(tmp.c_str(), (const uint8_t*)data.data(), data.size());
  ASSERT_EQ(rc, 0);
  std::ifstream in(tmp, std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(got, data);
  fs::remove(tmp);
}

TEST(PlatformAtomicWrite, NoLeftoverTempFile) {
  fs::path tmp = fs::temp_directory_path() / "liboffs_atomic_notmp.cbor";
  std::string data = "no leftover";
  ASSERT_EQ(platform_file_atomic_write(tmp.c_str(), (const uint8_t*)data.data(), data.size()), 0);
  // After a successful write, no .tmp sibling of the target should remain.
  for (auto& entry : fs::directory_iterator(tmp.parent_path())) {
    std::string name = entry.path().filename().string();
    if (name.rfind("liboffs_atomic_notmp", 0) == 0 && name != "liboffs_atomic_notmp.cbor") {
      FAIL() << "Leftover temp file: " << name;
    }
  }
  fs::remove(tmp);
}