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
  DPF("%ld  initMpscFifo:*pQ=%p stub=%p stub->pPool=%p\n",
      pthread_self(), pQ, pStub, pStub->pPool);
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
    DPF("%ld  deinitMpscFifo:-pQ=%p no pool for stub=%p\n",
        pthread_self(), pQ, pStub);
    return pStub;
  } else if (pStub->pPool == pQ) {
    // Can't return the stub to the poll we're deinitializing
    DPF("%ld  deinitMpscFifo:-pQ=%p don't ret our own stub=%p\n",
        pthread_self(), pQ, pStub);
    return NULL;
  } else {
    // Return the stub to the pool
    DPF("%ld  deinitMpscFifo:+pQ=%p ret stub=%p stub->pPool=%p\n",
        pthread_self(), pQ, pStub, pStub->pPool);
    ret(pStub);
    DPF("%ld  deinitMpscFifo:-pQ=%p ret stub=%p stub->pPool=%p\n",
        pthread_self(), pQ, pStub, pStub->pPool);
    return NULL;
  }
}

/**
 * @see mpscifo.h
 */
void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  DPF("%ld  add:+pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pMsg, pMsg->arg1, pMsg->arg2);
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  // rmv will stall spinning if preemted at this critical spot
  pPrev->pNext = pMsg;
  DPF("%ld  add:-pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pMsg, pMsg->arg1, pMsg->arg2);
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv_non_stalling(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if (pNext != NULL) {
    pTail->pRspQ = pNext->pRspQ;
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    DPF("%ld  rmv_non_stailling: got msg pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pTail, pTail->arg1, pTail->arg2);
    return pTail;
  } else {
    DPF("%ld  rmv_non_stalling: 'empty' pQ=%p msg=NULL\n", pthread_self(), pQ);
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
    DPF("%ld  rmv: empty pQ=%p pNext=%p pTail=%p == pHead=%p\n",
        pthread_self(), pQ, pNext, pTail, pQ->pHead);
    return NULL;
  } else {
    if (pNext == NULL) {
      // Q is NOT empty but producer was preempted at the critical spot
      DPF("%ld  rmv: stalling pQ=%p pNext=%p pTail=%p != pHead=%p\n",
          pthread_self(), pQ, pNext, pTail, pQ->pHead);
      uint32_t i;
      for (i = 0; (pNext = pTail->pNext) == NULL; i++) {
        sched_yield();
      }
    }
    pTail->pRspQ = pNext->pRspQ;
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    DPF("%ld  rmv: got msg pQ=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pQ, pTail, pTail->arg1, pTail->arg2);
    return pTail;
  }
}

/**
 * @see mpscifo.h
 */
Msg_t *rmv_no_dbg_on_empty(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if ((pNext == NULL) && (pTail == pQ->pHead)) {
    // Q is empty
    return NULL;
  } else {
    return rmv(pQ);
  }
}

/**
 * @see mpscfifo.h
 */
void ret(Msg_t* pMsg) {
  if ((pMsg != NULL) && (pMsg->pPool != NULL)) {
    DPF("%ld  ret: pool=%p msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), pMsg->pPool, pMsg, pMsg->arg1, pMsg->arg2);
    add(pMsg->pPool, pMsg);
  } else {
    if (pMsg == NULL) {
      DPF("%ld  ret: No msg msg=%p\n", pthread_self(), pMsg);
    } else {
      DPF("%ld  ret: No pool pool=%p msg=%p arg1=%lu arg2=%lu\n",
          pthread_self(), pMsg->pPool, pMsg, pMsg->arg1, pMsg->arg2);
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
    DPF("%ld  send_rsp_or_ret: send pRspQ=%p rsp arg1=%lu arg2=%lu\n",
        pthread_self(), pRspQ, msg->arg1, msg->arg2);
    add(pRspQ, msg);
  } else {
    DPF("%ld  send_rsp_or_ret: ret msg=%p arg1=%lu arg2=%lu\n",
        pthread_self(), msg, msg->arg1, msg->arg2);
    ret(msg);
  }
}
