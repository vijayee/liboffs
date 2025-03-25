//
// Created by victor on 3/22/25.
//

#include <gtest/gtest.h>
extern "C" {
#include "../src/RefCounter/refcounter.h"
#include "../src/RefCounter/refcounter.p.h"
}

TEST(TestRefCounter, TestRefCounterFunctions) {
  refcounter_t* refc1 = (refcounter_t*) calloc(sizeof(refcounter_t), 1);
  refcounter_init(refc1);

  refcounter_t* refc2 = (refcounter_t*) refcounter_reference(refc1);
  EXPECT_EQ(refc1, refc2);

  EXPECT_EQ(refcounter_count(refc1), 2);
}
