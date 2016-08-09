/**
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

#ifndef COM_SAVILLE_MPSCFIFO_H
#define COM_SAVILLE_MPSCFIFO_H

#include <stdint.h>

typedef struct MpscFifo_t MpscFifo_t;
typedef struct Msg_t Msg_t;

#define VOLATILE volatile

#define USE_ATOMIC_TYPES 0

typedef struct Msg_t {
#if USE_ATOMIC_TYPES
  _Atomic(Msg_t*) VOLATILE pNext __attribute__ (( aligned (64) )); // Next message
#else
  Msg_t* pNext __attribute__ (( aligned (64) )); // Next message
#endif
  MpscFifo_t* pPool;
  MpscFifo_t* pRspQ;
  uint64_t arg1;
  uint64_t arg2;
} Msg_t;

typedef struct MpscFifo_t {
#if USE_ATOMIC_TYPES
  _Atomic(Msg_t*) VOLATILE pHead __attribute__(( aligned (64) ));
  _Atomic(Msg_t*) pTail __attribute__(( aligned (64) ));
#else
  Msg_t* pHead __attribute__(( aligned (64) ));
  Msg_t* pTail __attribute__(( aligned (64) ));
#endif
  VOLATILE _Atomic(uint32_t) count;
} MpscFifo_t;

extern _Atomic(uint64_t) gTick;

#define LDR "%6ld %lx  "
#define ldr() ++gTick, pthread_self()

#define CRASH() do { *((volatile uint8_t*)0) = 0; } while(0)


/**
 * Initialize an MpscFifo_t. Don't forget to empty the fifo
 * and delete the stub before freeing MpscFifo_t.
 */
extern MpscFifo_t *initMpscFifo(MpscFifo_t *pQ, Msg_t *pStub);

/**
 * Deinitialize the MpscFifo_t and return the stub if this routine
 * can't return it to its pool.  Assumes the fifo is empty and
 * the only member is the stub.
 */
extern Msg_t *deinitMpscFifo(MpscFifo_t *pQ);

/**
 * Add a Msg_t to the Queue. This maybe used by multiple
 * entities on the same or different thread. This will never
 * block as it is a wait free algorithm.
 */
extern void add(MpscFifo_t *pQ, Msg_t *pMsg);

/**
 * Remove a Msg_t from the Queue. This maybe used only by
 * a single thread and returns NULL if empty or would
 * have blocked.
 */
extern Msg_t *rmv_non_stalling(MpscFifo_t *pQ);

/**
 * Remove a Msg_t from the Queue. This maybe used only by
 * a single thread and returns NULL if empty. This may
 * stall if a producer call add and was preempted before
 * finishing.
 */
extern Msg_t *rmv(MpscFifo_t *pQ);

/**
 * Remove a Msg_t from the Queue DO NOT PRINT DBG output if empty.
 * This maybe used only by a single thread and returns NULL if empty.
 * This may stall if a producer call add and was preempted before
 * finishing.
 */
extern Msg_t *rmv_no_dbg_on_empty(MpscFifo_t *pQ);

/**
 * Return the message to its pool.
 */
extern void ret_msg(Msg_t* pMsg);

/**
 * Send a response arg1 if the msg->pRspQ != NULL otherwise ret msg
 */
extern void send_rsp_or_ret(Msg_t* msg, uint64_t arg1);

#endif
