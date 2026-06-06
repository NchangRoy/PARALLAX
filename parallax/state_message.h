#ifndef STATE_MESSAGE_H
#define STATE_MESSAGE_H

#define MSG_HELLO           1
#define MSG_STATECAPTURE    2
#define MSG_STATECAPTURE_INIT  3
#define MSG_HEARTBEAT  4


#define HELLO_TYPE "HELLO"
#define STATECAPTURE_TYPE "STATECAPTURE"
#define STATECAPTURE_INIT_TYPE "STATECAPTURE_INIT"
#define HB_TYPE "HEARTBEAT"
#define BE_TYPE "BACKEND"


#include <stdint.h>
#include <time.h>

typedef struct{
    char uuid[37]; // 36 chars + null terminator
    char ip[16];
    int port;
    int type;
    int role; // ROLE_UNKNOWN=0, ROLE_WORKER=1, ROLE_CONTROLLER=2, ROLE_MASTER=3

    // CPU
    float cpu_usage;
    float load_avg[3];
    
    // Memory
    float mem_usage;
    float mem_available_mb;
    long mem_used_mb;
    long mem_total_mb;
    
    // Disk
    float disk_usage;
    long disk_used_mb;
    long disk_total_mb;

    // Network
    float network_bandwidth_mbps;
    int active_connections;

    // System
    int active_processes;
    float context_switch_rate;
    long uptime_seconds;

    // Computed
    int is_overloaded;

    // Timestamp
    time_t timestamp;

    int queue_len;
    float score;

    int cpu_cores;
    int cpu_threads_per_core;
    float cpu_freq_mhz;
    char  cpu_model[128];
    char  disk_mount[32];
    char  network_iface[16];
}MachineMetrics;

// heartbeat message (sent every 2 seconds)
typedef struct{
    char uuid[37];     // 36 chars + null terminator
    int type;          // MSG_HEARTBEAT
}MachineHeartbeat;

#endif // STATE_MESSAGE_H