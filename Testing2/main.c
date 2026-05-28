#include "ms_queue.h"
#include "network_agent.h"
#include "linked_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/msg.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define NETWORK_AGENT_MTYPE 1L
#define NETWORK_AGENT_MAX_DATA 65536

typedef struct {
    long mtype;
    uint64_t type;
    uint64_t size;
    char data[NETWORK_AGENT_MAX_DATA];
} queued_message;

void *test_listener(void *arg) {
    int qid = *((int *)arg);
    queued_message msg;
    printf("Listener thread started on queue ID %d\n", qid);
    while (1) {
        ssize_t received = msgrcv(qid, &msg, sizeof(msg) - sizeof(long), NETWORK_AGENT_MTYPE, 0);
        if (received >= 0) {
            if (msg.size == 8) {
                uint64_t val = 0;
                memcpy(&val, msg.data, 8);
                printf("[Queue %d] Received network value: %llu\n", qid, (unsigned long long)val);
            } else {
                printf("[Queue %d] Received message with size %llu\n", qid, (unsigned long long)msg.size);
            }
        }
    }
    return NULL;
}

int main()
{
    printf("=== Starting System with 2 MQs ===\n");
    
    // Create first queue "1"
    int test_qid1 = create_mq("1", 8);
    if (test_qid1 < 0) {
        printf("Failed to create '1' queue\n");
        return 1;
    }

    // Create second queue "2"
    int test_qid2 = create_mq("2", 8);
    if (test_qid2 < 0) {
        printf("Failed to create '2' queue\n");
        return 1;
    }

    pthread_t tid1, tid2;
    if (pthread_create(&tid1, NULL, test_listener, &test_qid1) != 0) {
        printf("Failed to create thread 1\n");
        return 1;
    }
    if (pthread_create(&tid2, NULL, test_listener, &test_qid2) != 0) {
        printf("Failed to create thread 2\n");
        return 1;
    }

    // Start network agent
    start();
    printf("Network agent started. Waiting for messages on both queues...\n");

    // Wait a brief moment to ensure the agent's background sender thread is fully up
    usleep(500000); // 0.5s

    // Send an outbound network message via the agent to localhost:9001
    printf("Sending an outbound message to 127.0.0.1:9001...\n");
    size_t data_size = 14;
    message_t *out_msg = (message_t *)malloc(sizeof(message_t) + data_size);
    out_msg->type = 99; // arbitrary type
    out_msg->size = data_size;
    memcpy(out_msg->data, "HELLO NETCAT\n", data_size);
    send_msg("127.0.0.1", 9001, out_msg);
    free(out_msg);

    // Keep main thread alive
    while (1) {
        sleep(1);
    }

    stop();
    destroy_queues();
    return 0;
}