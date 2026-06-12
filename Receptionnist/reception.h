#ifndef RECEPTION_H
#define RECEPTION_H

#include <pthread.h>
#include "../parallax/state_message.h"

// ═══════════════════════════════════════════════════════════════════════════
//  RECEPTIONIST CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════

#define RECEPTIONIST_LISTENING_PORT 9002      // Port receptionist listens on for submissions
#define RECEPTIONIST_QUERY_INTERVAL 5         // Query master IP every 5 seconds
#define REQUEST_MASTER_IP_TIMEOUT 10          // Timeout waiting for master IP from controller

// ═══════════════════════════════════════════════════════════════════════════
//  DATA STRUCTURES
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Master IP Request - sent by receptionist to controller
 * Payload: just identifies the receptionist requesting the master IP
 */
typedef struct {
    char receptionist_uuid[37];   // UUID of receptionist requesting
    char receptionist_ip[16];     // IP of receptionist (for reply)
    int receptionist_port;        // Port of receptionist (for reply)
} MasterIPRequest;

/**
 * Master IP Response - sent by controller to receptionist
 * Updates receptionist with current master IP/port
 */
typedef struct {
    char master_uuid[37];         // UUID of current master
    char master_ip[16];           // Master IP address
    int master_port;              // Master port
} MasterIPResponse;

/**
 * Code Submission - sent by backend to receptionist
 * Contains program code to be executed on master
 */
typedef struct {
    char program_id[64];          // Unique program identifier
    char source_ip[16];           // IP of backend submitting code
    int source_port;              // Port of backend
    uint64_t code_size;           // Size of the code payload
    char code[8000];              // Program code (max 8KB)
} CodeSubmission;

/**
 * Code Forward - sent by receptionist to master
 * Forwards received code to master for execution
 */
typedef struct {
    char program_id[64];          // Program identifier (from submission)
    char receptionist_uuid[37];   // Which receptionist is forwarding
    uint64_t code_size;           // Size of the code
    char code[8000];              // Program code
} CodeForward;

/**
 * Receptionist State
 */
typedef struct {
    char uuid[37];                // Receptionist UUID
    char controller_ip[16];       // Controller IP (must be discovered)
    char master_ip[16];           // Current master IP (updated by controller)
    int master_port;              // Current master port
    pthread_mutex_t master_lock;  // Lock for master_ip/master_port updates
    volatile int master_ip_valid; // 1 = has valid master IP, 0 = waiting for first response
} ReceptionistState;

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Initialize receptionist:
 * 1. Discover controller via broadcast
 * 2. Start thread to query master IP from controller
 * 3. Start listening thread for code submissions
 */
void receptionist_init(void);

/**
 * Stop receptionist and all threads
 */
void receptionist_stop(void);

/**
 * Get current master IP (thread-safe)
 */
void receptionist_get_master(char *ip, int *port);

/**
 * Get receptionist state (internal use)
 */
ReceptionistState* receptionist_get_state(void);

#endif // RECEPTION_H
