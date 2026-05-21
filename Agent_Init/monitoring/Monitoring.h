#ifndef MONITORING_H
#define MONITORING_H

#include <time.h>
#include <pthread.h>

// ================OS DETECTION========================
#if defined(_WIN32) || defined(_WIN64)
    #define OS_WINDOWS
#elif defined(__APPLE__) || defined(__MACH__)
    #define OS_MACOS
#elif defined(__linux__)
    #define OS_LINUX
#else
    #error "Unsupported OS"
#endif

#define MONITORING_INTERVAL 5
#define OVERLOAD_CPU_THRESHOLD 85.0f
#define OVERLOAD_MEM_THRESHOLD 90.0f

typedef struct{
    // CPU
    float cpu_usage;
    float load_avg[3];
    
    // Memory
    float mem_free_gb;
    float mem_available_gb;
    float mem_used_gb;
    float mem_total_gb;
    
    // Disk
    float disk_total_gb;
    float disk_free_gb;
    float disk_usage_percent;

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
}MachineMetrics;

void *monitoring_thread_run(void *arg);
MachineMetrics monitoring_get_latest();
void monitoring_stop();

#endif