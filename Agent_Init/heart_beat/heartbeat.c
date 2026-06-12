#include <stdio.h>
#include "heartbeat.h"
#include "../init.h"
#include "../network/network_agent.h"

// ══════════════════════════════════════════════════════════════════════════
//  LIGHTWEIGHT HEARTBEAT THREAD (Sends every 2 seconds with role only)
// ══════════════════════════════════════════════════════════════════════════

extern char controller_ip[16];
static volatile int heartbeat_running = 0;

void *heartbeat_thread_run(void *arg){
    (void)arg; // avoids warning "Unused parameter"
    
    heartbeat_running = 1;
    printf("[HEARTBEAT] Thread started\n");

    while(heartbeat_running){
        MachineHeartbeat hb;
        memset(&hb, 0, sizeof(MachineHeartbeat));
        
        // Fill heartbeat with minimal data
        hb.type = MSG_HEARTBEAT;
        strncpy(hb.uuid, get_agent_uuid(), sizeof(hb.uuid) - 1);
        
        // Send lightweight heartbeat
        message_t *pkt = (message_t *)malloc(sizeof(message_t) + sizeof(MachineHeartbeat));
        if (pkt) {
            pkt->mq_type = 1;
            strcpy(pkt->type, HB_TYPE);
            pkt->size = sizeof(MachineHeartbeat);
            memcpy(pkt->data, &hb, sizeof(MachineHeartbeat));
            
            send_msg(controller_ip, 9000, NULL, pkt);
            
            free(pkt);
            printf("[HEARTBEAT] Lightweight heartbeat sent for node %s\n", hb.uuid);
        }

        sleep(HEARTBEAT_INTERVAL);
    }

    printf("[HEARTBEAT] Thread stopped\n");
    return NULL;
}

void heartbeat_stop(void){
    heartbeat_running = 0;
}