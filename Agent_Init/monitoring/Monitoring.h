#ifndef MONITORING_H
#define MONITORING_H

#include <pthread.h>
#include "state_message.h"

// ================OS DETECTION========================
#if (defined(_WIN32) || defined(_WIN64)) && !defined(OS_WINDOWS)
    #define OS_WINDOWS
#elif (defined(__APPLE__) || defined(__MACH__)) && !defined(OS_MACOS)
    #define OS_MACOS
#elif defined(__linux__) && !defined(OS_LINUX)
    #define OS_LINUX
#endif

#define MONITORING_INTERVAL 5
#define OVERLOAD_CPU_THRESHOLD 85.0f
#define OVERLOAD_MEM_THRESHOLD 90.0f


void *monitoring_thread_run(void *arg);
MachineMetrics monitoring_get_latest();
void monitoring_stop();

#endif