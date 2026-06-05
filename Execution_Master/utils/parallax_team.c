/* ==========================================================================
 * parallax_team.c  —  Master-side execution team management
 *
 * Implements the handshake protocol between the master and remote worker
 * nodes (CHCK → optional PROG → TASK), manages thread teams where each
 * thread drives one worker, and provides the team lifecycle API used by
 * the orchestrator.
 *
 * Protocol overview (per worker thread)
 * --------------------------------------
 *  1. CHCK   — ask the worker whether a named program is already loaded.
 *  2. PROG   — (only if CHCK returned "NONE") ship the source code.
 *  3. TASK   — send a data chunk and function name; block for the result.
 *
 * All one-shot reply queues are deleted after their response is consumed.
 * ========================================================================== */

#include "parallax_team.h"
#include "barrier.h"
#include "master_exec.h"
#include "ms_queue.h"
#include "net_utils.h"
#include "network_agent.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Private types used only within this file
 * -------------------------------------------------------------------------- */

/*
 * recv_task_t — wire format for a task message sent to the worker.
 * The flexible array member carries the raw data chunk.
 */
typedef struct {
  char function_name[64];
  uint64_t data_count;
  uint8_t data[];
} recv_task_t;

/*
 * prog_t — wire format for a PROG message containing source code.
 */
typedef struct {
  char prog_name[64];
  char prog_code[7500];
} prog_t;

/* --------------------------------------------------------------------------
 * Worker–master protocol functions
 * -------------------------------------------------------------------------- */

/*
 * check_program_exists
 *
 * Description: Sends a CHCK message to the worker at (ip, port) asking whether
 *              the named program is already compiled and has a live task queue.
 *              Polls a one-shot reply queue until the worker responds, then
 *              deletes the reply queue.
 *
 * Input:  ip           — worker IP address string.
 *         port         — worker network port.
 *         prog_name    — name of the program to check.
 *         task_mq_name — caller-supplied buffer (≥ 64 bytes) that receives
 *                        the task MQ name on success, or "NONE" if missing.
 *
 * Output: 1  if the program exists (task_mq_name is populated).
 *         0  if the program is missing (task_mq_name == "NONE").
 *        -1  on error (msgrcv failure).
 */
int check_program_exists(char *ip, int port, const char *prog_name,
                         char *task_mq_name) {
  size_t total_size = sizeof(message_t) + strlen(prog_name) + 1;
  message_t *msg = malloc(total_size);
  memset(msg, 0, total_size);

  msg->mq_type = 1;

  char iface[64] = {0};
  load_network_interface(iface, sizeof(iface));
  get_local_ip(msg->sender_ip, sizeof(msg->sender_ip), iface);
  msg->sender_port = 9005;
  strcpy(msg->type, "CHCK");

  /* Create a one-shot reply queue and embed its name in the message */
  char *recv_q = create_mq(NULL, 0);
  snprintf(msg->recv_type, sizeof(msg->recv_type), "%s", recv_q);
  msg->size = strlen(prog_name) + 1;
  strcpy(msg->data, prog_name);

  map_entry *entry = find_by_msg_type(recv_q);

  send_msg(ip, port, "master_out", msg);
  printf("[Master] Sent CHCK for '%s' to worker %s:%d\n", prog_name, ip, port);
  free(msg);

  /* Poll the reply queue until the worker responds */
  queued_message resp_msg;
  printf("[Master] Waiting for CHCK response on queue %s...\n", recv_q);
  ssize_t received = -1;
  while (received < 0) {
    received = msgrcv(entry->queue_id, &resp_msg,
                      sizeof(resp_msg) - sizeof(long), 1L, IPC_NOWAIT);
    if (received < 0) {
      if (errno == ENOMSG) {
        usleep(100000); /* 100 ms poll interval */
        continue;
      }
      perror("check_program_exists: msgrcv");
      delete_mq(recv_q);
      return -1;
    }
  }

  strcpy(task_mq_name, resp_msg.data);
  printf("[Master] CHCK response: %s\n", task_mq_name);

  delete_mq(recv_q); /* clean up one-shot reply queue */

  return (strcmp(task_mq_name, "NONE") == 0) ? 0 : 1;
}

/*
 * send_prog_message_and_wait
 *
 * Description: Sends a PROG message containing the program's source code to
 *              the worker.  Blocks until the worker confirms it has compiled
 *              the binary and created a task queue, then deletes the reply
 *              queue.  On success task_mq_name is overwritten with the task
 *              queue name the worker allocated.
 *
 * Input:  ip           — worker IP address string.
 *         port         — worker network port.
 *         task_mq_name — caller-supplied buffer (≥ 64 bytes) that receives
 *                        the worker's new task MQ name.
 *
 * Output: 0 on success, -1 on error.
 */
