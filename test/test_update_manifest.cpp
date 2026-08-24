#include <gtest/gtest.h>

extern "C" {
#include "Update/update_manifest.h"
#include <cbor.h>
}

#include <cstdint>
#include <cstring>

/* Build a CBOR manifest in memory matching the format update_manifest_parse
   expects: [version:1, release_tag:"v1.2.3", files:[[path, sha256, size], ...]] */

TEST(UpdateManifest, ParseValidManifest) {
  cbor_item_t* root = cbor_new_definite_array(3);
  cbor_array_push(root, cbor_move(cbor_build_uint8(1)));
  cbor_array_push(root, cbor_move(cbor_build_string("v1.2.3")));

  cbor_item_t* files = cbor_new_definite_array(2);
  cbor_item_t* file_one = cbor_new_definite_array(3);
  cbor_array_push(file_one, cbor_move(cbor_build_string("offs-daemon")));
  cbor_array_push(file_one, cbor_move(cbor_build_string("abc123...64hex")));
  cbor_array_push(file_one, cbor_move(cbor_build_uint64(12345)));
  cbor_array_push(files, cbor_move(file_one));

  cbor_item_t* file_two = cbor_new_definite_array(3);
  cbor_array_push(file_two, cbor_move(cbor_build_string("offs-cli")));
  cbor_array_push(file_two, cbor_move(cbor_build_string("def456...64hex")));
  cbor_array_push(file_two, cbor_move(cbor_build_uint64(6789)));
  cbor_array_push(files, cbor_move(file_two));

  cbor_array_push(root, cbor_move(files));

  unsigned char* buffer = NULL;
  size_t buffer_len = 0;
  cbor_serialize_alloc(root, &buffer, &buffer_len);
  cbor_decref(&root);

  update_manifest_t* manifest = update_manifest_parse(buffer, buffer_len);
  ASSERT_NE(manifest, nullptr);
  EXPECT_EQ(manifest->version, 1u);
  EXPECT_STREQ(manifest->release_tag, "v1.2.3");
  EXPECT_EQ(manifest->file_count, 2u);
  EXPECT_STREQ(manifest->files[0].path, "offs-daemon");
  EXPECT_STREQ(manifest->files[0].sha256, "abc123...64hex");
  EXPECT_EQ(manifest->files[0].size, 12345u);
  EXPECT_STREQ(manifest->files[1].path, "offs-cli");
  EXPECT_STREQ(manifest->files[1].sha256, "def456...64hex");
  EXPECT_EQ(manifest->files[1].size, 6789u);
  update_manifest_free(manifest);
  free(buffer);
}

TEST(UpdateManifest, RejectsBadVersion) {
  cbor_item_t* root = cbor_new_definite_array(3);
  cbor_array_push(root, cbor_move(cbor_build_uint8(2)));
  cbor_array_push(root, cbor_move(cbor_build_string("v1.2.3")));
  cbor_array_push(root, cbor_move(cbor_new_definite_array(0)));

  unsigned char* buffer = NULL;
  size_t buffer_len = 0;
  cbor_serialize_alloc(root, &buffer, &buffer_len);
  cbor_decref(&root);

  EXPECT_EQ(update_manifest_parse(buffer, buffer_len), nullptr);
  free(buffer);
}

TEST(UpdateManifest, RejectsMalformedFileEntry) {
  /* A file entry with only 2 elements (path, size) — missing sha256. */
  cbor_item_t* root = cbor_new_definite_array(3);
  cbor_array_push(root, cbor_move(cbor_build_uint8(1)));
  cbor_array_push(root, cbor_move(cbor_build_string("v1.2.3")));

  cbor_item_t* files = cbor_new_definite_array(1);
  cbor_item_t* file_one = cbor_new_definite_array(2);
  cbor_array_push(file_one, cbor_move(cbor_build_string("offs-daemon")));
  cbor_array_push(file_one, cbor_move(cbor_build_uint64(12345)));
  cbor_array_push(files, cbor_move(file_one));
  cbor_array_push(root, cbor_move(files));

  unsigned char* buffer = NULL;
  size_t buffer_len = 0;
  cbor_serialize_alloc(root, &buffer, &buffer_len);
  cbor_decref(&root);

  EXPECT_EQ(update_manifest_parse(buffer, buffer_len), nullptr);
  free(buffer);
}

TEST(UpdateManifest, RejectsNonArray) {
  /* Garbage bytes that don't decode as a CBOR array. */
  const uint8_t garbage[] = {0xFF};
  EXPECT_EQ(update_manifest_parse(garbage, sizeof(garbage)), nullptr);
}

