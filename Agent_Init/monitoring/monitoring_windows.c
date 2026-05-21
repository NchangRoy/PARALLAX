#include <stdio.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <pdh.h>

// Linker hints (si compilé avec gcc/mingw)
// Compiler avec : gcc ... -lpsapi -liphlpapi -lpdh

// ============================================================
// CPU USAGE
// ============================================================
static void read_cpu_usage(MachineMetrics *metrics) {
    static ULONGLONG prev_idle = 0, prev_kernel = 0, prev_user = 0;

    FILETIME idle_time, kernel_time, user_time;
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        metrics->cpu_usage = 0.0f;
        return;
    }

    // Convertir FILETIME en ULONGLONG
    ULONGLONG idle   = ((ULONGLONG)idle_time.dwHighDateTime   << 32) | idle_time.dwLowDateTime;
    ULONGLONG kernel = ((ULONGLONG)kernel_time.dwHighDateTime << 32) | kernel_time.dwLowDateTime;
    ULONGLONG user   = ((ULONGLONG)user_time.dwHighDateTime   << 32) | user_time.dwLowDateTime;

    ULONGLONG delta_idle   = idle   - prev_idle;
    ULONGLONG delta_kernel = kernel - prev_kernel;
    ULONGLONG delta_user   = user   - prev_user;

    // kernel_time inclut idle_time sur Windows
    ULONGLONG delta_total = delta_kernel + delta_user;
    ULONGLONG delta_busy  = delta_total  - delta_idle;

    if (delta_total > 0)
        metrics->cpu_usage = (float)delta_busy / delta_total * 100.0f;
    else
        metrics->cpu_usage = 0.0f;

    prev_idle   = idle;
    prev_kernel = kernel;
    prev_user   = user;
}

// ============================================================
// MEMORY
// ============================================================
static void read_memory(MachineMetrics *metrics) {
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);

    if (!GlobalMemoryStatusEx(&mem_status)) return;

    metrics->mem_total_gb     = mem_status.ullTotalPhys     / (1024.0f * 1024.0f * 1024.0f);
    metrics->mem_free_gb      = mem_status.ullAvailPhys     / (1024.0f * 1024.0f * 1024.0f);
    metrics->mem_available_gb = metrics->mem_free_gb;
    metrics->mem_used_gb      = metrics->mem_total_gb - metrics->mem_free_gb;
}

// ============================================================
// DISK
// ============================================================
static void read_disk(MachineMetrics *metrics) {
    static ULONGLONG prev_read_bytes = 0, prev_write_bytes = 0;
    static long prev_time = 0;

    // Espace disque sur C:\
    ULARGE_INTEGER free_bytes, total_bytes;
    if (GetDiskFreeSpaceExA("C:\\", NULL, &total_bytes, &free_bytes)) {
        metrics->disk_total_gb = total_bytes.QuadPart / (1024.0f * 1024.0f * 1024.0f);
        metrics->disk_free_gb  = free_bytes.QuadPart  / (1024.0f * 1024.0f * 1024.0f);
    }

    // IO usage via performance counters (PDH)
    // Simplifié : on utilise GetSystemInfo pour l'instant
    // Pour un vrai IO%, il faudrait PDH queries — trop lourd pour ce module
    // On laisse à 0 et on note le TODO
    metrics->disk_usage_percent = 0.0f; // TODO: implémenter via PDH
}

// ============================================================
// NETWORK
// ============================================================
static void read_network(MachineMetrics *metrics) {
    static ULONGLONG prev_rx = 0, prev_tx = 0;
    static long prev_time = 0;

    // Bande passante via GetIfTable
    MIB_IFTABLE *if_table = NULL;
    DWORD size = 0;

    GetIfTable(NULL, &size, FALSE);
    if_table = (MIB_IFTABLE *)malloc(size);
    if (!if_table) return;

    ULONGLONG rx_bytes = 0, tx_bytes = 0;
    if (GetIfTable(if_table, &size, FALSE) == NO_ERROR) {
        for (DWORD i = 0; i < if_table->dwNumEntries; i++) {
            MIB_IFROW *row = &if_table->table[i];
            // Ignorer loopback (type 24) et interfaces inactives
            if (row->dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (row->dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
            rx_bytes += row->dwInOctets;
            tx_bytes += row->dwOutOctets;
        }
    }
    free(if_table);

    long now = time(NULL);
    long delta_time = now - prev_time;
    ULONGLONG delta_bytes = (rx_bytes - prev_rx) + (tx_bytes - prev_tx);

    if (delta_time > 0)
        metrics->network_bandwidth_mbps = (delta_bytes / (float)delta_time) / (1024.0f * 1024.0f);
    else
        metrics->network_bandwidth_mbps = 0.0f;

    prev_rx   = rx_bytes;
    prev_tx   = tx_bytes;
    prev_time = now;

    // Connexions TCP actives via GetTcpTable
    MIB_TCPTABLE *tcp_table = NULL;
    DWORD tcp_size = 0;
    GetTcpTable(NULL, &tcp_size, FALSE);
    tcp_table = (MIB_TCPTABLE *)malloc(tcp_size);
    int count = 0;
    if (tcp_table && GetTcpTable(tcp_table, &tcp_size, FALSE) == NO_ERROR)
        count = (int)tcp_table->dwNumEntries;
    free(tcp_table);
    metrics->active_connections = count;
}

// ============================================================
// SYSTEM
// ============================================================
static void read_system(MachineMetrics *metrics) {
    static long prev_time = 0;
    static ULONGLONG prev_ctxt = 0;

    // Uptime
    metrics->uptime_seconds = (long)(GetTickCount64() / 1000ULL);

    // Processus actifs
    DWORD pids[1024];
    DWORD needed = 0;
    if (EnumProcesses(pids, sizeof(pids), &needed))
        metrics->active_processes = (int)(needed / sizeof(DWORD));

    // Load average : n'existe pas nativement sur Windows
    // On approxime avec le CPU usage sur 1/5/15 min via PDH
    // Pour l'instant on met cpu_usage comme valeur unique
    metrics->load_avg[0] = metrics->cpu_usage / 100.0f;
    metrics->load_avg[1] = metrics->cpu_usage / 100.0f;
    metrics->load_avg[2] = metrics->cpu_usage / 100.0f;

    // Context switch rate via NtQuerySystemInformation (non documenté)
    // Trop risqué à utiliser en production — on laisse à 0
    metrics->context_switch_rate = 0.0f; // TODO
}

// ============================================================
// FLAGS
// ============================================================
static void compute_flags(MachineMetrics *metrics) {
    metrics->is_overloaded = 0;

    if (metrics->cpu_usage > OVERLOAD_CPU_THRESHOLD) {
        metrics->is_overloaded = 1;
        return;
    }

    if (metrics->mem_total_gb > 0.0f) {
        float mem_usage_percent = (metrics->mem_used_gb / metrics->mem_total_gb) * 100.0f;
        if (mem_usage_percent > OVERLOAD_MEM_THRESHOLD)
            metrics->is_overloaded = 1;
    }
}