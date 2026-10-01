#include <gtest/gtest.h>
#include <string>
extern "C" {
#include "../src/Platform/platform_dirs.h"
}

/* Env save/restore so parallel suites and other tests are not
   perturbed by the override. */
class PlatformDirs : public ::testing::Test {
protected:
  void SetUp() override {
    const char* prior = getenv("OFFS_CONFIG");
    had_prior_ = (prior != NULL && prior[0] != '\0');
    if (had_prior_) prior_ = prior;
#if defined(_WIN32)
    _putenv("OFFS_CONFIG=");
#else
    unsetenv("OFFS_CONFIG");
#endif
  }
  void TearDown() override {
#if defined(_WIN32)
    if (had_prior_) {
      std::string restore = "OFFS_CONFIG=" + prior_;
      _putenv(restore.c_str());
    }
#else
    if (had_prior_) {
      setenv("OFFS_CONFIG", prior_.c_str(), 1);
    }
#endif
  }

  bool had_prior_ = false;
  std::string prior_;
};

/* $OFFS_CONFIG wins when set, verbatim, whatever the platform. */
TEST_F(PlatformDirs, EnvConfigWins) {
  /* Verbatim copy, no existence check — a leading-slash string is
     fine on either platform. */
#if defined(_WIN32)
  ASSERT_EQ(0, _putenv("OFFS_CONFIG=/liboffs_env_test/offs.json"));
#else
  ASSERT_EQ(0, setenv("OFFS_CONFIG", "/liboffs_env_test/offs.json", 1));
#endif
  char path[1024];
  ASSERT_EQ(0, offs_default_config_path_get(path, sizeof(path)));
  EXPECT_STREQ("/liboffs_env_test/offs.json", path);
}

/* No $OFFS_CONFIG → the platform machine-wide default, the same root
   offs_default_dirs_get() puts the default state dirs under. */
TEST_F(PlatformDirs, PlatformFallbackWhenEnvUnset) {
  char path[1024];
  ASSERT_EQ(0, offs_default_config_path_get(path, sizeof(path)));
#if defined(_WIN32)
  std::string got = path;
  ASSERT_GE(got.size(), strlen("\\offs\\offs.json"));
  EXPECT_EQ(got.rfind("\\offs\\offs.json"), got.size() - strlen("\\offs\\offs.json"));
#else
  EXPECT_STREQ("/etc/offs/offs.json", path);
#endif
}

/* An empty $OFFS_CONFIG is treated exactly like unset (the caller's
   gate cannot distinguish, so the helper must not resolve ""). */
TEST_F(PlatformDirs, EmptyEnvFallsThrough) {
#if defined(_WIN32)
  ASSERT_EQ(0, _putenv("OFFS_CONFIG="));
#else
  ASSERT_EQ(0, setenv("OFFS_CONFIG", "", 1));
#endif
  char path[1024];
  ASSERT_EQ(0, offs_default_config_path_get(path, sizeof(path)));
#if defined(_WIN32)
  std::string got = path;
  EXPECT_NE(got.find("offs.json"), std::string::npos);
#else
  EXPECT_STREQ("/etc/offs/offs.json", path);
#endif
}

TEST_F(PlatformDirs, TinyBufferRejected) {
  char tiny[8];
  EXPECT_EQ(-1, offs_default_config_path_get(tiny, sizeof(tiny)));
  EXPECT_EQ(-1, offs_default_config_path_get(NULL, sizeof(tiny)));
}