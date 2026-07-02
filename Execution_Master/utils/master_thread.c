#include "master_thread.h"
#include "ms_queue.h"
#include "net_utils.h"
#include "network_agent.h"
#include "node_details.h"
#include "orchestrator.h"
#include "parallax_team.h"
#include "pthread.h"
#include "../../parallax/state_message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define PROG_DIR "./progs"

#ifdef STANDALONE
char controller_ip[16] = "127.0.0.1";
#else
extern char controller_ip[16];
#endif

/* Reads a whole file into a malloc'd, NUL-terminated buffer (caller frees).
   Returns NULL on failure. *out_len excludes the NUL terminator. */
static char *read_file_all(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = '\0';
  if (out_len) *out_len = n;
  return buf;
}

/* Ships `content` to the controller as a PROG_LOG, prefixed with a
   __PARALLAX_STATUS__ marker so the Receptionist (send_result_callback in
   Receptionnist/reception.c) can tell the backend whether the program
   succeeded or failed, instead of silently dropping failures like before. */
static void send_prog_log(const char *prog_name, int success,
                           const char *content, size_t content_len) {
  prog_log_t log_msg;
  memset(&log_msg, 0, sizeof(log_msg));
  strncpy(log_msg.prog_name, prog_name, sizeof(log_msg.prog_name) - 1);

  const char *marker = success ? "__PARALLAX_STATUS__=OK\n" : "__PARALLAX_STATUS__=FAILED\n";
  size_t marker_len = strlen(marker);
  size_t cap = sizeof(log_msg.log_content) - 1;

  size_t mlen = marker_len < cap ? marker_len : cap;
  memcpy(log_msg.log_content, marker, mlen);
  size_t remaining = cap - mlen;
  size_t clen = content_len < remaining ? content_len : remaining;
  memcpy(log_msg.log_content + mlen, content, clen);
  log_msg.log_size = (uint32_t)(mlen + clen);

  size_t pkt_size = sizeof(message_t) + sizeof(prog_log_t);
  message_t *pkt = malloc(pkt_size);
  if (pkt) {
    memset(pkt, 0, pkt_size);
    pkt->mq_type = 1;
    strcpy(pkt->type, PROG_LOG_TYPE);
    pkt->size = sizeof(prog_log_t);
    memcpy(pkt->data, &log_msg, sizeof(prog_log_t));
    send_msg(controller_ip, 9000, "outgoing", pkt);
    free(pkt);
    printf("[Master] Execution log sent to controller (status=%s)\n",
           success ? "OK" : "FAILED");
  }
}

