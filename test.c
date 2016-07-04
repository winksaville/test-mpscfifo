/**
 * This software is released into the public domain.
 */

#define NDEBUG

#define _DEFAULT_SOURCE

#include "mpscfifo.h"

#include <sys/types.h>
#include <pthread.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>

typedef _Atomic(uint64_t) Counter;
//static typedef uint64_t Counter;

typedef struct ClientParams {
  pthread_t thread;
  MpscFifo_t* fifo;

  _Atomic(bool) done;
  _Atomic(Msg_t*) msg;
  uint64_t error_count;
  uint64_t count;
  sem_t sem_ready;
  sem_t sem_waiting;
} ClientParams;


#ifdef NDEBUG
#define DPF(format, ...) ((void)(0))
#else
#define DPF(format, ...)  printf(format, __VA_ARGS__)
#endif

static void* client(void* p) {
  DPF("client:+param=%p\n", p);
  ClientParams* cp = (ClientParams*)p;

  // Signal we're ready
  sem_post(&cp->sem_ready);

  // While we're not done wait for a signal to do work
  // do the work and signal work is complete.
  while (!cp->done) {
    sem_wait(&cp->sem_waiting);

    if (!cp->done) {
      cp->count += 1;
      Msg_t* msg = cp->msg;
      if (msg == NULL) {
        cp->error_count += 1;
      } else {
        add(cp->fifo, msg);
        cp->msg = NULL;
      }
    }
  }

  // Get any message waiting ran add it back to its fifo
  Msg_t* msg = cp->msg;
  if (msg != NULL) {
    add(cp->fifo, msg);
  }

  DPF("client:-param=%p\n", p);

  return NULL;
}

bool multi_thread_main(const uint32_t client_count, const uint64_t loops,
    const uint32_t msg_count) {
  bool error;
  ClientParams clients[client_count];
  MpscFifo_t fifo;
  uint32_t clients_created = 0;
  uint64_t msgs_count = 0;
  uint64_t no_msgs_count = 0;
  uint64_t not_ready_client_count = 0;


  printf("multi_thread_msg:+client_count=%d loops=%ld msg_count=%d\n",
      client_count, loops, msg_count);

  // Allocate messages
  Msg_t* msgs = malloc(sizeof(Msg_t) * (msg_count + 1));
  if (msgs == NULL) {
    printf("multi_thread_msg: Unable to allocate messages, aborting\n");
    error = true;
    goto done;
  }

  // Output info on the fifo and messages
  printf("multi_thread_msg: &msgs[0]=%p &msgs[1]=%p sizeof(Msg_t)=%ld(0x%lx)\n",
      &msgs[0], &msgs[1], sizeof(Msg_t), sizeof(Msg_t));
  printf("multi_thread_msg: &fifo=%p, &pHead=%p, &pTail=%p sizeof(fifo)=%ld(0x%lx)\n",
      &fifo, &fifo.pHead, &fifo.pTail, sizeof(fifo), sizeof(fifo));

  // Init the fifo with the first msg as the stub
  initMpscFifo(&fifo, &msgs[0]);

  // Add the remaining messages
  for (uint32_t i = 1; i <= msg_count; i++) {
    DPF("multi_thread_msg: add %d msg=%p\n", i, &msgs[i]);
    // Cast away the constantness to initialize
    add(&fifo, &msgs[i]);
  }
  printf("multi_thread_msg: after creating pool fifo.count=%d\n", fifo.count);

  // Create the clients
  for (uint32_t i = 0; i < client_count; i++, clients_created++) {
    ClientParams* param = &clients[i];
    param->done = false;
    param->msg = NULL;
    param->error_count = 0;
    param->count = 0;
    param->fifo = &fifo;

    sem_init(&param->sem_ready, 0, 0);
    sem_init(&param->sem_waiting, 0, 0);

    int retv = pthread_create(&param->thread, NULL, client, (void*)&clients[i]);
    if (retv != 0) {
      printf("multi_thread_msg: error thread creation , clients[%u]=%p retv=%d\n",
          i, param, retv);
      error = true;
      goto done;
    }

    // Wait until it starts
    sem_wait(&param->sem_ready);
  }
  printf("multi_thread_msg: created %u clients\n", clients_created);

  // Loop though all the clients writing a message to them
  for (uint32_t i = 0; i < loops; i++) {
    for (uint32_t c = 0; c < clients_created; c++) {
      ClientParams* client = &clients[c];
      Msg_t* msg = client->msg;
      if (msg == NULL) {
        msg = rmv(&fifo);
        if (msg != NULL) {
          msgs_count += 1;
          client->msg = msg;
          sem_post(&client->sem_waiting);
        } else {
          no_msgs_count += 1;
          sched_yield();
        }
      } else {
        DPF("multi_thread_msg: client=%p msg=%p != NULL\n", client, msg);
        not_ready_client_count += 1;
        sched_yield();
      }
    }
  }

  error = false;

done:
  printf("multi_thread_msg: done, joining %u clients\n", clients_created);
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* param = &clients[i];

    // Signal the client to stop
    param->done = true;
    sem_post(&param->sem_waiting);

    // Wait until the thread completes
    int retv = pthread_join(param->thread, NULL);
    if (retv != 0) {
      printf("multi_thread_msg: joining failed, clients[%u]=%p retv=%d\n",
          i, (void*)param, retv);
    }

    sem_destroy(&param->sem_ready);
    sem_destroy(&param->sem_waiting);
    if (param->error_count != 0) {
      printf("multi_thread_msg: clients[%u]=%p error_count=%ld\n",
          i, (void*)param, param->error_count);
      error = true;
    }
  }

  // Remove all msgs
  printf("multi_thread_msg: fifo.count=%d\n", fifo.count);
  Msg_t* msg;
  uint32_t rmv_count = 0;
  while ((msg = rmv(&fifo)) != NULL) {
    rmv_count += 1;
    DPF("multi_thread_msg: remove msg=%p\n", msg);
  }
  printf("multi_thread_msg: fifo had %d msgs expected %d fifo.count=%d\n",
      rmv_count, msg_count, fifo.count);
  error |= rmv_count != msg_count;

  deinitMpscFifo(&fifo);
  if (msgs != NULL) {
    free(msgs);
  }

  uint64_t expected_value = loops * clients_created;
  uint64_t sum = msgs_count + no_msgs_count + not_ready_client_count;
  printf("multi_thread_msg: sum=%ld expected_value=%ld\n", sum, expected_value);
  printf("multi_thread_msg: msgs_count=%ld no_msgs_count=%ld not_ready_client_count=%ld\n",
      msgs_count, no_msgs_count, not_ready_client_count);

  error |= sum != expected_value;
  printf("multi_thread_msg:-error=%d\n\n", error);

  return error;
}

int main(int argc, char *argv[]) {
  bool error = false;

  if (argc != 4) {
    printf("Usage:\n");
    printf(" %s client_count loops msg_count\n", argv[0]);
    return 1;
  }

  u_int32_t client_count = strtoul(argv[1], NULL, 10);
  u_int64_t loops = strtoull(argv[2], NULL, 10);
  u_int32_t msg_count = strtoul(argv[3], NULL, 10);

  printf("test client_count=%d loops=%ld\n", client_count, loops);

  error |= multi_thread_main(client_count, loops, msg_count);

  if (!error) {
    printf("Success\n");
  }

  return error ? 1 : 0;
}
