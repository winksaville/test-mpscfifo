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

#define NDEBUG

#define _DEFAULT_SOURCE

#include "mpscfifo.h"
#include "dpf.h"

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
  return pQ;
}

/**
 * @see mpscfifo.h
 */
Msg_t *deinitMpscFifo(MpscFifo_t *pQ) {
  Msg_t *pStub = pQ->pHead;
  pQ->pHead = NULL;
  pQ->pTail = NULL;
  if (pStub->pPool == NULL) {
    // Return stub as its doesn't blelow to a pool
    return pStub;
  } else if (pStub->pPool == pQ) {
    // Can't return the stub to the poll we're deinitializing
    return NULL;
  } else {
    // Return the stub to the pool
    ret(pStub);
    return NULL;
  }
}

/**
 * @see mpscifo.h
 */
void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  // rmv will stall spinning if preemted at this critical spot
  pPrev->pNext = pMsg;
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv_non_stalling(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if (pNext != NULL) {
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    return pTail;
  } else {
    return NULL;
  }
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if ((pNext == NULL) && (pTail == pQ->pHead)) {
    // Q is empty
    return NULL;
  } else {
    if (pNext == NULL) {
      // Q is NOT empty but producer was preempted at the critical spot
      uint32_t i;
      for (i = 0; (pNext = pTail->pNext) == NULL; i++) {
        sched_yield();
      }
      DPF("rmv: Bad luck producer was prempted pQ=%p i=%d\n", pQ, i);
    }
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    return pTail;
  }
}

/**
 * @see mpscfifo.h
 */
void ret(Msg_t* pMsg) {
  if ((pMsg != NULL) && (pMsg->pPool != NULL)) {
    add(pMsg->pPool, pMsg);
  }
}
