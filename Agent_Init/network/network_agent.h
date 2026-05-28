#ifndef NETWORK_AGENT_H
#define NETWORK_AGENT_H

#include "socket.h"

#define NETWORK_AGENT_MTYPE 1L
#define NETWORK_AGENT_MAX_DATA 65536
typedef struct {
    long mtype;
    char type[64];
    uint64_t size;
    char data[NETWORK_AGENT_MAX_DATA];
} queued_message;

typedef struct {
    long mtype;
    char ip[16];
    int port;
    char type[64];
    uint64_t size;
    char data[NETWORK_AGENT_MAX_DATA];
} outgoing_message;

void *network_thread_run(void *args);
void network_stop();
void send_msg(char *Ip, int port, message_t *message);

#endif
