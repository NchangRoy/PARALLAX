/* ==========================================================================
 * worker_exec.c  —  Execution Worker
 *
 * Runs inside a worker node.  Accepts CHCK and PROG messages from the master
 * over the network agent, manages a registry of compiled programs, and spawns
 * per-program listener threads that execute incoming tasks via fork/exec and
 * pipe the results back to the master.
 *
 * Key design points
 * -----------------
 *  • The inner task execution loop (task_listen_loop) is shared between the
 *    full compile path and the restart-safe revive path.
 *  • On a CHCK request the worker first checks the in-memory registry, then
 *    checks whether the compiled binary already exists on disk.  This means a
 *    worker restart does not force the master to re-send source code.
 *  • One-shot reply queues (CHCK/PROG responses) are owned by the master; the
 *    worker simply addresses replies to the queue name embedded in recv_type.
 * ========================================================================== */

#include "worker_exec.h"
#include "ms_queue.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define MASTER_IP "127.0.0.1"
#define MASTER_PORT 9001
#define MAX_PROGRAMS 100

/* --------------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------------- */

/*
 * program_mapping — maps a program name to its live task message queue name.
 * Entries are added when a PROG message is successfully processed and the
 * execution thread is running.
 */
typedef struct {
  char prog_name[64]; /* logical program name (e.g. "test_output")  */
  char task_mq[64];   /* msg_type of the IPC queue serviced by the   *
                       * per-program execution thread                 */
} program_mapping;

/*
 * listen_context — argument passed to listen_thread_func.
 * Used on the restart-safe path where the binary is already on disk and we
 * only need to create a fresh task queue and start listening.
 */
typedef struct {
  char prog_name[64]; /* binary name under bin/           */
  char task_mq[64];   /* newly created task queue name    */
  int mq_id;          /* resolved System V queue ID       */
} listen_context;

/* --------------------------------------------------------------------------
 * Global program registry
 * -------------------------------------------------------------------------- */

program_mapping loaded_programs[MAX_PROGRAMS];
int loaded_programs_count = 0;
pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --------------------------------------------------------------------------
 * Internal — task execution loop (shared by both code paths)
 * -------------------------------------------------------------------------- */

/*
 * task_listen_loop
 *
 * Description: Blocking loop that waits for recv_task_t messages on a given
 *              System V queue, forks the pre-compiled binary to execute each
 *              task, captures the result over a pipe, and sends it back to
 *              the master via the network agent.
 *
 * Input:  mq_id     — System V queue ID to read tasks from.
 *         prog_name — binary name under ./bin/ to execute for each task.
 *
 * Output: never returns (infinite loop); the thread must be cancelled or the
 *         process killed to stop it.
 */
static void task_listen_loop(int mq_id, const char *prog_name) {
  while (1) {
    queued_message msg;
    ssize_t received = msgrcv(mq_id, &msg, sizeof(msg) - sizeof(long), 1, 0);
    if (received < 0) {
      perror("task_listen_loop: msgrcv");
      continue;
    }

    message_t *message = (message_t *)&msg;
    recv_task_t *task = (recv_task_t *)message->data;

    printf("[Worker] Received task: function=%s\n", task->function_name);

    /* Create a pipe so the child binary can write its result back */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
      perror("task_listen_loop: pipe");
      continue;
    }

    int run_pid = fork();
    if (run_pid == 0) {
      /* --- child: exec the compiled binary --- */
      close(pipefd[0]);

       char binary_path[128];
      snprintf(binary_path, sizeof(binary_path), "./progs/bin/%s", prog_name);

      char fd_str[16];
      snprintf(fd_str, sizeof(fd_str), "%d", pipefd[1]);

      /* Dump raw task data to a temp file so the binary can read it */
      char data_filename[128];
      snprintf(data_filename, sizeof(data_filename), "progs/scratch/data_%d.bin",
               getpid());
      FILE *dfp = fopen(data_filename, "wb");
      if (dfp) {
        size_t data_size = message->size - sizeof(recv_task_t);
        fwrite(task->data, 1, data_size, dfp);
        fclose(dfp);
      }

      char *arg[] = {binary_path, task->function_name, data_filename, fd_str,
                     NULL};
      execvp(binary_path, arg);
      perror("task_listen_loop: execvp");
      exit(1);
    }

    /* --- parent: collect result from pipe then wait for child --- */
    close(pipefd[1]);
    char ret_buf[256];
    memset(ret_buf, 0, sizeof(ret_buf));
    read(pipefd[0], ret_buf, sizeof(ret_buf) - 1);
    close(pipefd[0]);

    int status;
    waitpid(run_pid, &status, 0);

    printf("[Worker] Task result: %s\n", ret_buf);

    /* Send result back to master on the one-shot reply queue */
    message_t *result_msg = malloc(sizeof(message_t) + strlen(ret_buf) + 1);
    memset(result_msg, 0, sizeof(message_t) + strlen(ret_buf) + 1);
    result_msg->mq_type = 1;
    strcpy(result_msg->type, message->recv_type);
    result_msg->size = strlen(ret_buf) + 1;
    strcpy(result_msg->data, ret_buf);

    send_msg(message->sender_ip, message->sender_port, "outgoing", result_msg);
    free(result_msg);
  }
}

