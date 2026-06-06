#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <pthread.h>
#include "state_message.h"

#define HEARTBEAT_INTERVAL 2

void *heartbeat_thread_run(void *arg);
void heartbeat_stop();

#endif