#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/msg.h>

#include "network_agent.h"
#include "ms_queue.h"
#include "state_message.h"

void init_test_agent() {
    static network_agent_config cfg = {9005, "master_out"};
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, network_thread_run, &cfg);
    
    // Give thread a moment to start
    usleep(500000); 
    printf("[SimulateMaster] Network agent started on port %d.\n", cfg.port);
}

int main() {
    init_test_agent();

    char *target_ip = "127.0.0.1";
    int target_port = 9000;

    // 1. Send STATECAPTURE to register as ROLE_MASTER (3)
    size_t metrics_size = sizeof(message_t) + sizeof(MachineMetrics);
    message_t *msg = malloc(metrics_size);
    memset(msg, 0, metrics_size);

    msg->mq_type = 1;
    strcpy(msg->type, STATECAPTURE_TYPE);
    msg->size = sizeof(MachineMetrics);

    MachineMetrics *metrics = (MachineMetrics *)msg->data;
    strcpy(metrics->uuid, "mock-master-uuid-12345");
    strcpy(metrics->ip, "127.0.0.1");
    metrics->port = 9005;
    metrics->role = 3; // ROLE_MASTER
    metrics->cpu_usage = 5.0;
    metrics->mem_usage = 10.0;
    metrics->disk_usage = 15.0;

    printf("[SimulateMaster] Registering master node with Controller...\n");
    send_msg(target_ip, target_port, "master_out", msg);
    free(msg);

    // 2. Loop and send HEARTBEAT every 2 seconds
    size_t hb_size = sizeof(message_t) + sizeof(MachineHeartbeat);
    while (1) {
        sleep(2);
        
        message_t *hb_msg = malloc(hb_size);
        memset(hb_msg, 0, hb_size);
        hb_msg->mq_type = 1;
        strcpy(hb_msg->type, HB_TYPE);
        hb_msg->size = sizeof(MachineHeartbeat);

        MachineHeartbeat *hb = (MachineHeartbeat *)hb_msg->data;
        strcpy(hb->uuid, "mock-master-uuid-12345");
        hb->type = MSG_HEARTBEAT;

        send_msg(target_ip, target_port, "master_out", hb_msg);
        free(hb_msg);
    }

    return 0;
}
