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

/**
 * We pass pointers in Msg_t.arg2 which is a uint64_t,
 * verify a void* fits.
 * TODO: sizeof(uint64_t) should be sizeof(Msg_t.arg2), how to do that?
 */
_Static_assert(sizeof(uint64_t) >= sizeof(void*), "Expect sizeof uint64_t >= sizeof void*");

typedef _Atomic(uint64_t) Counter;
//static typedef uint64_t Counter;

typedef struct MsgPool_t {
  Msg_t* msgs;
  uint32_t msg_count;
  MpscFifo_t fifo;
} MsgPool_t;

bool MsgPool_init(MsgPool_t* pool, uint32_t msg_count) {
  bool error;
  Msg_t* msgs;

  // Allocate messages
  msgs = malloc(sizeof(Msg_t) * (msg_count + 1));
  if (msgs == NULL) {
    printf("MsgPool_init: pool=%p ERROR unable to allocate messages, aborting msg_count=%u\n",
        pool, msg_count);
    error = true;
    goto done;
  }

  // Output info on the pool and messages
  DPF("MsgPool_init: pool=%p &msgs[0]=%p &msgs[1]=%p sizeof(Msg_t)=%lu(0x%lx)\n",
      pool, &msgs[0], &msgs[1], sizeof(Msg_t), sizeof(Msg_t));

  // Create pool first message will be stub
  for (uint32_t i = 0; i <= msg_count; i++) {
    Msg_t* msg = &msgs[i];
    DPF("MsgPool_init: add %u msg=%p%s\n", i, msg, i == 0 ? " stub" : "");
    msg->pPool = &pool->fifo;
    if (i == 0) {
      // Use first msg to init pool
      initMpscFifo(&pool->fifo, msg);
    } else {
      // Add remaining msgw to pool
      add(&pool->fifo, msg);
    }
  }

  DPF("MsgPool_init: pool=%p, pHead=%p, pTail=%p sizeof(*pool)=%lu(0x%lx)\n",
      pool, pool->fifo.pHead, pool->fifo.pTail, sizeof(*pool), sizeof(*pool));

  error = false;
done:
  if (error) {
    free(msgs);
    pool->msgs = NULL;
    pool->msg_count = 0;
  } else {
    pool->msgs = msgs;
    pool->msg_count = msg_count;
  }

  return error;
}

void MsgPool_deinit(MsgPool_t* pool) {
  DPF("MsgPool_deinit:+pool=%p msgs=%p\n", pool, pool->msgs);
  if (pool->msgs != NULL) {
    // Empty the pool
    for (uint32_t i = 0; i < pool->msg_count; i++) {
      Msg_t *msg;

      // Wait until this is returned
      // TODO: Bug it may never be returned!
      bool once = false;
      while ((msg = rmv(&pool->fifo)) == NULL) {
        if (!once) {
          once = true;
          printf("MsgPool_deinit: waiting for %u\n", i);
        }
        sched_yield();
      }

      DPF("MsgPool_deinit: removed %u msg=%p\n", i, msg);
    }

    DPF("MsgPool_deinit: pool=%p deinitMpscFifo fifo=%p\n", pool, &pool->fifo);
    deinitMpscFifo(&pool->fifo);

    DPF("MsgPool_deinit: pool=%p free msgs=%p\n", pool, pool->msgs);
    free(pool->msgs);
    pool->msgs = NULL;
    pool->msg_count = 0;
  }
  DPF("MsgPool_deinit:-pool=%p\n", pool);
}

Msg_t* MsgPool_get_msg(MsgPool_t* pool) {
  Msg_t* msg = rmv(&pool->fifo);
  if (msg != NULL) {
    msg->pRspQ = NULL;
    msg->arg1 = 0;
    msg->arg2 = 0;
  }
  return msg;
}

typedef struct ClientParams {
  MpscFifo_t cmdFifo;

  pthread_t thread;
  uint32_t msg_count;
  uint32_t max_peer_count;

  Msg_t stub;
  MsgPool_t pool;

  uint64_t error_count;
  uint64_t msgs_processed;
  uint64_t msgs_sent;
  sem_t sem_ready;
  sem_t sem_waiting;
} ClientParams;

#define CmdUnknown       0 // arg2 == the command that's unknown
#define CmdDoNothing     1
#define CmdDidNothing    2
#define CmdConnect       3 // arg2 == MpscFifo_t* to connect with
#define CmdConnected     4 // arg2 == MpscFifo_t* connected to
#define CmdDisconnectAll 5
#define CmdDisconnected  6
#define CmdStop          7
#define CmdStopped       8

