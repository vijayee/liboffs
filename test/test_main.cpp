//
// Created by victor on 3/22/25.
//

#include <gtest/gtest.h>
#include <string.h>
#include <openssl/crypto.h>
extern "C" {
#include "../src/RefCounter/refcounter.h"
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/frand.h"
#include "../src/BlockCache/block.h"
#include "../src/Util/allocator.h"
#include "../src/Util/error.h"
#include "../src/BlockCache/fibonacci.h"
#include "../src/BlockCache/index.h"
#include "Platform/platform.h"
#include <time.h>
}

int main(int argc, char** argv) {
  OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, NULL);
  testing::InitGoogleTest(&argc, argv);
  int result = RUN_ALL_TESTS();
  OPENSSL_cleanup();
  return result;
}

TEST(TestFRand, TestFRandFunction) {
  uint8_t* rand = frand(20);

  buffer_t* buf1 = buffer_create_from_existing_memory((uint8_t *) get_clear_memory(20), 20);
  buffer_t* buf2 = buffer_create_from_existing_memory(rand, 20);
  buffer_t* buf3 = buffer_concat(buf1, buf2);

  buffer_destroy(buf1);
  buffer_destroy(buf2);
  buffer_destroy(buf3);
}

TEST(TestError, TestErrorCreateDestroy) {
  std::string message = "This is an error";
  char* cmessage = (char*)message.c_str();
  char* file = (char*)__FILE__;
  char* func = (char*)__func__;
  int line = __LINE__;
  async_error_t* error = error_create(cmessage, file, func, line);
  EXPECT_EQ(strcmp(error->message, cmessage), 0);
  EXPECT_NE(error->message, cmessage);
  EXPECT_EQ(strcmp(error->file, file), 0);
  EXPECT_NE(error->file, file);
  EXPECT_EQ(strcmp(error->function, func), 0);
  EXPECT_NE(error->function, func);
  EXPECT_EQ(error->line, line);
  error_destroy(error);
}

TEST(TestCoreCount, GetCoreCount) {
  int corecnt = platform_core_count();
  printf("Machine has %d cores\n", corecnt);
  EXPECT_GT(corecnt, 0);
}

TEST(TestFibonacciHitCounter, HitCounterFunctions) {
  EXPECT_EQ(fibonacci(0), 0);
  EXPECT_EQ(fibonacci(1), 1);
  EXPECT_EQ(fibonacci(2), 1);
  fibonacci_hit_counter_t  counter1 = fibonacci_hit_counter_create();
  fibonacci_hit_counter_t  counter2 = fibonacci_hit_counter_from(6,5);
  fibonacci_hit_counter_t  counter3 = fibonacci_hit_counter_from(6,5);
  EXPECT_EQ(counter2.threshold, 8);
  EXPECT_EQ(fibonacci_hit_counter_compare(&counter1, &counter2), -1);
  EXPECT_EQ(fibonacci_hit_counter_compare(&counter2, &counter1), 1);
  EXPECT_EQ(fibonacci_hit_counter_compare(&counter2, &counter3), 0);
  for (int i = 0; i < 4; i++) {
    if (i == 3) {
      EXPECT_EQ(fibonacci_hit_counter_increment(&counter2), 1);
    } else {
      EXPECT_EQ(fibonacci_hit_counter_increment(&counter2), 0);
    }
  }
  EXPECT_EQ(counter2.threshold, 13);
  EXPECT_EQ(counter2.count, 0);
  EXPECT_EQ(fibonacci_hit_counter_decrement(&counter2), 1);
  EXPECT_EQ(counter2.threshold, 8);
  EXPECT_EQ(counter2.count, 8);
  EXPECT_EQ(fibonacci_hit_counter_decrement(&counter2), 0);
}