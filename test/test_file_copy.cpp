#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
extern "C" {
#include "../src/Util/file_copy.h"
#include "../src/Util/mkdir_p.h"
}

namespace fs = std::filesystem;

/* Each test gets a fresh temp dir. */
class FileCopy : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / "liboffs_file_copy_test";
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path p(const char* name) { return dir_ / name; }

  static void write_file(const fs::path& where, const std::string& content) {
    std::ofstream out(where, std::ios::binary);
    out << content;
  }
  static std::string read_file(const fs::path& where) {
    std::ifstream in(where, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  fs::path dir_;
};

/* The move tests ride the same fixture helpers; a separate suite name
   keeps them filterable apart from the plain-copy tests. */
class FileMoveVerified : public FileCopy {};

TEST_F(FileCopy, CopyCreatesIdenticalBytes) {
  fs::path src = p("src.bin");
  fs::path dst = p("dst.bin");
  std::string content(100000, 'x');
  content[50000] = '\n'; /* a mid-stream byte boundary must survive */
  write_file(src, content);

  ASSERT_EQ(0, file_copy(src.string().c_str(), dst.string().c_str()));
  EXPECT_EQ(content, read_file(dst));
}

TEST_F(FileCopy, CopyMissingSourceFails) {
  fs::path dst = p("dst.bin");
  EXPECT_NE(0, file_copy(p("missing.bin").string().c_str(), dst.string().c_str()));
  EXPECT_FALSE(fs::exists(dst));
}

TEST_F(FileCopy, CopyOverwritesExistingDest) {
  fs::path src = p("src.bin");
  fs::path dst = p("dst.bin");
  write_file(src, "tiny");
  write_file(dst, "a much longer previous file that must be fully replaced");

  ASSERT_EQ(0, file_copy(src.string().c_str(), dst.string().c_str()));
  EXPECT_EQ("tiny", read_file(dst));
}

TEST_F(FileCopy, CopyOntoDirectoryPathFailsSourceIntact) {
  fs::path src = p("src.bin");
  fs::path dst_dir = p("dst_dir");
  fs::create_directories(dst_dir);
  write_file(src, "payload");

  EXPECT_NE(0, file_copy(src.string().c_str(), dst_dir.string().c_str()));
  EXPECT_EQ("payload", read_file(src));
}

TEST_F(FileCopy, DirIsEmptyEmptyNonemptyMissing) {
  fs::path empty = p("empty");
  fs::create_directories(empty);
  EXPECT_EQ(1, dir_is_empty(empty.string().c_str()));

  fs::path nonempty = p("nonempty");
  fs::create_directories(nonempty);
  write_file(nonempty / "f.txt", "x");
  EXPECT_EQ(0, dir_is_empty(nonempty.string().c_str()));

  EXPECT_EQ(-1, dir_is_empty(p("no_such_dir").string().c_str()));
}

TEST_F(FileCopy, CopyTreeCopiesNested) {
  fs::path src = p("tree_src");
  fs::path dst = p("tree_dst");
  fs::create_directories(src / "sub" / "deep");
  write_file(src / "a.txt", "alpha");
  write_file(src / "sub" / "b.txt", "beta");
  write_file(src / "sub" / "deep" / "c.txt", "gamma");

  ASSERT_EQ(0, copy_tree(src.string().c_str(), dst.string().c_str()));
  EXPECT_EQ("alpha", read_file(dst / "a.txt"));
  EXPECT_EQ("beta", read_file(dst / "sub" / "b.txt"));
  EXPECT_EQ("gamma", read_file(dst / "sub" / "deep" / "c.txt"));
  EXPECT_EQ(0, dir_is_empty(dst.string().c_str()));
}

TEST_F(FileMoveVerified, HappySha256RemovesSource) {
  fs::path src = p("move_src.bin");
  fs::path dst = p("move_dst.bin");
  std::string content(5000, 'm');
  write_file(src, content);

  ASSERT_EQ(0, file_move_verified(src.string().c_str(), dst.string().c_str(),
                                  FILE_VERIFY_SHA256));
  EXPECT_FALSE(fs::exists(src));
  EXPECT_EQ(content, read_file(dst));
}

TEST_F(FileMoveVerified, VerifyNoneStillMoves) {
  fs::path src = p("mv_src.bin");
  fs::path dst = p("mv_dst.bin");
  write_file(src, "unsafe payload");

  ASSERT_EQ(0, file_move_verified(src.string().c_str(), dst.string().c_str(),
                                  FILE_VERIFY_NONE));
  EXPECT_FALSE(fs::exists(src));
  EXPECT_EQ("unsafe payload", read_file(dst));
}

TEST_F(FileMoveVerified, CopyFailureKeepsSource) {
  fs::path src = p("mvf_src.bin");
  fs::path dst_dir = p("mvf_dst_dir"); /* a dir at the dest file path */
  write_file(src, "must survive");
  fs::create_directories(dst_dir);

  EXPECT_NE(0, file_move_verified(src.string().c_str(), dst_dir.string().c_str(),
                                  FILE_VERIFY_SHA256));
  EXPECT_TRUE(fs::exists(src));
  EXPECT_EQ("must survive", read_file(src));
}

TEST(FileSha256, KnownVector) {
  char hex[65];
  fs::path path = fs::temp_directory_path() / "liboffs_sha256_vec.txt";
  {
    std::ofstream out(path, std::ios::binary);
    out << "abc";
  }
  ASSERT_EQ(0, file_sha256_hex(path.string().c_str(), hex));
  EXPECT_STREQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               hex);
  fs::remove(path);
}

TEST(FileSha256, MissingFileFails) {
  char hex[65];
  EXPECT_NE(0, file_sha256_hex("Z:/definitely/not/here.bin", hex));
}

/* mkdir_p must create every missing level of its chain — including when the
 * path mixes separators, which the cache-move flow does
 * ("C:\...\base/src/sub" with src missing used to return 0 while creating
 * nothing: the parent step truncated at the last BACKslash). */
class MkdirP : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / "liboffs_mkdirp_test";
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path dir_;
};

TEST_F(MkdirP, MixedSeparatorsDeepChain) {
  std::string leaf;
#ifdef _WIN32
  /* Native leading separators, forward slashes for the tail. */
  leaf = (dir_ / "mix").string() + "/deep/leaf";
#else
  leaf = (dir_ / "mix" / "deep" / "leaf").string();
#endif
  char buffer[1024];
  snprintf(buffer, sizeof(buffer), "%s", leaf.c_str());
  ASSERT_EQ(0, mkdir_p(buffer));
  EXPECT_TRUE(fs::is_directory(leaf));

  /* Already-existing leaf stays success. */
  snprintf(buffer, sizeof(buffer), "%s", leaf.c_str());
  EXPECT_EQ(0, mkdir_p(buffer));
}

TEST_F(MkdirP, MissingLeafUnderExistingParent) {
  std::string leaf = (dir_ / "one/level").string();
  char buffer[1024];
  snprintf(buffer, sizeof(buffer), "%s", leaf.c_str());
  ASSERT_EQ(0, mkdir_p(buffer));
  EXPECT_TRUE(fs::is_directory(leaf));
}

TEST_F(MkdirP, ExistingFileAtLeafFails) {
  fs::path file = dir_ / "afile.txt";
  {
    std::ofstream out(file, std::ios::binary);
    out << "x";
  }
  char buffer[1024];
  snprintf(buffer, sizeof(buffer), "%s", file.string().c_str());
  EXPECT_NE(0, mkdir_p(buffer));
}