int send_prog_message_and_wait(char *ip, int port, char *task_mq_name) {
  prog_t prog;
  memset(&prog, 0, sizeof(prog));
  strcpy(prog.prog_name, "test_output");
  strcpy(prog.prog_code, "#include <stdio.h>\n"
                         "#include <stdlib.h>\n"
                         "#include <string.h>\n"
                         "\n"
                         "typedef void *(*fn)(void *);\n"
                         "\n"
                         "void *sum_array(void *arg) {\n"
                         "    char *filename = (char*)arg;\n"
                         "    FILE *f = fopen(filename, \"rb\");\n"
                         "    if (!f) return strdup(\"0\");\n"
                         "    fseek(f, 0, SEEK_END);\n"
                         "    long size = ftell(f);\n"
                         "    fseek(f, 0, SEEK_SET);\n"
                         "    int count = size / sizeof(int);\n"
                         "    int *arr = malloc(size);\n"
                         "    fread(arr, sizeof(int), count, f);\n"
                         "    fclose(f);\n"
                         "    long long sum = 0;\n"
                         "    for(int i = 0; i < count; i++) {\n"
                         "        sum += arr[i];\n"
                         "    }\n"
                         "    free(arr);\n"
                         "    char *result = malloc(64);\n"
                         "    sprintf(result, \"%lld\", sum);\n"
                         "    return result;\n"
                         "}\n"
                         "\n"
                         "fn matcher(char *name) {\n"
                         "    if (strcmp(name, \"sum_array\") == 0) {\n"
                         "        return sum_array;\n"
                         "    }\n"
                         "    return NULL;\n"
                         "}\n"
                         "\n"
                         "int main() { return 0; }\n");

  size_t total_size =
      sizeof(message_t) + sizeof(prog.prog_name) + strlen(prog.prog_code) + 1;
  message_t *msg = malloc(total_size);
  memset(msg, 0, total_size);

  msg->mq_type = 1;
  char iface[64] = {0};
  load_network_interface(iface, sizeof(iface));
  get_local_ip(msg->sender_ip, sizeof(msg->sender_ip), iface);
  msg->sender_port = 9005;
  strcpy(msg->type, "PROG");

  /* One-shot reply queue for the worker's PROG acknowledgement */
  char *recv_q = create_mq(NULL, 0);
  snprintf(msg->recv_type, sizeof(msg->recv_type), "%s", recv_q);
  msg->size = sizeof(prog.prog_name) + strlen(prog.prog_code) + 1;
  memcpy(msg->data, &prog, msg->size);

  send_msg(ip, port, "master_out", msg);
  printf("[Master] Sent PROG message to worker %s:%d\n", ip, port);
  free(msg);

  map_entry *entry = find_by_msg_type(recv_q);
  if (!entry) {
    printf("[Master] Could not find reply queue %s for PROG response\n",
           recv_q);
    return -1;
  }

  /* Poll until the worker sends the compiled task MQ name */
  queued_message resp_msg;
  printf("[Master] Waiting for PROG response on queue %s...\n", recv_q);
  ssize_t received = -1;
  while (received < 0) {
    received = msgrcv(entry->queue_id, &resp_msg,
                      sizeof(resp_msg) - sizeof(long), 1L, IPC_NOWAIT);
    if (received < 0) {
      if (errno == ENOMSG) {
        usleep(100000);
        continue;
      }
      perror("send_prog_message_and_wait: msgrcv");
      delete_mq(recv_q);
      return -1;
    }
  }

  strcpy(task_mq_name, resp_msg.data);
  printf("[Master] PROG response: task_mq=%s\n", task_mq_name);

  delete_mq(recv_q); /* clean up one-shot reply queue */
  return 0;
}

/*
 * send_task_message_and_wait
 *
 * Description: Packages a data chunk and function name into a recv_task_t and
 *              sends it to the worker's task queue (task_mq_name).  Blocks
 *              until the worker sends back the execution result, then deletes
 *              the one-shot reply queue.  The result string is duplicated into
 *              *result_ptr if the caller provides a non-NULL pointer.
 *
 * Input:  ip            — worker IP address string.
 *         port          — worker network port.
 *         task_mq_name  — name of the worker's task queue (from CHCK/PROG).
 *         function_name — name of the function to invoke inside the binary.
 *         chunk         — data chunk to pass to the function.
 *         result_ptr    — optional: receives a strdup'd result string.
 *
 * Output: 0 on success, -1 on error.
 */
