#include "master_thread.h"
#include "ms_queue.h"
#include "network_agent.h"
#include "node_details.h"
#include "orchestrator.h"
#include "parallax_team.h"
#include "pthread.h"
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

  // parse code

  // run code

  char prefix[64] = "";
  char network_prefix[64] = "";
  char parallax_prefix[64] = "";

  // Check if we are running from root workspace or Execution_Master
  if (access("Execution_Master/utils/master_exec.c", F_OK) == 0) {
    // We are at root workspace
    strcpy(prefix, "Execution_Master/utils/");
    strcpy(network_prefix, "Agent_Init/network/");
    strcpy(parallax_prefix, "parallax/");
  } else {
    // We are at Execution_Master subdirectory
    strcpy(prefix, "utils/");
    strcpy(network_prefix, "../Agent_Init/network/");
    strcpy(parallax_prefix, "../parallax/");
  }

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
           "-pthread "
           "-o %s/bin_%s",
           filepath, ip_filepath, prefix, prefix, prefix, prefix, prefix, network_prefix,
           network_prefix, network_prefix, network_prefix, prefix,
           network_prefix, parallax_prefix, PROG_DIR, prog->program_name);

  printf("[Master] Compiling program: %s\n", compile_cmd);
  if (system(compile_cmd) == 0) {
    printf("[Master] Program compiled successfully.\n");
    snprintf(run_cmd, sizeof(run_cmd), "%s/bin_%s", PROG_DIR,
             prog->program_name);
    printf("[Master] Executing program: %s\n", run_cmd);
    system(run_cmd);
  } else {
    printf("[Master] Program compilation failed.\n");
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
