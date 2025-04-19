//
// Created by victor on 4/15/25.
//
#include "queue.h"
#include "../Util/util.h"
#include "../Util/allocator.h"


work_queue_t workQueue;

void work_queue_init() {
  workQueue.first = NULL;
  workQueue.last = NULL;
  platform_lock_init(&workQueue.lock);
}
void work_enqueue(work_t* work) {
  platform_lock(&workQueue.lock);
  if (workQueue.first == NULL && workQueue.last == NULL) {
    work_queue_item_t* item = get_clear_memory(sizeof(work_queue_item_t));
    item->last = NULL;
    item->next = NULL;
    item->work = work;
    workQueue.first = item;
    workQueue.last = item;
  } else {
    work_queue_item_t* new_item = get_clear_memory(sizeof(work_queue_item_t));
    new_item->work= work;
    work_queue_item_t* current = workQueue.first;

    int cmp = priority_compare(&work->priority,&current->work->priority);
    while ((current->next != NULL ) && ((cmp == 1) || (cmp == 0))) {
      current = current->next;
      cmp = priority_compare(&work->priority,&current->work->priority);
    }
    if ((cmp == -1) && (current->next == NULL)) {// current is the last in the list and the new work has greater priority
      new_item->last = current->last;
      new_item->next = current;
      if (current->last != NULL) { // current is not the first item in the list
        work_queue_item_t* temp = current->last;
        temp->next = new_item;
      } else {// current is the first in the list and new work should become the new first
        workQueue.first= new_item;
      }
      current->last = new_item;
    } else if ((cmp != -1) && current->next == NULL) { //current is the last in the list and the new work should come after it
      current->next = new_item;
      new_item->last = current;
      workQueue.last = new_item;
    } else if (current->last == NULL) { // work should be at the begining of a populated list
      new_item->next = current;
      current->last = new_item;
      workQueue.first = new_item;
    } else { // we are somewhere in the middle of the list and work has greater priority than the current
      work_queue_item_t* temp = current->last;
      temp->next = new_item;
      new_item->last= temp;
      new_item->next = current;
      current->last = new_item;
    }
  }
  platform_unlock(&workQueue.lock);
}
work_t* work_dequeue() {
  platform_lock(&workQueue.lock);
  if (workQueue.first == NULL) {
    platform_unlock(&workQueue.lock);
    return NULL;
  } else {
    work_queue_item_t* temp = workQueue.first;
    work_t* work= temp->work;
    workQueue.first= temp->next;
    if(workQueue.first != NULL) {
      workQueue.first->last = NULL;
    }
    if (workQueue.last == temp) {
      workQueue.last = NULL;
    }
    free(temp);
    platform_unlock(&workQueue.lock);
    return work;
  }

}
