/*
 * This software is released into the public domain.
 *
 * A MpscFifo is a wait free/thread safe multi-producer
 * single consumer first in first out queue. This algorithm
 * is from Dimitry Vyukov's non intrusive MPSC code here:
 *   http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
 *
 * The fifo has a head and tail, the elements are added
 * to the head of the queue and removed from the tail.
 * To allow for a wait free algorithm a stub element is used
 * so that a single atomic instruction can be used to add and
 * remove an element. Therefore, when you create a queue you
 * must pass in an areana which is used to manage the stub.
 *
 * A consequence of this algorithm is that when you add an
 * element to the queue a different element is returned when
 * you remove it from the queue. Of course the contents are
 * the same but the returned pointer will be different.
 */
#include "mpscfifo.h"

#include <sys/types.h>
#include <pthread.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @see mpscfifo.h
 */
MpscFifo_t *initMpscFifo(MpscFifo_t *pQ, Msg_t *pStub) {
  pStub->pNext = NULL;
  pQ->pHead = pStub;
  pQ->pTail = pStub;
  pQ->count = 0;
  return pQ;
}

/**
 * @see mpscfifo.h
 */
Msg_t *deinitMpscFifo(MpscFifo_t *pQ) {
  Msg_t *pStub = pQ->pHead;
  pQ->pHead = NULL;
  pQ->pTail = NULL;
  return pStub;
}

/**
 * @see mpscifo.h
 */
void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  pPrev->pNext = pMsg;
  pQ->count += 1;
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv(MpscFifo_t *pQ) {
  uint32_t initial_count = pQ->count;
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if (pNext != NULL) {
    pTail->data = pNext->data;
    pQ->pTail = pNext;
    pQ->count -= 1;
    return pTail;
  } else {
    if (initial_count != 0) {
      printf("rmv: BUG pNext == NULL but initial_count=%d\n", initial_count);
    }
    return NULL;
  }
}