int send_task_message_and_wait(char *ip, int port, const char *task_mq_name,
                               const char *function_name, chunk_data *chunk,
                               void **result_ptr) {
  size_t task_size = sizeof(recv_task_t) + chunk->chunk_size;

  recv_task_t *task = malloc(task_size);
  memset(task, 0, task_size);
  strcpy(task->function_name, function_name);
  task->data_count = chunk->chunk_size;
  memcpy(task->data, chunk->chunk, chunk->chunk_size);

  size_t total_size = sizeof(message_t) + task_size;
  message_t *msg = malloc(total_size);
  memset(msg, 0, total_size);

  msg->mq_type = 1;
  char iface[64] = {0};
  load_network_interface(iface, sizeof(iface));
  get_local_ip(msg->sender_ip, sizeof(msg->sender_ip), iface);
  msg->sender_port = 9005;
  strcpy(msg->type, task_mq_name);

  /* One-shot reply queue for the task execution result */
  char *recv_q = create_mq(NULL, 0);
  strcpy(msg->recv_type, recv_q);
  msg->size = task_size;
  memcpy(msg->data, task, task_size);

  send_msg(ip, port, "master_out", msg);
  printf("[Master] Sent task (function=%s) to queue %s\n", function_name,
         task_mq_name);

  free(task);
  free(msg);

  /* Block waiting for the execution result */
  printf("[Master] Waiting for task result on queue %s...\n", recv_q);
  map_entry *entry = find_by_msg_type(recv_q);
  queued_message resp_msg;
  ssize_t received = msgrcv(entry->queue_id, &resp_msg,
                            sizeof(resp_msg) - sizeof(long), 1L, 0);
  if (received < 0) {
    perror("send_task_message_and_wait: msgrcv");
    delete_mq(recv_q);
    return -1;
  }

  message_t *resp = (message_t *)&resp_msg;
  printf("[Master] Task result: %s\n", resp->data);

  if (result_ptr)
    *result_ptr = strdup((char *)resp->data);

  delete_mq(recv_q); /* clean up one-shot reply queue */
  return 0;
}

/* --------------------------------------------------------------------------
 * Worker thread — per-worker execution logic
 * -------------------------------------------------------------------------- */

/*
 * thread_func_test
 *
 * Description: Thread function executed by each worker_t in a team.  Performs
 *              the full CHCK → (optional PROG) → TASK handshake with the
 *              assigned remote worker node, then signals the team barrier so
 *              the master can proceed after all workers have finished.
 *
 * Input:  arg — pointer to a worker_context describing the target node,
 *               data chunk, function name, and result slot.
 *
 * Output: always NULL.
 */
void *thread_func_test(void *arg) {
  worker_context *param = (worker_context *)arg;

  printf("[Team] Thread tid=%d started  function=%s  worker=%s\n", param->tid,
         param->function_name, param->exec_node->ip);

  chunk_data *chunk = param->chunk;
  int *data = (int *)chunk->chunk;
  int count = (int)(chunk->chunk_size / sizeof(int));

  printf("[Team] Chunk data (%d ints): ", count);
  for (int i = 0; i < count; i++)
    printf("%d ", data[i]);
  printf("\n");

  sleep(param->tid * 3); /* staggered start to reduce contention */

  printf("[Team] Thread tid=%d resuming — sending to worker\n", param->tid);

  /* Step 1 — CHCK */
  char task_mq_name[64];
  memset(task_mq_name, 0, sizeof(task_mq_name));

  int exists =
      check_program_exists(param->exec_node->ip, param->exec_node->port,
                           "test_output", task_mq_name);
  if (exists == 0) {
    /* Step 2 — PROG (only when missing) */
    printf("[Team] Program missing on worker, sending source...\n");
    if (send_prog_message_and_wait(param->exec_node->ip, param->exec_node->port,
                                   task_mq_name) < 0) {
      printf("[Team] Failed to send PROG.\n");
    }
  } else if (exists > 0) {
    printf("[Team] Program already loaded on worker, skipping PROG.\n");
  }

  /* Step 3 — TASK */
  printf("[Team] Sending task to queue: %s\n", task_mq_name);
  if (send_task_message_and_wait(param->exec_node->ip, param->exec_node->port,
                                 task_mq_name, param->function_name,
                                 param->chunk, param->result_ptr) < 0) {
    printf("[Team] Failed to send task.\n");
  }

  barrier_wait(param->barrier);
  return NULL;
}

/* --------------------------------------------------------------------------
 * Team lifecycle API
 * -------------------------------------------------------------------------- */