/* --------------------------------------------------------------------------
 * Internal — thread entry points
 * -------------------------------------------------------------------------- */

/*
 * listen_thread_func
 *
 * Description: Thread entry point for the restart-safe path.  Called when a
 *              compiled binary already exists on disk and we only need to
 *              attach a fresh task queue to it without recompiling.
 *              Calls task_listen_loop and frees the context on return.
 *
 * Input:  arg — heap-allocated listen_context* (ownership transferred).
 *
 * Output: always NULL.
 */
static void *listen_thread_func(void *arg) {
  listen_context *ctx = (listen_context *)arg;
  printf("[Worker] Reusing existing binary progs/bin/%s on mq %s\n", ctx->prog_name,
         ctx->task_mq);
  task_listen_loop(ctx->mq_id, ctx->prog_name);
  free(ctx);
  return NULL;
}

/*
 * execution_thread_func
 *
 * Description: Thread entry point for the full compile-then-listen path.
 *              Saves the received source code to disk, creates a task MQ,
 *              registers the program, replies to the master with the MQ name,
 *              compiles the source with gcc, then enters task_listen_loop.
 *
 * Input:  arg — heap-allocated execution_context* containing the PROG message
 *               and the master's address (ownership transferred).
 *
 * Output: always NULL.
 */
void *execution_thread_func(void *arg) {
  execution_context *context = (execution_context *)arg;
  message_t *message = (message_t *)context->message;
  prog_t *prog = (prog_t *)message->data;

  mkdir("progs", 0777);
  mkdir("progs/scratch", 0777);
  mkdir("progs/bin", 0777);

  char prog_path[128];
  snprintf(prog_path, sizeof(prog_path), "progs/scratch/%.60s.c", prog->prog_name);

  char bin_path[128];
  snprintf(bin_path, sizeof(bin_path), "progs/bin/%.60s", prog->prog_name);

  /* Save source code to disk */
  FILE *fp = fopen(prog_path, "w");
  if (fp == NULL) {
    perror("execution_thread_func: fopen source");
    exit(1);
  }
  fwrite(prog->prog_code, 1, strlen(prog->prog_code), fp);
  fclose(fp);

  /* Create the task MQ and register it before replying to master */
  char *task_mq = create_mq(NULL, 0);

  pthread_mutex_lock(&mapping_mutex);
  if (loaded_programs_count < MAX_PROGRAMS) {
    strcpy(loaded_programs[loaded_programs_count].prog_name, prog->prog_name);
    strcpy(loaded_programs[loaded_programs_count].task_mq, task_mq);
    loaded_programs_count++;
  }
  pthread_mutex_unlock(&mapping_mutex);

  /* Reply to master with the task MQ name so it can dispatch tasks */
  message_t *resp = malloc(sizeof(message_t) + strlen(task_mq) + 1);
  if (!resp) {
    perror("execution_thread_func: malloc resp");
    exit(1);
  }
  memset(resp, 0, sizeof(message_t) + strlen(task_mq) + 1);
  strcpy(resp->type, message->recv_type);
  resp->size = strlen(task_mq) + 1;
  strcpy(resp->data, task_mq);
  send_msg(context->master_ip, context->port, "outgoing", resp);
  free(resp);

  /* Resolve the queue ID before compilation blocks the thread */
  map_entry *entry = find_by_msg_type(task_mq);
  printf("[Worker] Generated task mq: %s\n", task_mq);
  if (!entry)
    return NULL;
  int mq_id = entry->queue_id;

  /* Compile: gcc <source> logic.c -Wl,-e,worker_entry -o <binary> */
  int pid = fork();
  if (pid == 0) {
    char logic_path[128] = "logic.c";
    if (access(logic_path, F_OK) != 0)
      strcpy(logic_path, "Execution_Worker/logic.c");

    char *args[] = {"gcc", prog_path, logic_path, "-Wl,-e,worker_entry",
                    "-o",  bin_path,  NULL};
    execvp("gcc", args);
    perror("execution_thread_func: gcc execvp");
    exit(1);
  }
  wait(NULL);

  /* Enter the shared task execution loop */
  task_listen_loop(mq_id, prog->prog_name);
  return NULL;
}

