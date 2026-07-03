#ifndef NETWORK_AGENT_H
#define NETWORK_AGENT_H

#include "socket.h"

#define NETWORK_AGENT_MTYPE 1L
#define NETWORK_AGENT_MAX_DATA 8000
typedef struct {
  long mtype;
  char type[64];
  char recv_type[64];
  char sender_ip[16];
  int sender_port;
  uint64_t size;
  char data[NETWORK_AGENT_MAX_DATA];
} queued_message;

typedef struct {
  long mtype;
  char ip[16];
  int port;
  char type[64];
  char recv_type[64];
  char sender_ip[16];
  int sender_port;
  uint64_t size;
  char data[NETWORK_AGENT_MAX_DATA];
} outgoing_message;

typedef struct {
  int port;
  char queue_name[64];
} network_agent_config;

void *network_thread_run(void *args);
void network_stop();
/* Port this process's network agent is bound to (9000 for master/worker/
   controller, 9008 for receptionist, ...) — defaults to 9000 until
   network_thread_run sets it from its config. Used by ms_queue.c to isolate
   fixed queue names (e.g. "HELLO_TYPE") between different agent roles
   sharing one host. */
int network_agent_get_port(void);
void send_msg(char *Ip, int port, char *queue_name, message_t *message);
void send_broadcast(int port, message_t *message);
void send_broadcast_iface(int port, message_t *message, const char *iface_name);

#endif