void *prog_listener_func(void *args) {
  program_message_t *prog = (program_message_t *)args;
  if (!prog)
    return NULL;

  // Ensure PROG_DIR exists
  mkdir(PROG_DIR, 0777);


  //strncpy(controller_ip,"192.168.50.1", sizeof(controller_ip));

  char ip_filepath[256];
  snprintf(ip_filepath, sizeof(ip_filepath), "%s/controller_ip.c", PROG_DIR);
  FILE *f_ip = fopen(ip_filepath, "w");
  if (f_ip) {
    fprintf(f_ip, "__attribute__((weak)) char controller_ip[16] = \"%s\";\n", controller_ip);
    fclose(f_ip);
    printf("[Master] Generated %s with controller IP: %s\n", ip_filepath, controller_ip);
  } else {
    perror("fopen controller_ip file");
  }

  char filepath[256];
  snprintf(filepath, sizeof(filepath), "%s/%s", PROG_DIR, prog->program_name);

  FILE *f = fopen(filepath, "wb");
  if (f) {
    fwrite(prog->code, 1, prog->code_size, f);
    fclose(f);
    printf("[Master] Saved program to %s\n", filepath);
  } else {
    perror("fopen prog file");
  }

  char compile_cmd[2048];
  char run_cmd[512];

  char prefix[64] = "";
  char network_prefix[64] = "";
  char parallax_prefix[64] = "";
  char root_prefix[64] = "";
  char parser_bin[128] = "";

  /* Detect working directory — root workspace vs Execution_Master subdir */
  if (access("Execution_Master/utils/master_exec.c", F_OK) == 0) {
    strcpy(prefix,          "Execution_Master/utils/");
    strcpy(network_prefix,  "Agent_Init/network/");
    strcpy(parallax_prefix, "parallax/");
    strcpy(root_prefix,     ".");
    strcpy(parser_bin,      "Parser/build/mytool");
  } else {
    strcpy(prefix,          "utils/");
    strcpy(network_prefix,  "../Agent_Init/network/");
    strcpy(parallax_prefix, "../parallax/");
    strcpy(root_prefix,     "..");
    strcpy(parser_bin,      "../Parser/build/mytool");
  }

  /* Run parser: produces <filepath>_parsed.c next to the source */
  char parse_cmd[512];
  char parsed_filepath[280];
  char parse_err_path[300];
  snprintf(parsed_filepath, sizeof(parsed_filepath), "%s_parsed.c", filepath);
  snprintf(parse_err_path, sizeof(parse_err_path), "%s.parse_err", filepath);
  snprintf(parse_cmd, sizeof(parse_cmd),
           "%s %s -- -I%s > %s 2>&1", parser_bin, filepath, root_prefix, parse_err_path);

  printf("[Master] Parsing: %s\n", parse_cmd);
  int parse_ret = system(parse_cmd);
  if (parse_ret != 0 || access(parsed_filepath, F_OK) != 0) {
    printf("[Master] Parsing failed (exit=%d) — aborting.\n", parse_ret);
    size_t elen = 0;
    char *err = read_file_all(parse_err_path, &elen);
    const char *fallback = "Parsing failed (no output captured).";
    send_prog_log(prog->program_name, 0, err ? err : fallback, err ? elen : strlen(fallback));
    free(err);
    free(prog);
    return NULL;
  }
  printf("[Master] Parsed output: %s\n", parsed_filepath);

  char compile_err_path[300];
  snprintf(compile_err_path, sizeof(compile_err_path), "%s/%s.compile_err",
           PROG_DIR, prog->program_name);

  snprintf(compile_cmd, sizeof(compile_cmd),
           "gcc %s %s "
           "%smaster_exec.c "
           "%sorchestrator.c "
           "%sparallax_team.c "
           "%sbarrier.c "
           "%snet_utils.c "
           "%snetwork_agent.c "
           "%sms_queue.c "
           "%ssocket.c "
           "%slinked_list.c "
           "-I%s "
           "-I%s "
           "-I%s "
           "-I%s "
           "-pthread "
           "-o %s/bin_%s > %s 2>&1",
           parsed_filepath, ip_filepath,
           prefix, prefix, prefix, prefix, prefix,
           network_prefix, network_prefix, network_prefix, network_prefix,
           prefix, network_prefix, parallax_prefix, root_prefix,
           PROG_DIR, prog->program_name, compile_err_path);

  printf("[Master] Compiling program: %s\n", compile_cmd);
  if (system(compile_cmd) == 0) {
    printf("[Master] Program compiled successfully.\n");

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", PROG_DIR, prog->program_name);

    snprintf(run_cmd, sizeof(run_cmd), "%s/bin_%s > %s 2>&1",
             PROG_DIR, prog->program_name, log_path);
    printf("[Master] Executing program, output -> %s\n", log_path);
    system(run_cmd);
    printf("[Master] Program finished. Log at: %s\n", log_path);

    size_t rlen = 0;
    char *run_output = read_file_all(log_path, &rlen);
    send_prog_log(prog->program_name, 1, run_output ? run_output : "", rlen);
    free(run_output);
  } else {
    printf("[Master] Program compilation failed.\n");
    size_t elen = 0;
    char *err = read_file_all(compile_err_path, &elen);
    const char *fallback = "Compilation failed (no output captured).";
    send_prog_log(prog->program_name, 0, err ? err : fallback, err ? elen : strlen(fallback));
    free(err);
  }
  free(prog);
  return NULL;
}

static volatile int master_running = 0;

void *master_thread_start(void *args) {
  (void)args;
  master_running = 1;

  // first create a mq for recieving programs
  char *progs = create_mq("PROG", 0);

  // create thread to listen on the mq
  printf("Waiting for programs on MQ: %s\n", progs);
  map_entry *prog_mq = find_by_msg_type(progs);
  if (!prog_mq)
    return NULL;

  queued_message msgp;

  while (master_running) {
    // read something from mq using IPC_NOWAIT
    ssize_t size = msgrcv(prog_mq->queue_id, &msgp, sizeof(msgp) - sizeof(long),
                          1L, IPC_NOWAIT);
    if (size < 0) {
      usleep(100000); // 100ms
      continue;
    }

    printf("Received Program \n");

    program_message_t *program =
        (program_message_t *)malloc(sizeof(program_message_t));
    if (program) {
      memcpy(program, msgp.data, sizeof(program_message_t));
      pthread_t handler_thread;
      pthread_create(&handler_thread, NULL, prog_listener_func,
                     (void *)program);
      pthread_detach(handler_thread);
    }
  }
  return NULL;
}

void master_thread_stop() { master_running = 0; }

#ifdef STANDALONE
int main(){
    printf("[Master] Starting network agent on port 9005...\n");
    static network_agent_config cfg = {9005, "master_out"};
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, network_thread_run, &cfg);
    usleep(500000);
    
    printf("[Master] Starting prog queue listener...\n");
    master_thread_start(NULL);
    
    return 0;
}
#endif