/* --------------------------------------------------------------------------
 * Internal — restart-safe binary revival
 * -------------------------------------------------------------------------- */

/*
 * revive_existing_binary
 *
 * Description: Called during CHCK handling when the program is not in the
 *              in-memory registry but its compiled binary exists on disk (e.g.
 *              after a worker restart).  Creates a fresh task MQ, registers
 *              the program, detaches a listen_thread_func thread, and returns
 *              the new MQ name so the master can be replied to immediately
 *              without waiting for a recompile.
 *
 * Input:  prog_name   — name of the compiled binary under bin/.
 *         out_task_mq — caller-supplied buffer (≥ 64 bytes) that receives
 *                       the name of the newly created task queue.
 *
 * Output: 0 on success, -1 on any allocation or thread-creation failure.
 */
static int revive_existing_binary(const char *prog_name, char *out_task_mq) {
  char *task_mq = create_mq(NULL, 0);
  if (!task_mq)
    return -1;

  pthread_mutex_lock(&mapping_mutex);
  if (loaded_programs_count < MAX_PROGRAMS) {
    strcpy(loaded_programs[loaded_programs_count].prog_name, prog_name);
    strcpy(loaded_programs[loaded_programs_count].task_mq, task_mq);
    loaded_programs_count++;
  }
  pthread_mutex_unlock(&mapping_mutex);

  map_entry *entry = find_by_msg_type(task_mq);
  if (!entry)
    return -1;

  listen_context *ctx = malloc(sizeof(listen_context));
  if (!ctx)
    return -1;

  strncpy(ctx->prog_name, prog_name, sizeof(ctx->prog_name) - 1);
  ctx->prog_name[sizeof(ctx->prog_name) - 1] = '\0';
  strncpy(ctx->task_mq, task_mq, sizeof(ctx->task_mq) - 1);
  ctx->task_mq[sizeof(ctx->task_mq) - 1] = '\0';
  ctx->mq_id = entry->queue_id;

  pthread_t t;
  if (pthread_create(&t, NULL, listen_thread_func, ctx) != 0) {
    perror("revive_existing_binary: pthread_create");
    free(ctx);
    return -1;
  }
  pthread_detach(t);

  strncpy(out_task_mq, task_mq, 63);
  out_task_mq[63] = '\0';
  return 0;
}

/* --------------------------------------------------------------------------
 * Public API — main worker thread
 * -------------------------------------------------------------------------- */

/*
 * worker_exec_thread
 *
 * Description: Main loop of the execution worker.  Creates the CHCK and PROG
 *              System V queues, then loops waiting for CHCK messages from the
 *              master.  For each CHCK it performs a three-tier lookup:
 *                1. In-memory registry (fastest path, same session).
 *                2. Binary on disk (restart-safe path, no recompile needed).
 *                3. Genuinely missing — replies "NONE" and blocks for a PROG
 *                   message, then hands off to execution_thread_func.
 *
 * Input:  arg — unused (pass NULL).
 *
 * Output: always NULL.  This function does not return under normal operation.
 */
