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

#define DELAY 0

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

#include <unistd.h>

/**
 * @see mpscfifo.h
 */
MpscFifo_t *initMpscFifo(MpscFifo_t *pQ, Msg_t *pStub) {
  DPF(LDR "initMpscFifo:*pQ=%p stub=%p stub->pPool=%p\n",
      ldr(), pQ, pStub, pStub->pPool);
  pStub->pNext = NULL;
  pQ->pHead = pStub;
  pQ->pTail = pStub;
  pQ->count = 0;
  pQ->msgs_processed = 0;
  return pQ;
}

/**
 * @see mpscfifo.h
 */
uint64_t deinitMpscFifo(MpscFifo_t *pQ, Msg_t**ppStub) {
  Msg_t *pStub = pQ->pHead;
  pQ->pHead = NULL;
  pQ->pTail = NULL;
  uint64_t msgs_processed = pQ->msgs_processed;
  if (pStub->pPool == NULL) {
    // Return stub as its doesn't blelow to a pool
    DPF(LDR "deinitMpscFifo:-pQ=%p no pool for stub=%p\n", ldr(), pQ, pStub);
    if (ppStub != NULL) {
      *ppStub = pStub;
    }
  } else if (pStub->pPool == pQ) {
    // Can't return the stub to the poll we're deinitializing
    DPF(LDR "deinitMpscFifo:-pQ=%p don't ret our own stub=%p\n", ldr(), pQ, pStub);
    if (ppStub != NULL) {
      *ppStub = NULL;
    }
  } else {
    // Return the stub to the pool
    DPF(LDR "deinitMpscFifo:+pQ=%p ret stub=%p stub->pPool=%p\n", ldr(), pQ, pStub, pStub->pPool);
    ret_msg(pStub);
    DPF(LDR "deinitMpscFifo:-pQ=%p ret stub=%p stub->pPool=%p\n", ldr(), pQ, pStub, pStub->pPool);
    if (ppStub != NULL) {
      *ppStub = NULL;
    }
  }
  return msgs_processed;
}

/**
 * @see mpscifo.h
 */
#if USE_ATOMIC_TYPES

void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  DPF(LDR "add:+pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
  DPF(LDR "add: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  pMsg->pNext = NULL;
  Msg_t* pPrev = __atomic_exchange_n((Msg_t**)&pQ->pHead, pMsg, __ATOMIC_ACQ_REL);
  // rmv will stall spinning if preempted at this critical spot

#if DELAY != 0
  usleep(DELAY);
#endif

  pPrev->pNext = pMsg;
  DPF("%ld  add:-pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pMsg, pMsg->arg1, pMsg->arg2);

  DPF(LDR "add: pQ=%p count=%d pPrev=%p pPrev->pNext=%p\n", ldr(), pQ, pQ->count, pPrev, pPrev->pNext);
  DPF(LDR "add: pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  DPF(LDR "add:-pQ=%p count=%d msg=%p pool=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pQ->count, pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
}

#else

void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  pMsg->pNext = NULL;
  Msg_t* pPrev = __atomic_exchange_n(&pQ->pHead, pMsg, __ATOMIC_ACQ_REL);
  // rmv will stall spinning if preempted at this critical spot
  __atomic_store_n(&pPrev->pNext, pMsg, __ATOMIC_RELEASE); //SEQ_CST);
}

#endif

/**
 * @see mpscifo.h
 */
#if USE_ATOMIC_TYPES

Msg_t *rmv_non_stalling(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  DPF(LDR "rmv_non_stalling:0+pQ=%p count=%d pTail=%p pNext=%p\n", ldr(), pQ, pQ->count, pTail, pNext);
  if (pNext != NULL) {
    pTail->pRspQ = pNext->pRspQ;
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    DPF(LDR "rmv_non_stalling:1-is EMPTY pQ=%p count=%d msg=NULL\n", ldr(), pQ, pQ->count);
    pQ->msgs_processed += 1;
    return pTail;
  } else {
    DPF("%ld  rmv_non_stalling:2-'empty' pQ=%p msg=NULL\n", pthread_self(), pQ);
    return NULL;
  }
}

#else

Msg_t *rmv_non_stalling(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  if (pNext != NULL) {
    pTail->pRspQ = pNext->pRspQ;
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    pQ->msgs_processed += 1;
    return pTail;
  } else {
    return NULL;
  }
}

#endif

/**
 * @see mpscifo.h
 */
#if USE_ATOMIC_TYPES

Msg_t *rmv(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  DPF(LDR "rmv:0+pQ=%p count=%d pHead=%p pHead->pNext=%p pTail=%p pTail->pNext=%p\n", ldr(), pQ, pQ->count, pQ->pHead, pQ->pHead->pNext, pQ->pTail, pQ->pTail->pNext);
  if ((pNext == NULL) && (pTail == pQ->pHead)) {
    // Q is empty
    DPF(LDR "rmv:1-empty pQ=%p pNext=%p pTail=%p == pHead=%p\n", ldr(), pQ, pNext, pTail, pQ->pHead);
    return NULL;
  } else {
    if (pNext == NULL) {
      // Q is NOT empty but producer was preempted at the critical spot
      DPF(LDR "rmv:2 stalling pQ=%p count=%d pNext=NULL\n", ldr(), pQ, pQ->count);
      for (uint32_t i = 0; (pNext = pTail->pNext) == NULL; i++) {
        sched_yield();
      }
      DPF(LDR "rmv:3 stalling %i times pQ=%p count=%d pNext=%p\n", ldr(), i pQ, pQ->count, pNext);
    }
    pTail->pRspQ = pNext->pRspQ;
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    DPF(LDR "rmv:4-got msg pQ=%p msg=%p arg1=%lu arg2=%lu\n", ldr(), pQ, pTail, pTail->arg1, pTail->arg2);
    pQ->processed += 1;
    return pTail;
  }
}

#else

Msg_t *rmv(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_SEQ_CST);
  if ((pNext == NULL) && (pTail == __atomic_load_n(&pQ->pHead, __ATOMIC_ACQUIRE))) {
    return NULL;
  } else {
    if (pNext == NULL) {
      while ((pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE)) == NULL) {
        sched_yield();
      }
    }
    pTail->pRspQ = pNext->pRspQ;
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    pQ->msgs_processed += 1;
    return pTail;
  }
}

