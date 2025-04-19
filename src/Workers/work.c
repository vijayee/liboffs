//
// Created by victor on 4/15/25.
//
#include "work.h"
#include "../Util/allocator.h"

work_t* work_create(priority_t priority, void* ctx, void (* execute)(void*)) {
  work_t* work = get_clear_memory(sizeof(work_t));
  work->priority = priority;
  work->ctx = ctx;
  work->execute= execute;
  refcounter_init((refcounter_t*) work);
  return work;
}
void work_execute(work_t* work) {
  work->execute(work->ctx);
}
void work_destroy(work_t* work) {
  refcounter_dereference((refcounter_t*) work);
  if(refcounter_count((refcounter_t*) work) == 0) {
    free(work);
  }
}