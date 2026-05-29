#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/msg.h>

#include "network_agent.h"
#include "ms_queue.h"

// Define the structures expected
typedef struct {
  char function_name[64];
  uint64_t data_count;
  uint8_t data[400]; // Fixed size for test
} recv_task_t;

typedef struct {
  char prog_name[64];
  char prog_code[7500];
} prog_t;

int main() {
    printf("[MockWorker] Starting network agent on port 9000...\n");
    static network_agent_config cfg = {9000, "worker_out"};
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, network_thread_run, &cfg);
    usleep(500000); 

    // Create queue for NODES requests (Simulating Controller)
    char *nodes_q = create_mq("NODES", 0);
    map_entry *nodes_entry = find_by_msg_type(nodes_q);

    // Create queue for PROG requests
    char *prog_q = create_mq("PROG", 0);
    map_entry *prog_entry = find_by_msg_type(prog_q);

    // Create queue for CHCK requests (Network agent puts it into PROG actually if type is PROG)
    // Wait, check_program_exists sends type="PROG" and recv_type="CHCK_<tid>". 
    // It will land in "PROG" queue.

    printf("[MockWorker] Listening on PROG and NODES queues...\n");

    while (1) {
        queued_message msg;
        // Check NODES
        ssize_t rec = msgrcv(nodes_entry->queue_id, &msg, sizeof(msg) - sizeof(long), NETWORK_AGENT_MTYPE, IPC_NOWAIT);
        if (rec > 0) {
            message_t *message = (message_t *)&msg;
            printf("[MockWorker] Received NODES request. Sending dummy node...\n");
            
            // Send back 1 mock metric
            #include "../../parallax/state_message.h"
            MachineMetrics mock_metrics[1];
            memset(&mock_metrics[0], 0, sizeof(MachineMetrics));
            strcpy(mock_metrics[0].uuid, "mock-worker-1");
            strcpy(mock_metrics[0].ip, "127.0.0.1");
            mock_metrics[0].port = 9000;
            mock_metrics[0].cpu_usage = 5.0;
            mock_metrics[0].mem_usage = 10.0;
            
            message_t *resp = malloc(sizeof(message_t) + sizeof(MachineMetrics));
            memset(resp, 0, sizeof(message_t) + sizeof(MachineMetrics));
            resp->mq_type = 1;
            strcpy(resp->type, message->recv_type); // reply to the queue the sender specified
            resp->size = sizeof(MachineMetrics);
            memcpy(resp->data, mock_metrics, sizeof(MachineMetrics));
            
            send_msg("127.0.0.1", 9005, "worker_out", resp);
            free(resp);
        }

        // Check PROG
        rec = msgrcv(prog_entry->queue_id, &msg, sizeof(msg) - sizeof(long), NETWORK_AGENT_MTYPE, IPC_NOWAIT);
        if (rec > 0) {
            message_t *message = (message_t *)&msg;
            
            // Check if it is a CHCK message
            if (strncmp(message->recv_type, "CHCK", 4) != 0 && strlen(message->recv_type) > 0) {
                // Wait, create_mq(NULL) returns a random string, NOT starting with CHCK!
                // How does the worker know if it is CHCK or PROG?
                // check_program_exists sends size = strlen(prog_name)+1 and data = prog_name.
                // send_prog_message sends size = sizeof(prog_name) + strlen(prog_code) + 1.
                if (message->size < 100) {
                    // It's a CHCK message!
                    printf("[MockWorker] Received CHCK request. Replying NONE.\n");
                    message_t *resp = malloc(sizeof(message_t) + 5);
                    memset(resp, 0, sizeof(message_t) + 5);
                    resp->mq_type = 1;
                    strcpy(resp->type, message->recv_type);
                    resp->size = 5;
                    strcpy(resp->data, "NONE");
                    send_msg("127.0.0.1", 9005, "worker_out", resp);
                    free(resp);
                } else {
                    // It's the actual PROG message
                    printf("[MockWorker] Received PROG upload! Compiling...\n");
                    // Simulate compilation success and return the task queue
                    message_t *resp = malloc(sizeof(message_t) + 64);
                    memset(resp, 0, sizeof(message_t) + 64);
                    resp->mq_type = 1;
                    strcpy(resp->type, message->recv_type);
                    char *task_q = create_mq("TASK_Q", 0);
                    resp->size = strlen(task_q) + 1;
                    strcpy(resp->data, task_q);
                    send_msg("127.0.0.1", 9005, "worker_out", resp);
                    free(resp);

                    // Now wait for the TASK on the new queue!
                    printf("[MockWorker] Waiting for TASK on %s...\n", task_q);
                    map_entry *task_entry = find_by_msg_type(task_q);
                    queued_message task_msg;
                    ssize_t t_rec = msgrcv(task_entry->queue_id, &task_msg, sizeof(task_msg) - sizeof(long), NETWORK_AGENT_MTYPE, 0); // block
                    if (t_rec > 0) {
                        message_t *t_m = (message_t *)&task_msg;
                        printf("[MockWorker] Received TASK! Executing...\n");
                        // Reply with result
                        message_t *result = malloc(sizeof(message_t) + 64);
                        memset(result, 0, sizeof(message_t) + 64);
                        result->mq_type = 1;
                        strcpy(result->type, t_m->recv_type);
                        result->size = 64;
                        strcpy(result->data, "Execution Success: 42");
                        send_msg("127.0.0.1", 9005, "worker_out", result);
                        free(result);
                        printf("[MockWorker] Task complete!\n");
                    }
                }
            }
        }
        usleep(100000);
    }
    return 0;
}
