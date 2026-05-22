#include <gtest/gtest.h>
extern "C" {
#include "../src/Configuration/config.h"
}

TEST(TestConfigValidate, RejectsNullConfig) {
  EXPECT_NE(config_validate(NULL), 0);
}

TEST(TestConfigValidate, DefaultConfigPasses) {
  config_t config = config_default();
  EXPECT_EQ(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroIndexBucketSize) {
  config_t config = config_default();
  config.index_bucket_size = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroIndexWait) {
  config_t config = config_default();
  config.index_wait = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsIndexMaxWaitLessThanWait) {
  config_t config = config_default();
  config.index_wait = 100;
  config.index_max_wait = 50;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, IndexMaxWaitEqualsWaitPasses) {
  config_t config = config_default();
  config.index_wait = 100;
  config.index_max_wait = 100;
  EXPECT_EQ(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroSchedulerThreads) {
  config_t config = config_default();
  config.scheduler_thread_count = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsTooManySchedulerThreads) {
  config_t config = config_default();
  config.scheduler_thread_count = 257;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroGossipInitInterval) {
  config_t config = config_default();
  config.gossip_init_interval_s = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsGossipSteadyLessThanInit) {
  config_t config = config_default();
  config.gossip_init_interval_s = 60;
  config.gossip_steady_interval_s = 30;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroGossipTimeout) {
  config_t config = config_default();
  config.gossip_timeout_ms = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsInverseDecayFactor) {
  config_t config = config_default();
  config.hebbian_decay_factor = 0.0f;
  EXPECT_NE(config_validate(&config), 0);

  config = config_default();
  config.hebbian_decay_factor = 1.0f;
  EXPECT_NE(config_validate(&config), 0);

  config = config_default();
  config.hebbian_decay_factor = 1.5f;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroEabfMaintenance) {
  config_t config = config_default();
  config.eabf_maintenance_ms = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsEabfBaseLessThanMaintenance) {
  config_t config = config_default();
  config.eabf_base_ttl_ms = 5000;
  config.eabf_maintenance_ms = 10000;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroTauMin) {
  config_t config = config_default();
  config.respiration_tau_min_ms = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsTauMaxLessThanTauMin) {
  config_t config = config_default();
  config.respiration_tau_min_ms = 100000;
  config.respiration_tau_max_ms = 50000;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroMaxRetries) {
  config_t config = config_default();
  config.relay_max_retries = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroRetryDelay) {
  config_t config = config_default();
  config.relay_retry_delay_ms = 0;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, RejectsZeroGossipInitCount) {
  config_t config = config_default();
  config.gossip_init_count = 0;
  EXPECT_NE(config_validate(&config), 0);
}
