//
// Created by victor on 6/16/25.
//
#include "ticker.h"
#include "time.h"
void ticker_start(ticker_t ticker, uint64_t delay) {
  struct timespec ts;
  ts.tv_nsec = delay;
  nanosleep(&ts, NULL);
  ticker.cb(ticker.ctx);
}