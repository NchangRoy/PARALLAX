#include "../../parallax/parallax_param.h"
#include "ms_queue.h"
#include "net_utils.h"
#include "network_agent.h"
#include "orchestrator.h"
#include "parallax_team.h"
#include "state_message.h"
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

__attribute__((weak)) char controller_ip[16] = "192.168.1.199";

void *sum_reduce(void *a, void *b) {
  if (!a && !b)
    return NULL;
  long long val_a = a ? atoll((char *)a) : 0;
  long long val_b = b ? atoll((char *)b) : 0;
  char *res = malloc(64);
  sprintf(res, "%lld", val_a + val_b);
  return res;
}

__attribute__((weak)) void *matcher(char *name) {
  (void)name;
  return NULL;
}

void execute_fxn(ParallaxParam *params, int param_count, char *fxn_name,
                 ParallaxExecutionCtx *ctx, const char *prog_code, const char *prog_name) {

  int node_count = ctx ? ctx->expected_node_count : 0;

  // Dynamically create a unique queue to receive the NODES response
  char *temp_mq = create_mq(NULL, 0);
  char nodes_mq_name[64];
  if (temp_mq) {
    strcpy(nodes_mq_name, temp_mq);
  } else {
    strcpy(nodes_mq_name, "NODES_TEST");
  }

  // first get worker xtics from controller
  //  Allocate message with room for data payload
  message_t *message = malloc(sizeof(message_t));
  memset(message, 0, sizeof(message_t));
  message->mq_type = 1;
  strcpy(message->type, "NODES");
  strcpy(message->recv_type, nodes_mq_name);

  // Resolve our actual LAN IP so the controller can reply across the network
  char iface[64] = {0};
  load_network_interface(iface, sizeof(iface));
  get_local_ip(message->sender_ip, sizeof(message->sender_ip), iface);
  message->sender_port =
      9000; // test agent listens on 9005 — controller replies here
  message->size = 0;

  printf("[MasterExec] Sending NODES query with reply address %s:%d (queue: %s)\n",
         message->sender_ip, message->sender_port, nodes_mq_name);

  send_msg(controller_ip, 9000, "outgoing", message);
  printf("[MasterExec] NODES query sent to %s:9000\n", controller_ip);
  free(message);

  // read nodes data that was sent to the NODES mq
  map_entry *node_mq = find_by_msg_type(nodes_mq_name);
  queued_message received_msg;

  while (1) {
    ssize_t size = msgrcv(node_mq->queue_id, &received_msg,
                          sizeof(queued_message) - sizeof(long), 1L, 0);
    if (size < 0) {
      continue;
    }
    break;
  }

  // Delete the dynamic queue to avoid IPC queue leaks
  delete_mq(nodes_mq_name);

  MachineMetrics *metrics = (MachineMetrics *)received_msg.data;

  // Count how many valid nodes were actually returned
  int actual_node_count = 0;
  while (strlen(metrics[actual_node_count].uuid) > 0) {
    actual_node_count++;
  }

  if (actual_node_count == 0) {
    printf("[MasterExec] Error: 0 nodes connected to Controller! Aborting "
           "task.\n");
    return;
  }
  printf("[MasterExec] %d worker(s) available\n", actual_node_count);

  /* Sort available nodes by score descending (best first) */
  for (int i = 1; i < actual_node_count; i++) {
    MachineMetrics tmp = metrics[i];
    int j = i - 1;
    while (j >= 0 && metrics[j].score < tmp.score) {
      metrics[j + 1] = metrics[j];
      j--;
    }
    metrics[j + 1] = tmp;
  }

  /* Resolve final node count */
  if (node_count == 0) {
    /* Auto mode: cap at PARALLAX_DEFAULT_NODE_CAP */
    node_count = (actual_node_count < PARALLAX_DEFAULT_NODE_CAP)
               ? actual_node_count : PARALLAX_DEFAULT_NODE_CAP;
    printf("[MasterExec] Auto node selection: using %d node(s) (cap=%d, available=%d)\n",
           node_count, PARALLAX_DEFAULT_NODE_CAP, actual_node_count);
  } else if (node_count > actual_node_count) {
    printf("[MasterExec] WARNING: requested %d node(s) but only %d available — proceeding with %d\n",
           node_count, actual_node_count, actual_node_count);
    node_count = actual_node_count;
  } else {
    printf("[MasterExec] Using top %d of %d available node(s) (by score)\n",
           node_count, actual_node_count);
  }

  /* ── create task assignments using the structured params ── */
  task_assignment *assignments =
      create_assignments(params, param_count, fxn_name, metrics, node_count);

  // display assignments
  for (int i = 0; i < node_count; i++) {
    chunk_data *chunk = (chunk_data *)assignments[i].chunk;
    int p_count = 0;
    if (chunk && chunk->chunk && chunk->chunk_size >= (int)sizeof(int)) {
      memcpy(&p_count, chunk->chunk, sizeof(int));
    }

    printf("\n=================================\n");
    printf("NODE %d ASSIGNMENT DETAILS\n", i);
    printf("=================================\n");
    printf("uuid          : %s\n", metrics[i].uuid);
    printf("ip            : %s\n", metrics[i].ip);
    printf("cpu usage     : %.2f\n", metrics[i].cpu_usage);
    printf("ram usage     : %.2f\n", metrics[i].mem_usage);
    printf("function      : %s\n", assignments[i].task->function_name);
    printf("nb chunk bytes: %d\n", chunk ? chunk->chunk_size : 0);
    printf("param count   : %d\n", p_count);
  }

  team *t = create_and_assign_task(assignments, node_count);

  prog_t *prog = (prog_t *)malloc(sizeof(prog_t));
  if (prog) {
    memset(prog, 0, sizeof(prog_t));
    strncpy(prog->prog_code, prog_code, sizeof(prog->prog_code) - 1);
    strncpy(prog->prog_name, prog_name, sizeof(prog->prog_name) - 1);
    team_attach_prog(prog, t);
  }

  team_start(t);

  team_wait(t);

  // Resolve reduce function dynamically
  void *(*reduce_fn)(void *, void *) = NULL;
  if (ctx && strlen(ctx->aggregator_name) > 0) {
    reduce_fn = (void *(*)(void *, void *))matcher(ctx->aggregator_name);
  }
  if (!reduce_fn) {
    reduce_fn = sum_reduce;
  }
  t->reduce_fxn = reduce_fn;

  void *final_result = team_reduce(t);

  // Aggregate results from each thread
  printf("\n=================================\n");
  printf("AGGREGATED EXECUTION RESULTS\n");
  printf("=================================\n");

  printf("---------------------------------\n");
  printf("FINAL REDUCED RESULT: %s\n",
         final_result ? (char *)final_result : "(null)");
  printf("=================================\n\n");

  for (int i = 0; i < t->num_workers; i++) {
    if (t->results[i]) {
      printf("Node %d partial result: %s\n", i, (char *)t->results[i]);
      free(t->results[i]); // Free the duplicated string
      t->results[i] = NULL;
    } else {
      printf("Node %d partial result: (null)\n", i);
    }
  }

  if (final_result)
    free(final_result);

  // Free local assignments data
  for (int i = 0; i < node_count; i++) {
    if (assignments[i].target_node)
      free(assignments[i].target_node);
    if (assignments[i].task)
      free(assignments[i].task);
    if (assignments[i].chunk) {
      if (assignments[i].chunk->chunk)
        free(assignments[i].chunk->chunk);
      free(assignments[i].chunk);
    }
  }
  free(assignments);

  team_destroy(t);
}
