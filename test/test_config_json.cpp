#include <gtest/gtest.h>
#include <cJSON.h>
extern "C" {
#include "../src/Configuration/config_json.h"
#include "../src/Configuration/config.h"
}

TEST(ConfigJson, KnownFields) {
  EXPECT_TRUE(config_is_known_field("api_key_hash"));
  EXPECT_TRUE(config_is_known_field("http_port"));
  EXPECT_TRUE(config_is_known_field("tcp_tls_enabled"));
  EXPECT_TRUE(config_is_known_field("https_cert_path"));
  EXPECT_FALSE(config_is_known_field("bogus_field"));
  EXPECT_FALSE(config_is_known_field(""));
  EXPECT_FALSE(config_is_known_field(NULL));
}

TEST(ConfigJson, FieldTypeClassification) {
  EXPECT_EQ(CONFIG_FIELD_STRING, config_field_type("api_key_hash"));
  EXPECT_EQ(CONFIG_FIELD_STRING, config_field_type("https_cert_path"));
  EXPECT_EQ(CONFIG_FIELD_BOOL, config_field_type("http_enabled"));
  EXPECT_EQ(CONFIG_FIELD_BOOL, config_field_type("tcp_tls_enabled"));
  EXPECT_EQ(CONFIG_FIELD_NUMBER, config_field_type("http_port"));
  EXPECT_EQ(CONFIG_FIELD_NUMBER, config_field_type("cache_size"));
}

TEST(ConfigJson, StringFieldValueFromLiteral) {
  cJSON* v = config_field_value_from_string("api_key_hash", "$2b$abc", NULL, 0);
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(cJSON_IsString(v));
  EXPECT_STREQ("$2b$abc", v->valuestring);
  cJSON_Delete(v);
}

TEST(ConfigJson, NullTokenRevertsAnyField) {
  for (const char* f : {"api_key_hash", "http_enabled", "http_port"}) {
    cJSON* v = config_field_value_from_string(f, "null", NULL, 0);
    ASSERT_NE(v, nullptr) << f;
    EXPECT_TRUE(cJSON_IsNull(v)) << f;
    cJSON_Delete(v);
  }
}

TEST(ConfigJson, BoolFieldValueAcceptsTrueFalseOneZero) {
  const char* bool_field = "http_enabled";
  struct Case { const char* in; int expect; };
  Case cases[] = {{"true", 1}, {"1", 1}, {"false", 0}, {"0", 0}};
  for (auto& c : cases) {
    cJSON* v = config_field_value_from_string(bool_field, c.in, NULL, 0);
    ASSERT_NE(v, nullptr) << c.in;
    EXPECT_TRUE(cJSON_IsBool(v)) << c.in;
    EXPECT_EQ(c.expect, cJSON_IsTrue(v)) << c.in;
    cJSON_Delete(v);
  }
}

TEST(ConfigJson, BoolFieldRejectsGarbage) {
  char err[128] = {0};
  cJSON* v = config_field_value_from_string("http_enabled", "maybe", err, sizeof(err));
  EXPECT_EQ(v, nullptr);
  EXPECT_STRNE(err, "");
}

TEST(ConfigJson, NumberFieldValueParsesInteger) {
  cJSON* v = config_field_value_from_string("http_port", "8080", NULL, 0);
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(cJSON_IsNumber(v));
  EXPECT_DOUBLE_EQ(8080.0, v->valuedouble);
  cJSON_Delete(v);
}

TEST(ConfigJson, NumberFieldRejectsNonInteger) {
  char err[128] = {0};
  EXPECT_EQ(nullptr, config_field_value_from_string("http_port", "abc", err, sizeof(err)));
  EXPECT_EQ(nullptr, config_field_value_from_string("http_port", "12x", err, sizeof(err)));
  EXPECT_EQ(nullptr, config_field_value_from_string("http_port", "", err, sizeof(err)));
}

TEST(ConfigJson, ConfigToJsonSerializesAllFieldGroups) {
  config_t cfg = config_default();
  cJSON* json = config_to_json(&cfg);
  ASSERT_NE(json, nullptr);
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "cache_size"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "http_port"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "http_enabled"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "api_key_hash"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "tcp_tls_enabled"));
  cJSON* port = cJSON_GetObjectItem(json, "http_port");
  ASSERT_NE(port, nullptr);
  EXPECT_TRUE(cJSON_IsNumber(port));
  cJSON_Delete(json);
  /* config_default() leaves all string fields NULL, so there is nothing
     heap-allocated to release — config_free() would free() the stack struct,
     so it must not be called on a value-return config_default(). */
}