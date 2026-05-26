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
    uint8_t desc_hash_data[32];
    for (int i = 0; i < 32; i++) desc_hash_data[i] = (uint8_t)(i * 11);
    file_ori->descriptor_hash = buffer_create_from_pointer_copy(desc_hash_data, 32);
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
    EXPECT_NE(file_entry->file_ori->descriptor_hash, nullptr);
    EXPECT_EQ(file_entry->file_ori->descriptor_hash->size, 32);
    EXPECT_EQ(memcmp(file_entry->file_ori->descriptor_hash->data, desc_hash_data, 32), 0);

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

TEST_F(TestOfd, DecodeExternalCborCbor2) {
    /* CBOR produced by Python cbor2, representing the same structure as the
       Dart cbor package would produce for a folder OFD.

       Structure:
       {"v": 1, "entries": [{"n": "index.html", "t": 0,
         "f": <32-byte hash>, "D": <32-byte hash>,
         "s": 52, "B": 128000, "T": 3, "o": 0}]}

       This is 119 bytes of valid CBOR. The crash was:
         malloc(): unaligned fastbin chunk detected
       when the server tried to decode CBOR from the Dart cbor package.
    */
    uint8_t cbor_data[] = {
        0xa2, 0x61, 0x76, 0x01, 0x67, 0x65, 0x6e, 0x74,
        0x72, 0x69, 0x65, 0x73, 0x81, 0xa8, 0x61, 0x6e,
        0x6a, 0x69, 0x6e, 0x64, 0x65, 0x78, 0x2e, 0x68,
        0x74, 0x6d, 0x6c, 0x61, 0x74, 0x00, 0x61, 0x66,
        0x58, 0x20, 0x04, 0x21, 0x09, 0x4c, 0x5d, 0x15,
        0xa4, 0x2b, 0xb7, 0x92, 0xa7, 0x25, 0xa1, 0xd2,
        0xc8, 0x54, 0x70, 0x7e, 0x49, 0x7f, 0xd5, 0xf5,
        0x32, 0x4a, 0x19, 0x8c, 0x80, 0x6f, 0xed, 0x66,
        0x82, 0x4d, 0x61, 0x44, 0x58, 0x20, 0x0b, 0x71,
        0xac, 0x15, 0x0e, 0x8f, 0x79, 0x15, 0xf3, 0x3d,
        0xe0, 0xbd, 0x50, 0x9b, 0xd7, 0xa6, 0x74, 0xc0,
        0x7f, 0x5e, 0x95, 0xdb, 0x5b, 0x81, 0xe3, 0xd5,
        0xf8, 0x35, 0xf1, 0xfc, 0x00, 0xc3, 0x61, 0x73,
        0x18, 0x34, 0x61, 0x42, 0x1a, 0x00, 0x01, 0xf4,
        0x00, 0x61, 0x54, 0x03, 0x61, 0x6f, 0x00
    };

    buffer_t* buf = buffer_create_from_pointer_copy(cbor_data, sizeof(cbor_data));
    ASSERT_NE(buf, nullptr);

    ofd_t* result = ofd_decode(buf);
    ASSERT_NE(result, nullptr) << "Failed to decode external CBOR (cbor2)";
    EXPECT_EQ(result->entries.length, 1) << "Should have 1 entry";

    ofd_entry_t* entry = ofd_find(result, "index.html");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->type, OFD_ENTRY_FILE);
    ASSERT_NE(entry->file_ori, nullptr);
    EXPECT_EQ(entry->file_ori->final_byte, 52u);
    EXPECT_EQ(entry->file_ori->block_type, standard);
    EXPECT_EQ(entry->file_ori->tuple_size, 3u);
    EXPECT_EQ(entry->file_ori->file_offset, 0u);
    ASSERT_NE(entry->file_ori->file_hash, nullptr);
    EXPECT_EQ(entry->file_ori->file_hash->size, 32u);
    ASSERT_NE(entry->file_ori->descriptor_hash, nullptr);
    EXPECT_EQ(entry->file_ori->descriptor_hash->size, 32u);

    buffer_destroy(buf);
    ofd_destroy(result);
}

TEST_F(TestOfd, EncodeDecodeMatchesExternalFormat) {
    /* Verify that ofd_encode produces CBOR that ofd_decode can handle,
       and that the decoded fields match what an external encoder would produce. */
    ofd_t* ofd = ofd_create();
    ASSERT_NE(ofd, nullptr);

    ori_t* file_ori = ori_create(52);
    uint8_t file_hash_data[32];
    uint8_t desc_hash_data[32];
    for (int i = 0; i < 32; i++) {
        file_hash_data[i] = (uint8_t)(i * 7);
        desc_hash_data[i] = (uint8_t)(i * 11);
    }
    file_ori->file_hash = buffer_create_from_pointer_copy(file_hash_data, 32);
    file_ori->descriptor_hash = buffer_create_from_pointer_copy(desc_hash_data, 32);
    file_ori->file_name = strdup("index.html");
    file_ori->block_type = standard;
    file_ori->tuple_size = 3;
    file_ori->file_offset = 0;
    ofd_add_file(ofd, "index.html", file_ori);
    DESTROY(file_ori, ori);

    /* Encode, then decode, then verify */
    buffer_t* encoded = ofd_encode(ofd);
    ASSERT_NE(encoded, nullptr);
    ofd_destroy(ofd);

    ofd_t* decoded = ofd_decode(encoded);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->entries.length, 1);

    ofd_entry_t* entry = ofd_find(decoded, "index.html");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->type, OFD_ENTRY_FILE);
    ASSERT_NE(entry->file_ori, nullptr);
    EXPECT_EQ(entry->file_ori->block_type, standard);
    EXPECT_EQ(entry->file_ori->tuple_size, 3u);

    buffer_destroy(encoded);
    ofd_destroy(decoded);
}