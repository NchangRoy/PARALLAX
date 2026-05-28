#include "socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

void send_to_queue(uint64_t queue_type, uint64_t val) {
    connection *conn = create_connection("127.0.0.1", 9000);
    if (conn == NULL) {
        printf("Failed to connect to 127.0.0.1:9000 for type %llu\n", (unsigned long long)queue_type);
        return;
    }

    size_t data_size = 8;
    message_t *msg = (message_t *)malloc(sizeof(message_t) + data_size);
    if (msg == NULL) {
        printf("Failed to allocate memory\n");
        close(conn->sockfd);
        free(conn);
        return;
    }

    msg->type = queue_type; 
    msg->size = data_size;
    memcpy(msg->data, &val, data_size);

    if (send_message(conn, msg) < 0) {
        printf("Failed to send message over socket for type %llu\n", (unsigned long long)queue_type);
    } else {
        printf("Successfully sent value (%llu) to queue type %llu over network\n", 
              (unsigned long long)val, (unsigned long long)msg->type);
    }

    free(msg);
    close(conn->sockfd);
    free(conn);
}

int main() {
    printf("=== Sender (Network) Program (2 Queues) ===\n");
    
    // Send to first queue
    send_to_queue(1, 11111111);
    
    // Send to second queue
    send_to_queue(2, 22222222);

    return 0;
}
