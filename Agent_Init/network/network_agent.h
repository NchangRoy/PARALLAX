#ifndef NETWORK_AGENT_H
#define NETWORK_AGENT_H

#include "socket.h"

void *network_thread_run(void *args);
void network_stop();
void send_msg(char *Ip, int port, message_t *message);

#endif
