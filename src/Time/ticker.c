//
// Created by victor on 6/16/25.
//
#include "ticker.h"
#include <unistd.h>
void ticker_start(ticker_t ticker, uint64_t delay) {
  usleep(delay);
  ticker.cb(ticker.ctx);
}