void *worker_exec_thread(void *arg) {
  (void)arg;

  printf("offsetof(message_t, data)     = %zu\n", offsetof(message_t, data));
  printf("offsetof(queued_message, data) = %zu\n",
         offsetof(queued_message, data));

  /* Create the two well-known queues this worker listens on */
  char *code_mq = create_mq("CHCK", 0);
  char *prog_mq = create_mq("PROG", 0);

  if (!code_mq) {
    perror("worker_exec_thread: create_mq CHCK");
    return NULL;
  }
  printf("[Worker] Created CHCK mq\n");

  map_entry *entry = find_by_msg_type("CHCK");
  if (!entry) {
    fprintf(stderr, "worker_exec_thread: CHCK queue not found\n");
    return NULL;
  }

  if (!prog_mq) {
    perror("worker_exec_thread: create_mq PROG");
    return NULL;
  }
  printf("[Worker] Created PROG mq\n");

  map_entry *prog_entry = find_by_msg_type("PROG");
  if (!prog_entry) {
    fprintf(stderr, "worker_exec_thread: PROG queue not found\n");
    return NULL;
  }

  int mq_id = entry->queue_id;
  queued_message msg;

  while (1) {
    printf("[WorkerExec] Waiting for CHCK message...\n");
    ssize_t received = msgrcv(mq_id, &msg, sizeof(msg) - sizeof(long), 1, 0);

    if (received <= 0) {
      perror("worker_exec_thread: msgrcv CHCK");
      continue;
    }

    message_t *message = (message_t *)&msg;
    printf("[WorkerExec] Received message type: %s\n", message->type);

    if (strcmp("CHCK", message->type) != 0)
      continue;

    char *requested_prog = (char *)message->data;
    printf("[WorkerExec] CHCK for program: %s\n", requested_prog);

    char found_mq[64] = "NONE";

    /* --- Tier 1: in-memory registry --- */
    pthread_mutex_lock(&mapping_mutex);
    for (int i = 0; i < loaded_programs_count; i++) {
      if (strcmp(loaded_programs[i].prog_name, requested_prog) == 0) {
        strcpy(found_mq, loaded_programs[i].task_mq);
        break;
      }
    }
    pthread_mutex_unlock(&mapping_mutex);

    /* --- Tier 2: binary on disk (worker-restart recovery) --- */
    if (strcmp(found_mq, "NONE") == 0) {
      char bin_path[128];
      snprintf(bin_path, sizeof(bin_path), "progs/bin/%s", requested_prog);

      if (access(bin_path, X_OK) == 0) {
        printf("[WorkerExec] Binary on disk, reviving listener for %s\n",
               requested_prog);
        if (revive_existing_binary(requested_prog, found_mq) < 0) {
          printf("[WorkerExec] Revival failed, falling back to PROG\n");
          strcpy(found_mq, "NONE");
        } else {
          printf("[WorkerExec] Revived listener on mq %s\n", found_mq);
        }
      }
    }

    /* --- Reply to master (found_mq is either a real MQ name or "NONE") --- */
    message_t *resp = malloc(sizeof(message_t) + strlen(found_mq) + 1);
    memset(resp, 0, sizeof(message_t) + strlen(found_mq) + 1);
    resp->mq_type = 1;
    strcpy(resp->type, message->recv_type);
    resp->size = strlen(found_mq) + 1;
    strcpy(resp->data, found_mq);
    printf("[WorkerExec] Replying to %s:%d mq=%s task_mq=%s\n",
           message->sender_ip, message->sender_port, resp->type, found_mq);
    send_msg(message->sender_ip, message->sender_port, "outgoing", resp);
    free(resp);

    /* --- Tier 3: program genuinely missing — wait for PROG --- */
    if (strcmp(found_mq, "NONE") == 0) {
      printf("[WorkerExec] Program missing. Waiting for PROG message...\n");
      received = -1;
      while (received < 0) {
        received = msgrcv(prog_entry->queue_id, &msg,
                          sizeof(msg) - sizeof(long), 1, 0);
        if (received < 0) {
          if (errno == ENOMSG) {
            usleep(100000);
            continue;
          }
          perror("worker_exec_thread: msgrcv PROG");
          continue;
        }
      }

      message = (message_t *)&msg;
      printf("[WorkerExec] Received PROG message type: %s\n", message->type);

      if (strcmp("PROG", message->type) == 0) {
        printf("[WorkerExec] Starting compile+listen thread.\n");
        execution_context *context = malloc(sizeof(execution_context));
        context->master_ip = strdup(message->sender_ip);
        context->port = message->sender_port;
        context->message = message;
        pthread_t worker_thread;
        pthread_create(&worker_thread, NULL, execution_thread_func,
                       (void *)context);
        pthread_detach(worker_thread);
      }
    }
  }

  return NULL;
}