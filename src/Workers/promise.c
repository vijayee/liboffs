//
// Created by victor on 4/8/25.
//
#include "promise.h"
#include "../Util/log.h"
#include <stdio.h>
#include <stdlib.h>
void promise_resolve(promise_t* promise, void* payload) {
  if (promise->resolve == NULL) {
    log_error("Unresolvable Promise");
    abort();
  }
  if (promise->hasFired == 0) {
    promise->hasFired = 1;
    promise->resolve(promise->ctx, payload);
  }
}

void promise_reject(promise_t* promise, async_error_t* error) {
  if (promise->reject == NULL) {
    char err[100];
    sprintf(err,"Unhandled Error -%s:%d %s", error->file, error->line, error->message);
    log_error(err);
    abort();
  }
  if (promise->hasFired == 0) {
    promise->hasFired = 1;
    promise->reject(promise->ctx, error);
  }
}