#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/Util/base58.h"
#include "../src/OFFStreams/off_url.h"
#include "../src/Buffer/buffer.h"
}

TEST(TestBase58, EncodeDecodeRoundTrip) {
    const uint8_t input[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    size_t input_len = sizeof(input);

    size_t encoded_len = base58_encoded_length(input_len);
    char* encoded = new char[encoded_len + 1];
    int result = base58_encode(input, input_len, encoded, encoded_len + 1);
    ASSERT_GT(result, 0);
    encoded[result] = '\0';

    size_t decoded_len = base58_decoded_length(result);
    uint8_t* decoded = new uint8_t[decoded_len];
    size_t bytes_written = 0;
    int decode_result = base58_decode(encoded, decoded, decoded_len, &bytes_written);
    ASSERT_EQ(decode_result, 0);
    EXPECT_EQ(bytes_written, input_len);
    EXPECT_EQ(memcmp(decoded, input, input_len), 0);

    delete[] encoded;
    delete[] decoded;
}

TEST(TestBase58, EncodeLeadingZeros) {
    const uint8_t input[] = {0x00, 0x00, 0x01, 0x02};
    size_t input_len = sizeof(input);

    size_t encoded_len = base58_encoded_length(input_len);
    char* encoded = new char[encoded_len + 1];
    int result = base58_encode(input, input_len, encoded, encoded_len + 1);
    ASSERT_GT(result, 0);
    encoded[result] = '\0';

    EXPECT_EQ(encoded[0], '1');
    EXPECT_EQ(encoded[1], '1');

    size_t decoded_len = base58_decoded_length(result);
    uint8_t* decoded = new uint8_t[decoded_len];
    size_t bytes_written = 0;
    base58_decode(encoded, decoded, decoded_len, &bytes_written);
    EXPECT_EQ(bytes_written, input_len);
    EXPECT_EQ(memcmp(decoded, input, input_len), 0);

    delete[] encoded;
    delete[] decoded;
}

TEST(TestBase58, EncodeDecode32Bytes) {
    uint8_t input[32];
    for (int i = 0; i < 32; i++) input[i] = (uint8_t)(i * 8 + 3);

    size_t encoded_len = base58_encoded_length(32);
    char* encoded = new char[encoded_len + 1];
    int result = base58_encode(input, 32, encoded, encoded_len + 1);
    ASSERT_GT(result, 0);
    encoded[result] = '\0';

    size_t decoded_len = base58_decoded_length(result);
    uint8_t* decoded = new uint8_t[decoded_len];
    size_t bytes_written = 0;
    base58_decode(encoded, decoded, decoded_len, &bytes_written);
    EXPECT_EQ(bytes_written, 32);
    EXPECT_EQ(memcmp(decoded, input, 32), 0);

    delete[] encoded;
    delete[] decoded;
}

TEST(TestBase58, DecodeInvalidChar) {
    char invalid[] = "123Oabc";
    uint8_t output[32];
    size_t bytes_written = 0;
    int result = base58_decode(invalid, output, sizeof(output), &bytes_written);
    EXPECT_EQ(result, -1);
}

TEST(TestBase58, EmptyInput) {
    char encoded[1];
    int result = base58_encode(NULL, 0, encoded, sizeof(encoded));
    EXPECT_EQ(result, 0);

    uint8_t output[1];
    size_t bytes_written = 0;
    result = base58_decode("", output, sizeof(output), &bytes_written);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(bytes_written, 0);
}

TEST(TestBase58, NullOutput) {
    const uint8_t input[] = {0x01, 0x02};
    int result = base58_encode(input, sizeof(input), NULL, 10);
    EXPECT_EQ(result, -1);
}

TEST(TestOffUrl, ParseOffUrlWithSlashContentType) {
    // Content type "application/octet-stream" contains a slash
    const char* url = "/offsystem/v3/application/octet-stream/1024/5Q8oZHMQF7oJ4WJFrnPaLm2bYQKb6VksPZgKqXKRvJph/3vZ7EeVNFrYNA2J8oJqHn5WQkL4cBF9puXMXdzGwoRmt/readme.txt";
    off_url_t* parsed = off_url_parse(url);
    ASSERT_NE(parsed, nullptr);
    EXPECT_STREQ(parsed->content_type, "application/octet-stream");
    EXPECT_EQ(parsed->stream_length, 1024);
    EXPECT_NE(parsed->file_hash, nullptr);
    EXPECT_NE(parsed->descriptor_hash, nullptr);
    EXPECT_STREQ(parsed->file_name, "readme.txt");
    off_url_destroy(parsed);
}

TEST(TestOffUrl, ParseDirectoryUrl) {
    const char* url = "/offsystem/v3/offsystem/directory/2048/5Q8oZHMQF7oJ4WJFrnPaLm2bYQKb6VksPZgKqXKRvJph/3vZ7EeVNFrYNA2J8oJqHn5WQkL4cBF9puXMXdzGwoRmt/mysite.ofd";
    off_url_t* parsed = off_url_parse(url);
    ASSERT_NE(parsed, nullptr);
    EXPECT_STREQ(parsed->content_type, "offsystem/directory");
    EXPECT_EQ(parsed->stream_length, 2048);
    off_url_destroy(parsed);
}

TEST(TestOffUrl, ParseInvalidUrl) {
    off_url_t* result = off_url_parse("http://example.com/not-off");
    EXPECT_EQ(result, nullptr);
}

TEST(TestOffUrl, ParseNullUrl) {
    off_url_t* result = off_url_parse(NULL);
    EXPECT_EQ(result, nullptr);
}

TEST(TestOffUrl, RoundTrip) {
    off_url_t* url = off_url_create();
    ASSERT_NE(url, nullptr);
    free(url->content_type);
    url->content_type = strdup("application/octet-stream");
    url->stream_length = 1024;
    free(url->file_name);
    url->file_name = strdup("test.txt");

    uint8_t fh_data[32];
    uint8_t dh_data[32];
    for (int i = 0; i < 32; i++) { fh_data[i] = (uint8_t)i; dh_data[i] = (uint8_t)(255 - i); }
    url->file_hash = buffer_create_from_pointer_copy(fh_data, 32);
    url->descriptor_hash = buffer_create_from_pointer_copy(dh_data, 32);

    char* url_str = off_url_to_string(url);
    ASSERT_NE(url_str, nullptr);

    // The URL should contain "/offsystem/v3/" followed by URL-encoded content type
    const char* offsystem_path = strstr(url_str, "/offsystem/v3/");
    ASSERT_NE(offsystem_path, nullptr);

    off_url_t* parsed = off_url_parse(offsystem_path);
    ASSERT_NE(parsed, nullptr) << "Failed to parse URL: " << offsystem_path;
    EXPECT_STREQ(parsed->content_type, "application/octet-stream");
    EXPECT_EQ(parsed->stream_length, 1024);
    EXPECT_EQ(parsed->file_hash->size, 32);
    EXPECT_EQ(parsed->descriptor_hash->size, 32);
    EXPECT_EQ(memcmp(parsed->file_hash->data, fh_data, 32), 0);
    EXPECT_EQ(memcmp(parsed->descriptor_hash->data, dh_data, 32), 0);

    off_url_destroy(parsed);
    off_url_destroy(url);
    free(url_str);
}

TEST(TestOffUrl, RoundTripDirectoryType) {
    off_url_t* url = off_url_create();
    ASSERT_NE(url, nullptr);
    free(url->content_type);
    url->content_type = strdup("offsystem/directory");
    url->stream_length = 4096;
    free(url->file_name);
    url->file_name = strdup("site.ofd");

    uint8_t fh_data[32];
    uint8_t dh_data[32];
    for (int i = 0; i < 32; i++) { fh_data[i] = (uint8_t)(i + 100); dh_data[i] = (uint8_t)(200 - i); }
    url->file_hash = buffer_create_from_pointer_copy(fh_data, 32);
    url->descriptor_hash = buffer_create_from_pointer_copy(dh_data, 32);

    char* url_str = off_url_to_string(url);
    ASSERT_NE(url_str, nullptr);

    const char* offsystem_path = strstr(url_str, "/offsystem/v3/");
    ASSERT_NE(offsystem_path, nullptr);

    off_url_t* parsed = off_url_parse(offsystem_path);
    ASSERT_NE(parsed, nullptr) << "Failed to parse URL: " << offsystem_path;
    EXPECT_STREQ(parsed->content_type, "offsystem/directory");
    EXPECT_EQ(parsed->stream_length, 4096);
    EXPECT_STREQ(parsed->file_name, "site.ofd");

    off_url_destroy(parsed);
    off_url_destroy(url);
    free(url_str);
}

TEST(TestOffUrl, FromHeaders) {
    off_url_t* url = off_url_from_headers("application/octet-stream", "data.bin", 4096, "http://localhost:9999");
    ASSERT_NE(url, nullptr);
    EXPECT_STREQ(url->content_type, "application/octet-stream");
    EXPECT_STREQ(url->file_name, "data.bin");
    EXPECT_EQ(url->stream_length, 4096);
    EXPECT_STREQ(url->server_address, "http://localhost:9999");
    off_url_destroy(url);
}

TEST(TestOffUrl, FromHeadersNullType) {
    off_url_t* url = off_url_from_headers(NULL, "data.bin", 4096, NULL);
    EXPECT_EQ(url, nullptr);
}

TEST(TestOffUrl, DestroyNull) {
    off_url_destroy(NULL);
}