#endif

/**
 * @see mpscifo.h
 */
#if USE_ATOMIC_TYPES

Msg_t *rmv_no_dbg_on_empty(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if ((pNext == NULL) && (pTail == pQ->pHead)) {
    // Q is "empty"
    return NULL;
  } else {
    return rmv(pQ);
  }
}

#else

Msg_t *rmv_no_dbg_on_empty(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = __atomic_load_n(&pTail->pNext, __ATOMIC_ACQUIRE);
  if ((pNext == NULL) && (pTail == __atomic_load_n(&pQ->pHead, __ATOMIC_ACQUIRE))) {
    return NULL;
  } else {
    return rmv(pQ);
  }
}

#endif

/**
 * @see mpscfifo.h
 */
void ret_msg(Msg_t* pMsg) {
  if ((pMsg != NULL) && (pMsg->pPool != NULL)) {
    DPF(LDR "ret_msg: pool=%p msg=%p arg1=%lu arg2=%lu\n", ldr(), pMsg->pPool, pMsg, pMsg->arg1, pMsg->arg2);
    add(pMsg->pPool, pMsg);
  } else {
    if (pMsg == NULL) {
      DPF(LDR "ret:#No msg msg=%p\n", ldr(), pMsg);
    } else {
      DPF(LDR "ret:#No pool msg=%p pool=%p arg1=%lu arg2=%lu\n",
          ldr(), pMsg, pMsg->pPool, pMsg->arg1, pMsg->arg2);
    }
  }
}

/**
 * @see mpscfifo.h
 */
void send_rsp_or_ret(Msg_t* msg, uint64_t arg1) {
  if (msg->pRspQ != NULL) {
    MpscFifo_t* pRspQ = msg->pRspQ;
    msg->pRspQ = NULL;
    msg->arg1 = arg1;
    DPF(LDR "send_rsp_or_ret: send pRspQ=%p msg=%p pool=%p arg1=%lu arg2=%lu\n",
        ldr(), pRspQ, msg, msg->pPool, msg->arg1, msg->arg2);
    add(pRspQ, msg);
  } else {
    DPF(LDR "send_rsp_or_ret: no RspQ ret msg=%p pool=%p arg1=%lu arg2=%lu\n",
        ldr(), msg, msg->pPool, msg->arg1, msg->arg2);
    ret_msg(msg);
  }
}