static void* client(void* p) {
  DPF("client:+param=%p\n", p);
  Msg_t* msg;
  uint32_t peer_idx = 0;
  uint32_t peer_send_idx = 0;

  ClientParams* cp = (ClientParams*)p;

  ClientParams** peers;
  if (cp->max_peer_count > 0) {
    DPF("client: param=%p allocate peers max_peer_count=%u\n",
        p, cp->max_peer_count);
    peers = malloc(sizeof(ClientParams*) * cp->max_peer_count);
    if (peers == NULL) {
      printf("client: param=%p ERROR unable to allocate peers max_peer_count=%u\n",
          p, cp->max_peer_count);
      cp->error_count += 1;
    }
  } else {
    DPF("client: param=%p No peers max_peer_count=%d\n", p, cp->max_peer_count);
    peers = NULL;
  }

  // Init local msg pool
  DPF("client: init msg pool=%p\n", &cp->pool);
  bool error = MsgPool_init(&cp->pool, cp->msg_count);
  if (error) {
    printf("client: param=%p ERROR unable to create msgs for pool\n", p);
    cp->error_count += 1;
  }

  // Init cmdFifo
  cp->stub.pPool = NULL;
  initMpscFifo(&cp->cmdFifo, &cp->stub);
  DPF("client: param=%p cp->cmdFifo=%p stub=%p\n", p, &cp->cmdFifo, &cp->stub);


  // Signal we're ready
  sem_post(&cp->sem_ready);

  // While we're not done wait for a signal to do work
  // do the work and signal work is complete.
  while (true) {
    DPF("client: param=%p waiting\n", p);
    sem_wait(&cp->sem_waiting);

    DPF("client: param=%p rmv msg\n", p);
    while((msg = rmv(&cp->cmdFifo)) != NULL) {
      cp->msgs_processed += 1;
      if (msg != NULL) {
        DPF("client: param=%p msg=%p arg1=%lu\n", p, msg, msg->arg1);
        switch (msg->arg1) {
          case CmdDoNothing: {
            DPF("client: param=%p msg=%p CmdDoNothing msgs_processed=%lu\n",
                p, msg, cp->msgs_processed);
            send_rsp_or_ret(msg, CmdDidNothing);
            break;
          }
          case CmdStop: {
            DPF("client: param=%p msg=%p CmdStop msgs_processed=%lu\n",
                p, msg, cp->msgs_processed);
            send_rsp_or_ret(msg, CmdStopped);
            goto done;
          }
          case CmdConnect: {
            if (peers != NULL) {
              if (peer_idx < cp->max_peer_count) {
                peers[peer_idx] = (ClientParams*)msg->arg2;
                DPF("client: param=%p msg=%p CmdConnect peer_idx=%u max_peer_count=%u peer=%p\n",
                    p, msg, peer_idx, cp->max_peer_count, peers[peer_idx]);
                peer_idx += 1;
              } else {
                printf("client: param=%p ERROR msg->arg2=%lx to many peers peer_idx=%u >= cp->max_peer_count=%u\n",
                    p, msg->arg2, peer_idx, cp->max_peer_count);
                cp->error_count += 1;
              }
            }
            send_rsp_or_ret(msg, CmdConnected);
            break;
          }
          case CmdDisconnectAll: {
            if (peers != NULL) {
              peer_idx = 0;
              DPF("client: param=%p msg=%p CmdDisconnect peer_idx=%u max_peer_count=%u\n",
                  p, msg, peer_idx, cp->max_peer_count);
            }
            send_rsp_or_ret(msg, CmdDisconnected);
            break;
          }
          default: {
            DPF("client: param=%p ERROR msg=%p Uknown arg1=%lu\n",
                p, msg, msg->arg1);
            cp->error_count += 1;
            msg->arg2 = msg->arg1;
            send_rsp_or_ret(msg, CmdUnknown);
            ret(msg);
          }
        }
      } else {
        cp->error_count += 1;
        DPF("client: param=%p ERROR msg=NULL msgs_processed=%lu\n",
            p, cp->msgs_processed);
      }

      /**
       * Send messages to all of the peers
       */
      for (uint32_t i = 0; i < peer_idx; i++) {
        msg = MsgPool_get_msg(&cp->pool);
        if (msg != NULL) {
          ClientParams* peer = peers[peer_send_idx];
          msg->arg1 = CmdDoNothing;
          DPF("multi_thread_msg: send client=%p msg=%p msg->arg1=%lu CmdDoNothing\n",
             client, msg, msg->arg1);
          add(&peer->cmdFifo, msg);
          sem_post(&peer->sem_waiting);
          cp->msgs_sent += 1;
          DPF("client: param=%p sent msg to %u peer=%p msg=%p\n",
              p, peer_send_idx, peer, msg);
          peer_send_idx += 1;
          if (peer_send_idx >= peer_idx) {
            peer_send_idx = 0;
          }
        } else {
          DPF("client: param=%p whoops done just ret msg=%p\n", p, msg);
          ret(msg);
        }
      }
    }
  }

done:
  // Flush any messages in the cmdFifo
  DPF("client: param=%p done, flushing fifo\n", p);
  uint32_t unprocessed = 0;
  while ((msg = rmv(&cp->cmdFifo)) != NULL) {
    printf("client: param=%p ret msg=%p\n", p, msg);
    unprocessed += 1;
    ret(msg);
  }

  // deinit cmd fifo
  DPF("client: param=%p deinit cmdFifo=%p\n", p, &cp->cmdFifo);
  deinitMpscFifo(&cp->cmdFifo);

  // deinit msg pool
  DPF("client: param=%p deinit msg pool=%p\n", p, &cp->pool);
  MsgPool_deinit(&cp->pool);

  DPF("client:-param=%p error_count=%lu\n", p, cp->error_count);
  return NULL;
}