/*
 * team_init
 *
 * Description: Allocates and initialises a team of nb_threads workers with a
 *              shared barrier.  Each worker gets its own worker_context.
 *              Chunks and target nodes must be assigned separately via
 *              create_and_assign_task before calling team_start.
 *
 * Input:  nb_threads — number of workers to create.
 *
 * Output: Pointer to a newly heap-allocated team, never NULL
 *         (exits on allocation failure).
 */
team *team_init(int nb_threads) {
  team *new_team = (team *)malloc(sizeof(team));

  new_team->num_workers = nb_threads;
  new_team->workers = malloc(sizeof(worker_t) * nb_threads);
  new_team->barrier = barrier_init(nb_threads);
  new_team->results = calloc(nb_threads, sizeof(void *));

  for (int i = 0; i < nb_threads; i++) {
    new_team->workers[i].id = i;
    new_team->workers[i].func = thread_func_test;

    worker_context *context = (worker_context *)malloc(sizeof(worker_context));
    context->tid = i;
    context->barrier = new_team->barrier;
    context->chunk = NULL;
    context->result_ptr = &new_team->results[i];

    new_team->workers[i].context = context;
  }

  new_team->reduce_fxn = NULL;
  new_team->tasks = NULL;

  return new_team;
}

/*
 * team_start
 *
 * Description: Spawns one POSIX thread per worker in the team.  All threads
 *              run thread_func_test with their respective worker_context.
 *
 * Input:  team — initialised team (chunks and nodes must already be assigned).
 *
 * Output: 0 always.
 */
int team_start(team *team) {
  for (int i = 0; i < team->num_workers; i++) {
    pthread_create(&team->workers[i].tid, NULL, team->workers[i].func,
                   (void *)team->workers[i].context);
  }
  return 0;
}

/*
 * team_wait
 *
 * Description: Joins all worker threads, blocking until every thread in the
 *              team has returned.
 *
 * Input:  team — a team that has been started with team_start.
 *
 * Output: 0 always.
 */
int team_wait(team *team) {
  for (int i = 0; i < team->num_workers; i++)
    pthread_join(team->workers[i].tid, NULL);
  return 0;
}

/*
 * team_destroy
 *
 * Description: Frees all memory associated with a team.  Must be called after
 *              team_wait returns.  Does not free result strings stored in the
 *              results array — the caller owns those.
 *
 * Input:  t — team to destroy (may be NULL, in which case this is a no-op).
 *
 * Output: none.
 */
void team_destroy(team *t) {
  if (!t)
    return;

  for (int i = 0; i < t->num_workers; i++)
    free(t->workers[i].context);

  free(t->workers);
  free(t->results);
  free(t->barrier);
  free(t);
}

/*
 * team_reduce
 *
 * Description: Applies the team's reduce function left-to-right across all
 *              result slots, accumulating a single combined result.  Frees
 *              intermediate accumulator values as it goes.
 *
 * Input:  t — team whose results array is fully populated (all threads done).
 *
 * Output: Pointer to the final accumulated result, or NULL if the team has no
 *         reduce function, no workers, or the team pointer is NULL.
 */
void *team_reduce(team *t) {
  if (!t || !t->reduce_fxn || t->num_workers == 0)
    return NULL;

  void *acc = t->results[0] ? strdup((char *)t->results[0]) : NULL;
  for (int i = 1; i < t->num_workers; i++) {
    void *next_acc = t->reduce_fxn(acc, t->results[i]);
    if (acc)
      free(acc);
    acc = next_acc;
  }
  return acc;
}

/*
 * create_and_assign_task
 *
 * Description: Convenience function that creates a team and binds each worker
 *              to its target node, data chunk, and function name from the
 *              provided assignments array.
 *
 * Input:  assignments   — array of nb_assignments task_assignment structs,
 *                         each describing one worker's workload.
 *         nb_assignments — number of assignments (determines team size).
 *
 * Output: Pointer to a fully initialised team ready for team_start.
 */
team *create_and_assign_task(task_assignment *assignments, int nb_assignments) {
  team *t = team_init(nb_assignments);

  for (int i = 0; i < nb_assignments; i++) {
    t->workers[i].context->exec_node = assignments[i].target_node;
    t->workers[i].context->chunk = assignments[i].chunk;

    strncpy(t->workers[i].context->function_name,
            assignments[i].task->function_name,
            sizeof(t->workers[i].context->function_name) - 1);
    t->workers[i]
        .context
        ->function_name[sizeof(t->workers[i].context->function_name) - 1] =
        '\0';
  }

  return t;
}