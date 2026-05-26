#include "offs_ofd_resolver.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* ---- total_blocks tests ---- */

static void test_total_blocks_small_file(void) {
  TEST("total_blocks small file (2KB, standard)");
  size_t blocks = offs_ofd_total_blocks(2048, 128000, 3);
  /* data_blocks = ceil(2048/128000) = 1
     desc_blocks = ceil(1*32*3 / 127968) = ceil(96/127968) = 1 */
  if (blocks != 2) FAIL("expected 2 blocks");
  PASS();
}

static void test_total_blocks_zero_byte(void) {
  TEST("total_blocks zero-byte file");
  size_t blocks = offs_ofd_total_blocks(0, 128000, 3);
  /* data_blocks = 0, desc_blocks = 0 */
  if (blocks != 0) FAIL("expected 0 blocks");
  PASS();
}

static void test_total_blocks_large_file(void) {
  TEST("total_blocks large file (10MB, standard)");
  /* data_blocks = ceil(10000000/128000) = 79
     desc_bytes = 79 * 32 * 3 = 7584
     desc_blocks = ceil(7584 / 127968) = 1 */
  size_t blocks = offs_ofd_total_blocks(10000000, 128000, 3);
  if (blocks != 80) FAIL("expected 80 blocks");
  PASS();
}

/* ---- build_recyclers tests ---- */

static void test_build_recyclers_exact_match(void) {
  TEST("build_recyclers exact match (no donors consumed)");
  uint8_t hash[32] = {1};
  offs_ofd_donor_t matched = {
    .name = "test.js", .url = "http://h/off/v3/app/js/100/h1/h2/test.js",
    .final_byte = 1000, .block_type = 128000, .tuple_size = 3,
    .file_hash = hash, .hash_len = 32
  };
  char** result = NULL;

  size_t out_count = 0;
  result = offs_ofd_build_recyclers(NULL, 0, &matched, 1000, NULL, 0, &out_count);
  if (!result) FAIL("result is NULL");
  if (out_count != 1) { free(result[0]); free(result); FAIL("expected out_count=1"); }
  if (strcmp(result[0], matched.url) != 0) { free(result[0]); free(result); FAIL("URL mismatch"); }
  free(result[0]);
  free(result);
  PASS();
}

static void test_build_recyclers_file_grew(void) {
  TEST("build_recyclers file grew (needs donors)");
  uint8_t hash1[32] = {1};
  uint8_t hash2[32] = {2};
  uint8_t hash3[32] = {3};

  offs_ofd_donor_t matched = {
    .name = "main.js", .url = "http://h/off/v3/app/js/2048/h1/h2/main.js",
    .final_byte = 2048, .block_type = 128000, .tuple_size = 3,
    .file_hash = hash1, .hash_len = 32
  };

  offs_ofd_donor_t donors[2] = {
    { .name = "old.js", .url = "http://h/off/v3/app/js/128000/h3/h4/old.js",
      .final_byte = 128000, .block_type = 128000, .tuple_size = 3,
      .file_hash = hash2, .hash_len = 32 },
    { .name = "lib.js", .url = "http://h/off/v3/app/js/256000/h5/h6/lib.js",
      .final_byte = 256000, .block_type = 128000, .tuple_size = 3,
      .file_hash = hash3, .hash_len = 32 },
  };

  char** result = NULL;
  size_t out_count = 0;
  result = offs_ofd_build_recyclers(donors, 2, &matched, 256000, NULL, 0, &out_count);
  if (!result) FAIL("result is NULL");
  if (out_count != 2) {
    for (size_t i = 0; i < out_count; i++) free(result[i]);
    free(result);
    FAIL("expected out_count=2");
  }
  if (strcmp(result[0], matched.url) != 0) {
    for (size_t i = 0; i < out_count; i++) free(result[i]);
    free(result);
    FAIL("first entry should be matched URL");
  }
  for (size_t i = 0; i < out_count; i++) free(result[i]);
  free(result);
  PASS();
}

