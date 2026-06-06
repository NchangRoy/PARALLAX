#ifndef STATE_RECEIVER_H
#define STATE_RECEIVER_H

#include "node.h"

// ─── State Receiver Thread ────────────────────────────────────────────────────
void* state_receiver_thread_run(void* arg);    // lance le thread
void  state_receiver_stop(void);               // arrête proprement le thread

// ─── Heartbeat Monitor Thread ────────────────────────────────────────────────
void* heartbeat_monitor_thread_run(void* arg); // monitors node heartbeat timeouts
void  heartbeat_monitor_stop(void);            // stops the monitor thread

#endif