TEST(UpdateManifest, RejectsWrongRootType) {
  /* A map instead of an array. */
  cbor_item_t* root = cbor_new_definite_map(0);
  unsigned char* buffer = NULL;
  size_t buffer_len = 0;
  cbor_serialize_alloc(root, &buffer, &buffer_len);
  cbor_decref(&root);
  EXPECT_EQ(update_manifest_parse(buffer, buffer_len), nullptr);
  free(buffer);
}

TEST(UpdateManifest, RejectsBadFileType) {
  /* A file entry where sha256 is a uint instead of a string. */
  cbor_item_t* root = cbor_new_definite_array(3);
  cbor_array_push(root, cbor_move(cbor_build_uint8(1)));
  cbor_array_push(root, cbor_move(cbor_build_string("v1.2.3")));

  cbor_item_t* files = cbor_new_definite_array(1);
  cbor_item_t* file_one = cbor_new_definite_array(3);
  cbor_array_push(file_one, cbor_move(cbor_build_string("offs-daemon")));
  cbor_array_push(file_one, cbor_move(cbor_build_uint64(12345))); /* wrong type */
  cbor_array_push(file_one, cbor_move(cbor_build_uint64(12345)));
  cbor_array_push(files, cbor_move(file_one));
  cbor_array_push(root, cbor_move(files));

  unsigned char* buffer = NULL;
  size_t buffer_len = 0;
  cbor_serialize_alloc(root, &buffer, &buffer_len);
  cbor_decref(&root);

  EXPECT_EQ(update_manifest_parse(buffer, buffer_len), nullptr);
  free(buffer);
}

/* Build a two-file manifest and parse it. Returns the parsed manifest
   (caller frees) or fails the test. Reused by the FindFile tests. */
static update_manifest_t* build_two_file_manifest() {
  cbor_item_t* root = cbor_new_definite_array(3);
  cbor_array_push(root, cbor_move(cbor_build_uint8(1)));
  cbor_array_push(root, cbor_move(cbor_build_string("v1.2.3")));

  cbor_item_t* files = cbor_new_definite_array(2);

  cbor_item_t* file_one = cbor_new_definite_array(3);
  cbor_array_push(file_one, cbor_move(cbor_build_string("offs-daemon")));
  cbor_array_push(file_one, cbor_move(cbor_build_string("aaa111...64hex")));
  cbor_array_push(file_one, cbor_move(cbor_build_uint64(12345)));
  cbor_array_push(files, cbor_move(file_one));

  cbor_item_t* file_two = cbor_new_definite_array(3);
  cbor_array_push(file_two, cbor_move(cbor_build_string("offs-cli")));
  cbor_array_push(file_two, cbor_move(cbor_build_string("bbb222...64hex")));
  cbor_array_push(file_two, cbor_move(cbor_build_uint64(6789)));
  cbor_array_push(files, cbor_move(file_two));

  cbor_array_push(root, cbor_move(files));

  unsigned char* buffer = NULL;
  size_t buffer_len = 0;
  cbor_serialize_alloc(root, &buffer, &buffer_len);
  cbor_decref(&root);

  update_manifest_t* manifest = update_manifest_parse(buffer, buffer_len);
  free(buffer);
  return manifest;
}

TEST(UpdateManifest, FindFileReturnsEntry) {
  update_manifest_t* manifest = build_two_file_manifest();
  ASSERT_NE(manifest, nullptr);

  const manifest_file_t* entry =
      update_manifest_find_file(manifest, "offs-cli");
  EXPECT_NE(entry, nullptr);
  if (entry != nullptr) {
    EXPECT_STREQ(entry->path, "offs-cli");
    EXPECT_STREQ(entry->sha256, "bbb222...64hex");
    EXPECT_EQ(entry->size, 6789u);
  }

  update_manifest_free(manifest);
}

TEST(UpdateManifest, FindFileReturnsNullForMissing) {
  update_manifest_t* manifest = build_two_file_manifest();
  ASSERT_NE(manifest, nullptr);

  EXPECT_EQ(update_manifest_find_file(manifest, "offs-updater"), nullptr);
  EXPECT_EQ(update_manifest_find_file(manifest, ""), nullptr);

  update_manifest_free(manifest);
}

TEST(UpdateManifest, FindFileReturnsNullForNullInputs) {
  update_manifest_t* manifest = build_two_file_manifest();
  ASSERT_NE(manifest, nullptr);

  EXPECT_EQ(update_manifest_find_file(nullptr, "offs-daemon"), nullptr);
  EXPECT_EQ(update_manifest_find_file(manifest, nullptr), nullptr);

  update_manifest_free(manifest);
}