/**
 * This software is released into the public domain.
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
#include <semaphore.h>

typedef _Atomic(uint64_t) Counter;
//static typedef uint64_t Counter;

typedef struct ClientParams {
  pthread_t thread;
  MpscFifo_t cmdFifo;

  _Atomic(bool) done;
  uint64_t error_count;
  uint64_t msgs_processed;
  sem_t sem_ready;
  sem_t sem_waiting;
} ClientParams;


static void* client(void* p) {
  DPF("client:+param=%p\n", p);
  Msg_t stub;
  Msg_t* msg;
  ClientParams* cp = (ClientParams*)p;

  stub.pPool = NULL;
  initMpscFifo(&cp->cmdFifo, &stub);
  DPF("client: param=%p cp->cmdFifo=%p\n", p, &cp->cmdFifo);

  // Signal we're ready
  sem_post(&cp->sem_ready);

  // While we're not done wait for a signal to do work
  // do the work and signal work is complete.
  while (!cp->done) {
    DPF("client: param=%p waiting\n", p);
    sem_wait(&cp->sem_waiting);

    DPF("client: param=%p rmv msg\n", p);
    msg = rmv(&cp->cmdFifo);
    DPF("client: param=%p got msg=%p\n", p, msg);
    if (msg != NULL) {
      cp->msgs_processed += 1;
      DPF("client: param=%p ret msg=%p msgs_processed=%lu\n",
          p, msg, cp->msgs_processed);
      ret(msg);
    }
  }

  // Flush any messages in the cmdFifo
  DPF("client: param=%p done, flushing fifo\n", p);
  uint32_t unprocessed = 0;
  while ((msg = rmv(&cp->cmdFifo)) != NULL) {
    DPF("client: param=%p ret msg=%p\n", p, msg);
    unprocessed += 1;
    ret(msg);
  }

  // Now deinit the pool
  deinitMpscFifo(&cp->cmdFifo);

  DPF("client:-param=%p error_count=%lu returned unprocessed=%u\n",
      p, cp->error_count, unprocessed);

  return NULL;
}

bool multi_thread_main(const uint32_t client_count, const uint64_t loops,
    const uint32_t msg_count) {
  bool error;
  ClientParams clients[client_count];
  MpscFifo_t pool;
  uint32_t clients_created = 0;
  uint64_t msgs_sent = 0;
  uint64_t no_msgs_count = 0;
  uint64_t not_ready_client_count = 0;


  printf("multi_thread_msg:+client_count=%u loops=%ld msg_count=%u\n",
      client_count, loops, msg_count);

  // Allocate messages
  Msg_t* msgs = malloc(sizeof(Msg_t) * (msg_count + 1));
  if (msgs == NULL) {
    printf("multi_thread_msg: ERROR Unable to allocate messages, aborting\n");
    error = true;
    goto done;
  }

  // Output info on the pool and messages
  printf("multi_thread_msg: &msgs[0]=%p &msgs[1]=%p sizeof(Msg_t)=%ld(0x%lx)\n",
      &msgs[0], &msgs[1], sizeof(Msg_t), sizeof(Msg_t));
  printf("multi_thread_msg: &pool=%p, &pHead=%p, &pTail=%p sizeof(pool)=%ld(0x%lx)\n",
      &pool, &pool.pHead, &pool.pTail, sizeof(pool), sizeof(pool));

  // Init the pool with the first msg as the stub
  msgs[0].pPool = &pool;
  initMpscFifo(&pool, &msgs[0]);

  // Add the remaining messages
  for (uint32_t i = 1; i <= msg_count; i++) {
    DPF("multi_thread_msg: add %u msg=%p\n", i, &msgs[i]);
    // Cast away the constantness to initialize
    msgs[i].pPool = &pool;
    add(&pool, &msgs[i]);
  }

  // Create the clients
  for (uint32_t i = 0; i < client_count; i++, clients_created++) {
    ClientParams* param = &clients[i];
    param->done = false;
    param->error_count = 0;
    param->msgs_processed = 0;

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

  // Loop though all the clients writing a messages to them
  for (uint32_t i = 0; i < loops; i++) {
    for (uint32_t c = 0; c < clients_created; c++) {
      // Test both flavors of rmv
      Msg_t* msg;
      if ((i & 1) == 0) {
        msg = rmv(&pool);
      } else {
        msg = rmv_non_stalling(&pool);
      }

      if (msg != NULL) {
        ClientParams* client = &clients[c];

        msgs_sent += 1;
        add(&client->cmdFifo, msg);
        sem_post(&client->sem_waiting);
        DPF("multi_thread_msg: sent client=%p msg=%p\n", client, msg);
      } else {
        no_msgs_count += 1;
        DPF("multi_thread_msg: Whoops msg == NULL c=%u msgs_sent=%lu no_msgs_count=%lu\n",
            c, msgs_sent, no_msgs_count);
        sched_yield();
      }
    }
  }

  error = false;

done:
  printf("multi_thread_msg: done, joining %u clients\n", clients_created);
  uint64_t msgs_processed = 0;
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];

    // Signal the client to stop
    client->done = true;
    sem_post(&client->sem_waiting);

    // Wait until the thread completes
    int retv = pthread_join(client->thread, NULL);
    if (retv != 0) {
      printf("multi_thread_msg: joining failed, clients[%u]=%p retv=%d\n",
          i, (void*)client, retv);
    }

    // Cleanup resources
    sem_destroy(&client->sem_ready);
    sem_destroy(&client->sem_waiting);

    // Record if clients discovered any errors
    if (client->error_count != 0) {
      printf("multi_thread_msg: ERROR clients[%u]=%p error_count=%lu\n",
          i, (void*)client, client->error_count);
      error = true;
    }
    printf("multi_thread_msg: clients[%u]=%p msg_count=%lu\n",
        i, (void*)client, client->msgs_processed);
    msgs_processed += client->msgs_processed;
  }

  // Remove all msgs from the pool
  Msg_t* msg;
  uint32_t rmv_count = 0;
  while ((msg = rmv(&pool)) != NULL) {
    rmv_count += 1;
    DPF("multi_thread_msg: remove msg=%p\n", msg);
  }
  if (rmv_count != msg_count) {
    printf("multi_thread_msg: ERROR pool had %u msgs expected %u\n",
        rmv_count, msg_count);
    error = true;
  }

  // Deinit the pool
  msg = deinitMpscFifo(&pool);
  if (msg != NULL) {
    printf("multi_thread_msg: ERROR all messages should be displosed by deinit\n");
    error = true;
  }

  if (msgs != NULL) {
    free(msgs);
  }

  uint64_t expected_value = loops * clients_created;
  uint64_t sum = msgs_sent + no_msgs_count + not_ready_client_count;
  if (sum != expected_value) {
    printf("multi_thread_msg: ERROR sum=%lu != expected_value=%lu\n", sum, expected_value);
    error = true;
  }

  printf("multi_thread_msg: msgs_processed=%lu msgs_sent=%lu "
      "no_msgs_count=%lu not_ready_client_count=%lu\n",
      msgs_processed, msgs_sent, no_msgs_count, not_ready_client_count);

  printf("multi_thread_msg:-error=%u\n\n", error);

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
