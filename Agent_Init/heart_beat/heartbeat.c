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
        hb.timestamp = time(NULL);
        
        // Fill UUID from agent
        strncpy(hb.uuid, get_agent_uuid(), sizeof(hb.uuid) - 1);
        
        // Fill IP and port dynamically
        char iface[16] = {0};
        load_network_interface(iface, sizeof(iface));
        get_local_ip(hb.ip, sizeof(hb.ip), iface);
        if (strcmp(hb.ip, "0.0.0.0") == 0) {
            strcpy(hb.ip, "127.0.0.1"); // Fallback if interface is down
        }
        hb.port = 9000; // Default worker listening port
        
        // Fill role from global agent role
        extern int agent_role; // Declared in init.c
        hb.role = agent_role;
        
        // Send lightweight heartbeat
        message_t *pkt = (message_t *)malloc(sizeof(message_t) + sizeof(MachineHeartbeat));
        if (pkt) {
            pkt->mq_type = 1;
            strcpy(pkt->type, HB_TYPE);
            pkt->size = sizeof(MachineHeartbeat);
            memcpy(pkt->data, &hb, sizeof(MachineHeartbeat));
            
            send_msg(controller_ip, 9000, NULL, pkt);
            
            free(pkt);
            printf("[HEARTBEAT] Lightweight heartbeat sent (role=%d)\n", hb.role);
        }

        sleep(HEARTBEAT_INTERVAL);
    }

    printf("[HEARTBEAT] Thread stopped\n");
    return NULL;
}

void heartbeat_stop(void){
    heartbeat_running = 0;
}