// Return 0 if successful !0 if an error
uint32_t wait_for_rsp(MpscFifo_t* fifo, uint64_t rsp_expected, void* client, uint32_t client_idx) {
  uint32_t retv;
  Msg_t* msg;

  // TODO: Add MpscFifo_t.sem_waiting??
  while ((msg = rmv(fifo)) == NULL) {
    sched_yield();
  }
  if (msg->arg1 != rsp_expected) {
    DPF("multi_thread_msg: ERROR unexpected arg1=%lu expected %lu arg2=%lu, client[%u]=%p\n",
        msg->arg1, rsp_expected, msg->arg2, client_idx, client);
    retv = 1;
  } else {
    retv = 0;
  }
  ret(msg);
  return retv;
}

bool multi_thread_main(const uint32_t client_count, const uint64_t loops,
    const uint32_t msg_count) {
  bool error;
  Msg_t stub;
  MpscFifo_t cmdFifo;
  ClientParams* clients;
  MsgPool_t pool;
  uint32_t clients_created = 0;
  uint64_t msgs_sent = 0;
  uint64_t no_msgs_count = 0;


  printf("multi_thread_msg:+client_count=%u loops=%lu msg_count=%u\n",
      client_count, loops, msg_count);

  stub.pPool = NULL;
  initMpscFifo(&cmdFifo, &stub);
  DPF("multi_thread_msg: cmdFifo=%p stub=%p\n", &cmdFifo, &stub);

  if (client_count == 0) {
    printf("multi_thread_msg: ERROR client_count=%d, aborting\n", client_count);
    error = true;
    goto done;
  }

  clients = malloc(sizeof(ClientParams) * client_count);
  if (clients == NULL) {
    printf("multi_thread_msg: ERROR Unable to allocate clients array, aborting\n");
    error = true;
    goto done;
  }

  DPF("multi_thread_msg: init msg pool=%p\n", &pool);
  error = MsgPool_init(&pool, msg_count);
  if (error) {
    printf("multi_thread_msg: ERROR Unable to allocate messages, aborting\n");
    goto done;
  }

  // Create the clients
  for (uint32_t i = 0; i < client_count; i++, clients_created++) {
    ClientParams* param = &clients[i];
    param->error_count = 0;
    param->msgs_processed = 0;
    param->msgs_sent = 0;
    param->msg_count = msg_count;
    param->max_peer_count = client_count;

    sem_init(&param->sem_ready, 0, 0);
    sem_init(&param->sem_waiting, 0, 0);

    int retv = pthread_create(&param->thread, NULL, client, (void*)&clients[i]);
    if (retv != 0) {
      printf("multi_thread_msg: ERROR thread creation , clients[%u]=%p retv=%d\n",
          i, param, retv);
      error = true;
      goto done;
    }

    // Wait until it starts
    sem_wait(&param->sem_ready);
  }
  printf("multi_thread_msg: created %u clients\n", clients_created);


  // Connect every client to every other client except themselves
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];
    for (uint32_t peer_idx = 0; peer_idx < clients_created; peer_idx++) {
      ClientParams* peer = &clients[peer_idx];
      if (peer_idx != i) {
        Msg_t* msg = MsgPool_get_msg(&pool);
        if (msg != NULL) {
          msg->pRspQ = &cmdFifo;
          msg->arg1 = CmdConnect;
          msg->arg2 = (uint64_t)peer;
          DPF("multi_thread_msg: send client=%p msg=%p arg1=%lu CmdConnect\n", client, msg, msg->arg1);
          add(&client->cmdFifo, msg);
          sem_post(&client->sem_waiting);
        }
        if (wait_for_rsp(&cmdFifo, CmdConnected, client, i)) {
          error = true;
          goto done;
        }
      }
    }
  }

  // Loop though all the clients writing messages to them
  for (uint32_t i = 0; i < loops; i++) {
    for (uint32_t c = 0; c < clients_created; c++) {
      // Test both flavors of rmv
      Msg_t* msg;
      if ((i & 1) == 0) {
        msg = rmv(&pool.fifo);
      } else {
        msg = rmv_non_stalling(&pool.fifo);
      }

      if (msg != NULL) {
        ClientParams* client = &clients[c];
        msg->arg1 = CmdDoNothing;
        DPF("multi_thread_msg: send client=%p msg=%p arg1=%lu CmdDoNothing\n", client, msg, msg->arg1);
        add(&client->cmdFifo, msg);
        sem_post(&client->sem_waiting);
        msgs_sent += 1;
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
  printf("multi_thread_msg: done, send CmdDisconnectAll %u clients\n", clients_created);
  uint64_t msgs_processed = 0;
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];

    // Request the client to stop
    Msg_t* msg = MsgPool_get_msg(&pool);
    bool once = false;
    while (msg == NULL) {
      if (!once) {
        once = true;
        DPF("multi_thread_msg: waiting for msg to send %u CmdDisconnectAll\n", i);
      }
      sched_yield();
      msg = MsgPool_get_msg(&pool);
    }
    msg->pRspQ = &cmdFifo;
    msg->arg1 = CmdDisconnectAll;
    DPF("multi_thread_msg: send client=%p msg=%p msg->arg1=%lu ddCmdDisconnectAll\n",
       client, msg, msg->arg1);
    add(&client->cmdFifo, msg);
    sem_post(&client->sem_waiting);
    if (wait_for_rsp(&cmdFifo, CmdDisconnected, client, i)) {
      error = true;
      goto done;
    }
  }

  printf("multi_thread_msg: done, send CmdStop %u clients\n", clients_created);
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];

    // Request the client to stop
    Msg_t* msg = MsgPool_get_msg(&pool);
    bool once = false;
    while (msg == NULL) {
      if (!once) {
        once = true;
        DPF("multi_thread_msg: waiting for msg to send %u CmdStop\n", i);
      }
      sched_yield();
      msg = MsgPool_get_msg(&pool);
    }
    DPF("multi_thread_msg: send %u CmdStop\n", i);
    msg->pRspQ = &cmdFifo;
    msg->arg1 = CmdStop;
    DPF("multi_thread_msg: send client=%p msg=%p msg->arg1=%lu CmdStop\n",
       client, msg, msg->arg1);
    add(&client->cmdFifo, msg);
    sem_post(&client->sem_waiting);
    if (wait_for_rsp(&cmdFifo, CmdStopped, client, i)) {
      error = true;
      goto done;
    }
  }

  printf("multi_thread_msg: done, joining %u clients\n", clients_created);
  for (uint32_t i = 0; i < clients_created; i++) {
    ClientParams* client = &clients[i];
    // Wait until the thread completes
    int retv = pthread_join(client->thread, NULL);
    if (retv != 0) {
      printf("multi_thread_msg: ERROR joining failed, clients[%u]=%p retv=%d\n",
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
    DPF("multi_thread_msg: clients[%u]=%p msgs_processed=%lu, msgs_sent=%lu\n",
        i, (void*)client, client->msgs_processed, client->msgs_sent);
    msgs_processed += client->msgs_processed;
  }

  // Deinit the cmdFifo
  DPF("multi_thread_msg: deinit cmdFifo=%p\n", &cmdFifo);
  deinitMpscFifo(&cmdFifo);

  // Deinit the msg pool
  DPF("multi_thread_msg: deinit msg pool=%p\n", &pool);
  MsgPool_deinit(&pool);

  uint64_t expected_value = loops * clients_created;
  uint64_t sum = msgs_sent + no_msgs_count;
  if (sum != expected_value) {
    printf("multi_thread_msg: ERROR sum=%lu != expected_value=%lu\n", sum, expected_value);
    error = true;
  }

  printf("multi_thread_msg: msgs_processed=%lu msgs_sent=%lu "
      "no_msgs_count=%lu\n", msgs_processed, msgs_sent, no_msgs_count);

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

  printf("test client_count=%u loops=%lu msg_count=%u\n", client_count, loops, msg_count);

  error |= multi_thread_main(client_count, loops, msg_count);

  if (!error) {
    printf("Success\n");
  }

  return error ? 1 : 0;
}
