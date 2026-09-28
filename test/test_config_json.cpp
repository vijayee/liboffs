#include <gtest/gtest.h>
#include <cJSON.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
extern "C" {
#include "../src/Configuration/config_json.h"
#include "../src/Configuration/config_pending.h"
#include "../src/Configuration/config.h"
}

namespace fs = std::filesystem;

TEST(ConfigJson, KnownFields) {
  EXPECT_TRUE(config_is_known_field("api_key_hash"));
  EXPECT_TRUE(config_is_known_field("bootstrap_peers"));
  EXPECT_TRUE(config_is_known_field("cache_dir"));
  EXPECT_TRUE(config_is_known_field("http_port"));
  EXPECT_TRUE(config_is_known_field("tcp_tls_enabled"));
  EXPECT_TRUE(config_is_known_field("https_cert_path"));
  EXPECT_FALSE(config_is_known_field("bogus_field"));
  EXPECT_FALSE(config_is_known_field(""));
  EXPECT_FALSE(config_is_known_field(NULL));
}

TEST(ConfigJson, FieldTypeClassification) {
  EXPECT_EQ(CONFIG_FIELD_STRING, config_field_type("api_key_hash"));
  EXPECT_EQ(CONFIG_FIELD_STRING, config_field_type("bootstrap_peers"));
  EXPECT_EQ(CONFIG_FIELD_STRING, config_field_type("cache_dir"));
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
  char peers_csv[] = "10.0.0.1:8080";
  cfg.bootstrap_peers = peers_csv;
  cJSON* json = config_to_json(&cfg);
  ASSERT_NE(json, nullptr);
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "cache_size"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "http_port"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "http_enabled"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "api_key_hash"));
  EXPECT_NE(nullptr, cJSON_GetObjectItem(json, "tcp_tls_enabled"));
  cJSON* peers = cJSON_GetObjectItem(json, "bootstrap_peers");
  ASSERT_NE(peers, nullptr);
  EXPECT_TRUE(cJSON_IsString(peers));
  EXPECT_STREQ("10.0.0.1:8080", peers->valuestring);
  cJSON* port = cJSON_GetObjectItem(json, "http_port");
  ASSERT_NE(port, nullptr);
  EXPECT_TRUE(cJSON_IsNumber(port));
  cJSON_Delete(json);
  /* config_default() leaves all other string fields NULL, and the
     bootstrap_peers value assigned above is a stack literal, so there is
     nothing heap-allocated to release — config_free() would free() the stack
     struct, so it must not be called on a value-return config_default(). */
}

/* The only JSON->config_t parse entry point is config_pending_load (it reads
   {data_dir}/pending_config.json), so this test stages a pending file with the
   CSV and asserts the raw string survives into the field. The CSV is stored
   verbatim; endpoint validation happens later via
   authority_set_bootstrap_peers. */
TEST(ConfigJson, BootstrapPeersFieldParses) {
  fs::path dir = fs::temp_directory_path() / "liboffs_config_bootstrap_peers_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "pending_config.json");
    out << "{\"bootstrap_peers\": \"10.0.0.1:8080,[2001:db8::1]:9090\"}";
  }
  config_t* config = config_pending_load(dir.string().c_str());
  std::error_code remove_ec;
  fs::remove_all(dir, remove_ec);
  ASSERT_NE(config, nullptr);
  ASSERT_NE(config->bootstrap_peers, nullptr);
  EXPECT_STREQ("10.0.0.1:8080,[2001:db8::1]:9090", config->bootstrap_peers);
  config_free(config);
}

/* cache_dir stages through the same pending file and loads into the new
   config_t field (the `offs cache move` persistence path). */
TEST(ConfigJson, CacheDirStagesAndLoads) {
  fs::path dir = fs::temp_directory_path() / "liboffs_config_cache_dir_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "pending_config.json");
    out << "{\"cache_dir\": \"D:/offs/cache\"}";
  }
  config_t* config = config_pending_load(dir.string().c_str());
  std::error_code remove_ec;
  fs::remove_all(dir, remove_ec);
  ASSERT_NE(config, nullptr);
  ASSERT_NE(config->cache_dir, nullptr);
  EXPECT_STREQ("D:/offs/cache", config->cache_dir);
  config_free(config);
}

/* A JSON null removes the key from the pending file entirely, which
   restores the config_default value (NULL = platform default) on the
   next load — the staging-revert path of `offs cache move`. The
   pending file is a whole-file flat merge (config_pending_save always
   rewrites), so the revert is a second save, not an append. */
TEST(ConfigJson, CacheDirNullRevertsToDefault) {
  fs::path dir = fs::temp_directory_path() / "liboffs_config_cache_dir_null_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::string dir_str = dir.string();
  ASSERT_EQ(0, config_pending_save(dir_str.c_str(),
                                   "{\"cache_dir\": \"C:/temp/first\"}",
                                   strlen("{\"cache_dir\": \"C:/temp/first\"}")));
  ASSERT_EQ(0, config_pending_save(dir_str.c_str(), "{\"cache_dir\": null}",
                                   strlen("{\"cache_dir\": null}")));
  config_t* config = config_pending_load(dir_str.c_str());
  std::error_code remove_ec;
  fs::remove_all(dir, remove_ec);
  ASSERT_NE(config, nullptr);
  EXPECT_EQ(nullptr, config->cache_dir);
  config_free(config);
}

/* The resize wire command stages max_capacity_bytes through the same
   pending file; 1 TiB must round-trip through the double exactly (it is
   well under 2^53). */
TEST(ConfigJson, MaxCapacityBytesStagesAndLoads) {
  fs::path dir = fs::temp_directory_path() / "liboffs_config_max_capacity_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "pending_config.json");
    out << "{\"max_capacity_bytes\": 1099511627776}";
  }
  config_t* config = config_pending_load(dir.string().c_str());
  std::error_code remove_ec;
  fs::remove_all(dir, remove_ec);
  ASSERT_NE(config, nullptr);
  EXPECT_EQ(1099511627776u, config->max_capacity_bytes);
  config_free(config);
}

TEST(ConfigJson, ConfigToJsonEmitsCacheDir) {
  config_t cfg = config_default();
  cJSON* json = config_to_json(&cfg);
  ASSERT_NE(json, nullptr);
  cJSON* absent = cJSON_GetObjectItem(json, "cache_dir");
  ASSERT_NE(absent, nullptr);
  EXPECT_TRUE(cJSON_IsNull(absent));
  cJSON_Delete(json);

  char stack_dir[] = "D:/offs";
  cfg.cache_dir = stack_dir;
  json = config_to_json(&cfg);
  ASSERT_NE(json, nullptr);
  cJSON* present = cJSON_GetObjectItem(json, "cache_dir");
  ASSERT_NE(present, nullptr);
  EXPECT_TRUE(cJSON_IsString(present));
  EXPECT_STREQ("D:/offs", present->valuestring);
  cJSON_Delete(json);
  /* Stack literal, value-config: nothing heap-allocated to release and
     config_free() must not be called (see ConfigToJsonSerializesAllFieldGroups). */
}
