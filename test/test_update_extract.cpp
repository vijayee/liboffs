#include <gtest/gtest.h>

extern "C" {
#include "Update/update_extract.h"
#include "Util/mkdir_p.h"
#include "Util/rm_rf.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

/* ---------------------------------------------------------------------------
 * Minimal USTAR tar builder for the containment tests.
 *
 * Each entry is (name, typeflag, content, linkname). For directories and
 * rejected types, content is empty (no data blocks are written). The builder
 * writes a 512-byte USTAR header per entry plus the file data padded to a
 * 512-byte boundary, then two zero blocks as the end-of-archive marker.
 *
 * The header checksum field is left as 8 spaces. The reader (update_extract)
 * does NOT validate checksums — the archive's overall SHA256 is verified
 * against the signed manifest before extraction, which is a stronger
 * integrity guarantee than per-header checksums. This keeps the test helper
 * simple and matches the production reader's leniency.
 * --------------------------------------------------------------------------- */

static std::vector<uint8_t> ustar_header(const std::string& name,
                                         char typeflag,
                                         uint64_t size,
                                         const std::string& linkname = "") {
  std::vector<uint8_t> hdr(512, 0);
  /* name[100] + prefix[155]: for short names, everything goes in name. Long
     names (rare in tests) split into prefix/basename at a '/' boundary. */
  if (name.size() <= 100) {
    memcpy(hdr.data() + 0, name.c_str(), name.size());
  } else {
    size_t split_at = name.size() - 100;
    split_at = name.find('/', split_at);
    if (split_at == std::string::npos || split_at > 155) {
      memcpy(hdr.data() + 0, name.c_str(), 100);
    } else {
      std::string prefix_part = name.substr(0, split_at);
      std::string base_part = name.substr(split_at + 1);
      memcpy(hdr.data() + 345, prefix_part.c_str(),
             std::min(prefix_part.size(), (size_t)155));
      memcpy(hdr.data() + 0, base_part.c_str(),
             std::min(base_part.size(), (size_t)100));
    }
  }
  /* mode[8] at 100 */
  snprintf(reinterpret_cast<char*>(hdr.data()) + 100, 8, "0000644");
  /* uid[8] at 108, gid[8] at 116 */
  snprintf(reinterpret_cast<char*>(hdr.data()) + 108, 8, "0001000");
  snprintf(reinterpret_cast<char*>(hdr.data()) + 116, 8, "0001000");
  /* size[12] at 124 — octal */
  snprintf(reinterpret_cast<char*>(hdr.data()) + 124, 12, "%011llo",
           static_cast<unsigned long long>(size));
  /* mtime[12] at 136 */
  snprintf(reinterpret_cast<char*>(hdr.data()) + 136, 12, "%011o", 0);
  /* chksum[8] at 148 — 8 spaces; reader does not validate. */
  memset(hdr.data() + 148, ' ', 8);
  /* typeflag[1] at 156 */
  hdr[156] = static_cast<uint8_t>(typeflag);
  /* linkname[100] at 157 (for symlinks/hardlinks) */
  if (!linkname.empty()) {
    memcpy(hdr.data() + 157, linkname.c_str(),
           std::min(linkname.size(), (size_t)100));
  }
  /* magic[6] at 257 — "ustar\0" */
  memcpy(hdr.data() + 257, "ustar", 6);
  /* version[2] at 263 — "00" */
  memcpy(hdr.data() + 263, "00", 2);
  return hdr;
}

struct TarEntry {
  std::string name;
  char typeflag;
  std::string content;
  std::string linkname;
};

static void write_tar(const fs::path& tar_path, const std::vector<TarEntry>& entries) {
  std::vector<uint8_t> tar;
  for (const auto& entry : entries) {
    auto hdr = ustar_header(entry.name, entry.typeflag,
                            entry.content.size(), entry.linkname);
    tar.insert(tar.end(), hdr.begin(), hdr.end());
    if (entry.typeflag == '0' || entry.typeflag == '\0') {
      tar.insert(tar.end(), entry.content.begin(), entry.content.end());
      size_t pad = (512 - (entry.content.size() % 512)) % 512;
      tar.insert(tar.end(), pad, 0);
    }
  }
  /* End-of-archive: two zero blocks. */
  tar.insert(tar.end(), 1024, 0);
  std::ofstream out(tar_path, std::ios::binary);
  ASSERT_TRUE(out.good());
  out.write(reinterpret_cast<const char*>(tar.data()), static_cast<std::streamsize>(tar.size()));
  out.close();
}

class UpdateExtract : public ::testing::Test {
 protected:
  fs::path scratch;
  fs::path dest_dir;
  fs::path tar_path;

  void SetUp() override {
    scratch = fs::temp_directory_path() /
              ("offs_extract_test_" + std::to_string(getpid()) + "_" +
               std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::create_directories(scratch);
    dest_dir = scratch / "dest";
    fs::create_directories(dest_dir);
    tar_path = scratch / "test.tar";
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(scratch, ec);
  }
};

TEST_F(UpdateExtract, ExtractsValidTarball) {
  write_tar(tar_path, {
      {"foo.txt", '0', std::string("hello"), ""},
      {"bar/baz.txt", '0', std::string("world"), ""},
  });
  ASSERT_EQ(update_extract(tar_path.c_str(), dest_dir.c_str(),
                           1ULL * 1024 * 1024 * 1024), 0);
  ASSERT_TRUE(fs::exists(dest_dir / "foo.txt"));
  ASSERT_TRUE(fs::exists(dest_dir / "bar" / "baz.txt"));
  {
    std::ifstream in(dest_dir / "foo.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello");
  }
  {
    std::ifstream in(dest_dir / "bar" / "baz.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "world");
  }
}

TEST_F(UpdateExtract, RejectsAbsolutePath) {
  write_tar(tar_path, {
      {"/etc/passwd", '0', std::string("evil"), ""},
  });
  ASSERT_EQ(update_extract(tar_path.c_str(), dest_dir.c_str(),
                           1ULL * 1024 * 1024 * 1024), -1);
  /* dest_dir should remain unchanged — no file written. */
  EXPECT_FALSE(fs::exists(dest_dir / "etc"));
}

TEST_F(UpdateExtract, RejectsDotDot) {
  write_tar(tar_path, {
      {"../../etc/passwd", '0', std::string("evil"), ""},
  });
  ASSERT_EQ(update_extract(tar_path.c_str(), dest_dir.c_str(),
                           1ULL * 1024 * 1024 * 1024), -1);
  EXPECT_FALSE(fs::exists(dest_dir / ".." / ".." / "etc"));
}

TEST_F(UpdateExtract, RejectsSymlinkEscape) {
  /* Stage 3 rejects ALL symlinks (typeflag '2'), not just escaping ones —
     simplest and safest. The linkname points outside dest_dir to confirm
     the rejection fires regardless of the target. */
  write_tar(tar_path, {
      {"evil", '2', std::string(""), "../../etc/passwd"},
  });
  ASSERT_EQ(update_extract(tar_path.c_str(), dest_dir.c_str(),
                           1ULL * 1024 * 1024 * 1024), -1);
  EXPECT_FALSE(fs::exists(dest_dir / "evil"));
}

TEST_F(UpdateExtract, RejectsHardlink) {
  write_tar(tar_path, {
      {"original.txt", '0', std::string("data"), ""},
      {"link.txt", '1', std::string(""), "original.txt"},
  });
  ASSERT_EQ(update_extract(tar_path.c_str(), dest_dir.c_str(),
                           1ULL * 1024 * 1024 * 1024), -1);
}

TEST_F(UpdateExtract, RejectsOversized) {
  /* max_bytes = 100, but the single file is 200 bytes. */
  std::string big(200, 'X');
  write_tar(tar_path, {
      {"big.txt", '0', big, ""},
  });
  ASSERT_EQ(update_extract(tar_path.c_str(), dest_dir.c_str(), 100), -1);
  EXPECT_FALSE(fs::exists(dest_dir / "big.txt"));
}

TEST_F(UpdateExtract, ExtractsDirectory) {
  write_tar(tar_path, {
      {"subdir/", '5', std::string(""), ""},
  });
  ASSERT_EQ(update_extract(tar_path.c_str(), dest_dir.c_str(),
                           1ULL * 1024 * 1024 * 1024), 0);
  EXPECT_TRUE(fs::is_directory(dest_dir / "subdir"));
}
