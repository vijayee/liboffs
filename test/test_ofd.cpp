#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/OFFStreams/ofd.h"
#include "../src/Buffer/buffer.h"
#include "../src/OFFStreams/ori.h"
}

class TestOfd : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TestOfd, CreateDestroy) {
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);
    EXPECT_EQ(ofd->entries.length, 0);
    ofd_destroy(ofd);
}

TEST_F(TestOfd, AddFileEntry) {
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);

    ori_t* ori = ori_create(1024);
    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)i;
    ori->file_hash = buffer_create_from_pointer_copy(hash_data, 32);
    ori->file_name = strdup("test.txt");

    ofd_add_file(ofd, "test.txt", ori);
    DESTROY(ori, ori);
    ASSERT_EQ(ofd->entries.length, 1);
    EXPECT_STREQ(ofd->entries.data[0].name, "test.txt");
    EXPECT_EQ(ofd->entries.data[0].type, OFD_ENTRY_FILE);
    EXPECT_NE(ofd->entries.data[0].file_ori, nullptr);

    ofd_destroy(ofd);
}

TEST_F(TestOfd, AddDirectoryEntry) {
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);

    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)(i + 100);
    buffer_t* dir_hash = buffer_create_from_pointer_copy(hash_data, 32);

    ofd_add_directory(ofd, "css", dir_hash);
    DESTROY(dir_hash, buffer);
    ASSERT_EQ(ofd->entries.length, 1);
    EXPECT_STREQ(ofd->entries.data[0].name, "css");
    EXPECT_EQ(ofd->entries.data[0].type, OFD_ENTRY_DIRECTORY);
    EXPECT_NE(ofd->entries.data[0].dir_hash, nullptr);
    EXPECT_EQ(ofd->entries.data[0].dir_hash->size, 32);

    ofd_destroy(ofd);
}

TEST_F(TestOfd, FindEntry) {
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);

    ori_t* ori = ori_create(512);
    uint8_t hash_data[32];
    for (int i = 0; i < 32; i++) hash_data[i] = (uint8_t)i;
    ori->file_hash = buffer_create_from_pointer_copy(hash_data, 32);
    ori->file_name = strdup("index.html");

    ofd_add_file(ofd, "index.html", ori);
    DESTROY(ori, ori);

    ofd_entry_t* found = ofd_find(ofd, "index.html");
    ASSERT_NE(found, nullptr);
    EXPECT_STREQ(found->name, "index.html");
    EXPECT_EQ(found->type, OFD_ENTRY_FILE);

    ofd_entry_t* not_found = ofd_find(ofd, "missing.txt");
    EXPECT_EQ(not_found, nullptr);

    ofd_destroy(ofd);
}

TEST_F(TestOfd, EncodeDecodeRoundTrip) {
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);

    // Add a file entry
    ori_t* file_ori = ori_create(2048);
    uint8_t file_hash_data[32];
    for (int i = 0; i < 32; i++) file_hash_data[i] = (uint8_t)(i * 7);
    file_ori->file_hash = buffer_create_from_pointer_copy(file_hash_data, 32);
    file_ori->file_name = strdup("readme.md");
    ofd_add_file(ofd, "readme.md", file_ori);
    DESTROY(file_ori, ori);

    // Add a directory entry
    uint8_t dir_hash_data[32];
    for (int i = 0; i < 32; i++) dir_hash_data[i] = (uint8_t)(i + 50);
    buffer_t* dir_hash = buffer_create_from_pointer_copy(dir_hash_data, 32);
    ofd_add_directory(ofd, "assets", dir_hash);
    DESTROY(dir_hash, buffer);

    // Encode
    buffer_t* encoded = ofd_encode(ofd);
    ASSERT_NE(encoded, nullptr);
    EXPECT_GT(encoded->size, 0u);

    // Decode
    ofd_t* decoded = ofd_decode(encoded);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->entries.length, 2);

    // Verify file entry
    ofd_entry_t* file_entry = ofd_find(decoded, "readme.md");
    ASSERT_NE(file_entry, nullptr);
    EXPECT_EQ(file_entry->type, OFD_ENTRY_FILE);
    EXPECT_NE(file_entry->file_ori, nullptr);
    EXPECT_EQ(file_entry->file_ori->file_hash->size, 32);
    EXPECT_EQ(memcmp(file_entry->file_ori->file_hash->data, file_hash_data, 32), 0);

    // Verify directory entry
    ofd_entry_t* dir_entry = ofd_find(decoded, "assets");
    ASSERT_NE(dir_entry, nullptr);
    EXPECT_EQ(dir_entry->type, OFD_ENTRY_DIRECTORY);
    EXPECT_NE(dir_entry->dir_hash, nullptr);
    EXPECT_EQ(dir_entry->dir_hash->size, 32);
    EXPECT_EQ(memcmp(dir_entry->dir_hash->data, dir_hash_data, 32), 0);

    buffer_destroy(encoded);
    ofd_destroy(decoded);
    ofd_destroy(ofd);
}

TEST_F(TestOfd, EncodeDecodeEmptyOfd) {
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);

    buffer_t* encoded = ofd_encode(ofd);
    ASSERT_NE(encoded, nullptr);

    ofd_t* decoded = ofd_decode(encoded);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->entries.length, 0);

    buffer_destroy(encoded);
    ofd_destroy(decoded);
    ofd_destroy(ofd);
}

TEST_F(TestOfd, DecodeInvalidData) {
    uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03};
    buffer_t* buf = buffer_create_from_pointer_copy(garbage, sizeof(garbage));

    ofd_t* result = ofd_decode(buf);
    EXPECT_EQ(result, nullptr);

    buffer_destroy(buf);
}

TEST_F(TestOfd, DecodeNullBuffer) {
    ofd_t* result = ofd_decode(NULL);
    EXPECT_EQ(result, nullptr);
}