//
// Tests for the platform_regex_compat shim. Verifies that the same POSIX
// regex API (regcomp/regexec/regfree/regex_t/regmatch_t) works on both
// Linux (system <regex.h>) and Windows (PCRE2 via pcre2posix.h) for the
// actual patterns the liboffs HTTP server uses.
//
// Background: the previous shim implemented regcomp via strcmp and rejected
// any pattern containing '(' or ')' with REG_EPAREN, which broke the
// off_server's GET/POST routes on Windows. This test exercises a pattern
// that includes capture groups and proves the back-end handles them.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/Platform/platform_regex_compat.h"
#include "../src/Util/allocator.h"
#include <string.h>
}

namespace {

// Pattern with capture groups — same shape as OFF_GET_PATTERN in
// src/ClientAPI/HTTP/off_routes.c:66 but shortened so the test stays
// focused on the shim's behavior. Exercises:
//   - literal prefix matching
//   - capture groups
//   - character class with hyphen and dot
//   - quantifier
//   - end anchor
static const char kPattern[] = "/offsystem/v3/([-+._a-zA-Z0-9]+)/(.+)$";

TEST(PlatformRegexCompat, CompilesAndMatchesCaptureGroup) {
  regex_t pattern;
  ASSERT_EQ(regcomp(&pattern, kPattern, REG_EXTENDED), 0)
      << "shim rejected a pattern the off_server uses; the regex back-end is broken";

  // The pattern has 2 capture groups, so pattern.re_nsub should be 2.
  EXPECT_EQ(pattern.re_nsub, 2u);

  // Real-looking URL with two capture groups.
  const char* path = "/offsystem/v3/abc123.456/baz";
  const size_t path_len = strlen(path);
  const size_t prefix_len = strlen("/offsystem/v3/");
  const size_t nmatch = pattern.re_nsub + 1;
  regmatch_t* matches = (regmatch_t*)get_memory(sizeof(regmatch_t) * nmatch);

  int rc = regexec(&pattern, path, nmatch, matches, 0);
  ASSERT_EQ(rc, 0) << "regexec should match the URL";

  // Whole match (matches[0]) spans the entire string.
  EXPECT_EQ(matches[0].rm_so, 0);
  EXPECT_EQ(matches[0].rm_eo, (regoff_t)path_len);

  // Capture 1: "abc123.456"
  const size_t capture1_len = strlen("abc123.456");
  EXPECT_EQ(matches[1].rm_so, (regoff_t)prefix_len);
  EXPECT_EQ(matches[1].rm_eo, (regoff_t)(prefix_len + capture1_len));
  EXPECT_EQ(strncmp(path + matches[1].rm_so, "abc123.456", capture1_len), 0);

  // Capture 2: "baz"
  const size_t capture2_start = path_len - strlen("baz");
  EXPECT_EQ(matches[2].rm_so, (regoff_t)capture2_start);
  EXPECT_EQ(matches[2].rm_eo, (regoff_t)path_len);
  EXPECT_EQ(strncmp(path + matches[2].rm_so, "baz", 3), 0);

  free(matches);
  regfree(&pattern);
}

TEST(PlatformRegexCompat, NoMatchReturnsNomatch) {
  regex_t pattern;
  ASSERT_EQ(regcomp(&pattern, kPattern, REG_EXTENDED), 0);

  // Path with the wrong prefix; regexec must not match.
  const char* bad = "/other/v1/abc/def";
  regmatch_t m;
  int rc = regexec(&pattern, bad, 1, &m, 0);
  EXPECT_EQ(rc, REG_NOMATCH);

  regfree(&pattern);
}

TEST(PlatformRegexCompat, RejectsInvalidPattern) {
  regex_t pattern;
  // Unmatched '(' is invalid in POSIX-extended regex on every back-end.
  int rc = regcomp(&pattern, "(unclosed", REG_EXTENDED);
  EXPECT_NE(rc, 0);
  // regfree is safe to call on a pattern that failed to compile in both
  // POSIX and PCRE2's wrapper.
  regfree(&pattern);
}

}  // namespace
