//
// Created by victor on 6/16/25.
//

#ifndef OFFS_TICKER_H
#define OFFS_TICKER_H
#include <stdint.h>
typedef struct {
  void* ctx;
  void (* cb)(void*);
} ticker_t;
void ticker_start(ticker_t ticker, uint64_t delay);
#endif //OFFS_TICKER_H
