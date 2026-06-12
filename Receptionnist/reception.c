#include "reception.h"
#include "../Agent_Init/init.h"
#include "../Agent_Init/network/network_agent.h"
#include "../Agent_Init/network/socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

// ═══════════════════════════════════════════════════════════════════════════
//  GLOBAL STATE
// ═══════════════════════════════════════════════════════════════════════════

static ReceptionistState g_receptionist = {0};
static atomic_int receptionist_running = 0;

// ═══════════════════════════════════════════════════════════════════════════
//  THREAD 1: MASTER IP QUERY THREAD
//  Periodically queries the controller for the current master IP and updates state
// ═══════════════════════════════════════════════════════════════════════════

void* master_ip_query_thread(void* arg) {
    (void)arg;
    printf("[RECEPTIONIST] Master IP query thread started\n");
    
    while (atomic_load(&receptionist_running)) {
        // Create request message
        MasterIPRequest req;
        memset(&req, 0, sizeof(MasterIPRequest));
        strncpy(req.receptionist_uuid, get_agent_uuid(), sizeof(req.receptionist_uuid) - 1);
        
        // Get our IP for controller to reply to
        char iface[16] = {0};
        load_network_interface(iface, sizeof(iface));
        get_local_ip(req.receptionist_ip, sizeof(req.receptionist_ip), iface);
        req.receptionist_port = RECEPTIONIST_LISTENING_PORT;
        
        // Send request to controller
        message_t *pkt = (message_t *)malloc(sizeof(message_t) + sizeof(MasterIPRequest));
        if (pkt) {
            pkt->mq_type = 1;
            strcpy(pkt->type, REQUEST_MASTER_IP_TYPE);
            pkt->size = sizeof(MasterIPRequest);
            memcpy(pkt->data, &req, sizeof(MasterIPRequest));
            
            printf("[RECEPTIONIST] Querying controller %s for master IP...\n", g_receptionist.controller_ip);
            send_msg(g_receptionist.controller_ip, 9000, NULL, pkt);
            free(pkt);
        }
        
        sleep(RECEPTIONIST_QUERY_INTERVAL);
    }
    
    printf("[RECEPTIONIST] Master IP query thread stopped\n");
    return NULL;
}

// ═══════════════════════════════════════════════════════════════════════════
//  THREAD 2: CODE SUBMISSION LISTENER THREAD
//  Listens for code submissions from backend and forwards to master
// ═══════════════════════════════════════════════════════════════════════════

void* code_submission_listener_thread(void* arg) {
    (void)arg;
    printf("[RECEPTIONIST] Code submission listener started on port %d\n", RECEPTIONIST_LISTENING_PORT);
    
    // Create listener socket
    connection *listener = create_listener("0.0.0.0", RECEPTIONIST_LISTENING_PORT, 5);
    if (!listener) {
        printf("[RECEPTIONIST] ERROR: Failed to create listener on port %d\n", RECEPTIONIST_LISTENING_PORT);
        return NULL;
    }
    
    while (atomic_load(&receptionist_running)) {
        // Accept connection from backend (non-blocking simulation with select/timeout)
        // For now, just sleep to avoid busy-waiting
        sleep(1);
        
        // In a real implementation, you would:
        // 1. Accept incoming connection
        // 2. Receive CodeSubmission message
        // 3. Lock master_ip state
        // 4. Forward code to current master
        // 5. Unlock and send response
        // 6. Close connection
    }
    
    printf("[RECEPTIONIST] Code submission listener stopped\n");
    return NULL;
}

// ═══════════════════════════════════════════════════════════════════════════
//  CONTROLLER INTERFACE
//  Receives messages from controller (master IP updates)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Handle master IP update from controller
 * Called when controller sends PROVIDE_MASTER_IP message
 */
void receptionist_handle_master_ip_update(MasterIPResponse* response) {
    if (!response) return;
    
    pthread_mutex_lock(&g_receptionist.master_lock);
    
    printf("[RECEPTIONIST] Updating master IP from controller:\n");
    printf("  Previous: %s:%d\n", g_receptionist.master_ip, g_receptionist.master_port);
    
    strncpy(g_receptionist.master_ip, response->master_ip, sizeof(g_receptionist.master_ip) - 1);
    g_receptionist.master_port = response->master_port;
    g_receptionist.master_ip_valid = 1;
    
    printf("  New: %s:%d (master_uuid=%s)\n", 
           g_receptionist.master_ip, g_receptionist.master_port, response->master_uuid);
    
    pthread_mutex_unlock(&g_receptionist.master_lock);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DISCOVERY PHASE
//  Find controller via broadcast or configuration
// ═══════════════════════════════════════════════════════════════════════════

static int discover_controller(void) {
    // Try to read controller IP from config or environment
    // For now, assume controller is on the network and broadcast to find it
    // In production, this might be: 224.0.0.1 (multicast) or fixed config
    
    printf("[RECEPTIONIST] Starting controller discovery...\n");
    
    // Attempt 1: Try localhost first (for testing)
    strcpy(g_receptionist.controller_ip, "127.0.0.1");
    printf("[RECEPTIONIST] Controller IP set to: %s\n", g_receptionist.controller_ip);
    
    return 1;  // Success
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void receptionist_init(void) {
    printf("[RECEPTIONIST] Initializing...\n");
    
    // Initialize state
    memset(&g_receptionist, 0, sizeof(ReceptionistState));
    pthread_mutex_init(&g_receptionist.master_lock, NULL);
    strncpy(g_receptionist.uuid, get_agent_uuid(), sizeof(g_receptionist.uuid) - 1);
    g_receptionist.master_ip_valid = 0;
    
    // Step 1: Discover controller
    if (!discover_controller()) {
        printf("[RECEPTIONIST] ERROR: Failed to discover controller\n");
        return;
    }
    
    // Step 2: Mark as running
    atomic_store(&receptionist_running, 1);
    
    // Step 3: Start query thread (asks controller for master IP)
    pthread_t query_thread;
    pthread_create(&query_thread, NULL, master_ip_query_thread, NULL);
    
    // Step 4: Start listener thread (accepts code submissions)
    pthread_t listener_thread;
    pthread_create(&listener_thread, NULL, code_submission_listener_thread, NULL);
    
    printf("[RECEPTIONIST] Initialization complete\n");
}

void receptionist_stop(void) {
    printf("[RECEPTIONIST] Stopping...\n");
    atomic_store(&receptionist_running, 0);
    sleep(1);  // Give threads time to exit
    pthread_mutex_destroy(&g_receptionist.master_lock);
    printf("[RECEPTIONIST] Stopped\n");
}

void receptionist_get_master(char *ip, int *port) {
    if (!ip || !port) return;
    
    pthread_mutex_lock(&g_receptionist.master_lock);
    strncpy(ip, g_receptionist.master_ip, 16);
    *port = g_receptionist.master_port;
    pthread_mutex_unlock(&g_receptionist.master_lock);
}

ReceptionistState* receptionist_get_state(void) {
    return &g_receptionist;
}