static void test_build_recyclers_no_match(void) {
  TEST("build_recyclers no match (uses donor pool only)");
  uint8_t hash1[32] = {10};
  offs_ofd_donor_t donors[1] = {
    { .name = "other.js", .url = "http://h/off/v3/app/js/128000/h1/h2/other.js",
      .final_byte = 128000, .block_type = 128000, .tuple_size = 3,
      .file_hash = hash1, .hash_len = 32 }
  };
  const char* fallback[] = {"http://backup/off/v3/app/js/0/h3/h4/backup.ofd"};

  char** result = NULL;
  size_t out_count = 0;
  result = offs_ofd_build_recyclers(donors, 1, NULL, 50000,
                                     fallback, 1, &out_count);
  if (!result) FAIL("result is NULL");
  if (out_count != 2) {
    for (size_t i = 0; i < out_count; i++) free(result[i]);
    free(result);
    FAIL("expected out_count=2");
  }
  for (size_t i = 0; i < out_count; i++) free(result[i]);
  free(result);
  PASS();
}

static void test_build_recyclers_fallback_only(void) {
  TEST("build_recyclers null donors (fallback only)");
  const char* fallback[] = {"http://fb/off/v3/app/js/0/h1/h2/backup.ofd"};

  char** result = NULL;
  size_t out_count = 0;
  result = offs_ofd_build_recyclers(NULL, 0, NULL, 50000,
                                     fallback, 1, &out_count);
  if (!result) FAIL("result is NULL");
  if (out_count != 1) { free(result[0]); free(result); FAIL("expected out_count=1"); }
  if (strcmp(result[0], fallback[0]) != 0) { free(result[0]); free(result); FAIL("URL mismatch"); }
  free(result[0]);
  free(result);
  PASS();
}

static void test_build_recyclers_zero_byte_matched(void) {
  TEST("build_recyclers zero-byte file with matched donor");
  uint8_t hash[32] = {1};
  offs_ofd_donor_t matched = {
    .name = "empty.txt", .url = "http://h/off/v3/app/js/0/h1/h2/empty.txt",
    .final_byte = 0, .block_type = 128000, .tuple_size = 3,
    .file_hash = hash, .hash_len = 32
  };

  char** result = NULL;
  size_t out_count = 0;
  /* file_size=0 → blocks_needed=0, no donors consumed */
  result = offs_ofd_build_recyclers(NULL, 0, &matched, 0, NULL, 0, &out_count);
  if (!result) FAIL("result is NULL");
  if (out_count != 1) { free(result[0]); free(result); FAIL("expected out_count=1"); }
  free(result[0]);
  free(result);
  PASS();
}

static void test_build_recyclers_empty_fallback(void) {
  TEST("build_recyclers empty fallback list");
  uint8_t hash[32] = {42};
  offs_ofd_donor_t donors[1] = {
    { .name = "file.js", .url = "http://h/off/v3/app/js/128000/h1/h2/file.js",
      .final_byte = 128000, .block_type = 128000, .tuple_size = 3,
      .file_hash = hash, .hash_len = 32 }
  };

  char** result = NULL;
  size_t out_count = 0;
  result = offs_ofd_build_recyclers(donors, 1, NULL, 50000, NULL, 0, &out_count);
  if (!result) FAIL("result is NULL");
  if (out_count != 1) { free(result[0]); free(result); FAIL("expected out_count=1"); }
  free(result[0]);
  free(result);
  PASS();
}

/* ---- main ---- */

int main(void) {
  printf("C OFD Resolver Tests:\n");

  test_total_blocks_small_file();
  test_total_blocks_zero_byte();
  test_total_blocks_large_file();
  test_build_recyclers_exact_match();
  test_build_recyclers_file_grew();
  test_build_recyclers_no_match();
  test_build_recyclers_fallback_only();
  test_build_recyclers_zero_byte_matched();
  test_build_recyclers_empty_fallback();

  printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
