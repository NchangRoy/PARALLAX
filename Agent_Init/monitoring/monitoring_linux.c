#include "Monitoring.h"
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

// ============================================================
// CPU USAGE
// ============================================================
static void read_cpu_usage(MachineMetrics *metrics){
    static long prev_idle = 0, prev_total = 0;
    
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    long user, nice, system, idle, iowait, irq, softirq;
    fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld", 
        &user, &nice, &system, &idle, &iowait, &irq, &softirq);
    fclose(f);

    long total = user + nice + system + idle + iowait + irq + softirq;
    long idle_total = idle + iowait;

    long delta_total = total - prev_total;
    long delta_idle = idle_total - prev_idle;

    if (delta_total > 0)
        metrics->cpu_usage = (1.0f * (float)delta_idle / delta_total) * 100.0f;
    else
        metrics->cpu_usage = 0.0f;

    prev_total = total;
    prev_idle = idle_total;
}

// ============================================================
// MEMORY
// ============================================================
static void read_memory(MachineMetrics *metrics){
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    long mem_total_kb = 0, mem_free_kb = 0, mem_available_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld kB", &mem_total_kb) == 1) continue;
        if (sscanf(line, "MemFree: %ld kB", &mem_free_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %ld kB", &mem_available_kb) == 1) continue;
    }
    fclose(f);

    metrics->mem_available_gb = mem_available_kb / (1024.0f * 1024.0f);
    metrics->mem_total_gb = mem_total_kb / (1024.0f * 1024.0f);
    metrics->mem_free_gb = mem_free_kb / (1024.0f * 1024.0f);
    metrics->mem_used_gb = metrics->mem_total_gb - metrics->mem_available_gb;

}

// ============================================================
// DISK
// ============================================================
static void read_disk(MachineMetrics *metrics){
    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        metrics->disk_total_gb = (st.f_blocks * st.f_frsize) / (1024.0f * 1024.0f * 1024.0f);
        metrics->disk_free_gb = (st.f_bfree * st.f_frsize) / (1024.0f * 1024.0f * 1024.0f);
    }

    static long prev_io_ms = 0;
    static long prev_time = 0;

    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;

    char line[256];
    long io_ms = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "sda ") || strstr(line, "nvme0n1 ") || strstr(line, "vda ")) {
            sscanf(line, "%*d %*d %*s %*d %*d %*d %*d %*d %*d %*d %ld", 
                &io_ms);
            break;
        }
    }
    fclose(f);

    long now = time(NULL);
    long delta_time = (now - prev_time) * 1000;
    long delta_io = io_ms - prev_io_ms;

    if (delta_time > 0)
        metrics->disk_usage_percent = (float)delta_io / delta_time * 100.0f;
    else
        metrics->disk_usage_percent = 0.0f;

    prev_io_ms = io_ms;
    prev_time = now;
}

// ============================================================
// NETWORK
// ============================================================
static void read_network(MachineMetrics *metrics){
    static long prev_rx = 0, prev_tx = 0;
    static long prev_time = 0;

    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
 
    char line[256];
    long rx_bytes = 0, tx_bytes = 0;

    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);

    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        sscanf(line, "%31s", iface);
        if (strcmp(iface, "lo:") == 0) continue; // Skip loopback
        
        sscanf(line, "%*s %ld %*d %*d %*d %*d %*d %*d %*d %ld", 
            &rx_bytes, &tx_bytes);
        break;
    }
    fclose(f);

    long now = time(NULL);
    long delta_time = now - prev_time;
    long delta_bytes = (rx_bytes - prev_rx) + (tx_bytes - prev_tx);

    if (delta_time > 0)
        metrics->network_bandwidth_mbps = (delta_bytes / (float)delta_time) / (1024.0f * 1024.0f);
    else
        metrics->network_bandwidth_mbps = 0.0f;

    prev_rx = rx_bytes;
    prev_tx = tx_bytes;
    prev_time = now;

    f = fopen("/proc/net/tcp", "r");
    if (!f) return;

    int count = 0;
    fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) count++;
    fclose(f);

    metrics->active_connections = count;
}

// ============================================================
// SYSTEM
// ============================================================
static void read_system(MachineMetrics *metrics){
    static long prev_context_switches = 0;
    static long prev_time = 0;

    FILE *f = fopen("/proc/loadavg", "r");
    if (f){
        int active = 0, total = 0;
        fscanf(f, "%f %f %f %d/%d", 
            &metrics->load_avg[0], 
            &metrics->load_avg[1], 
            &metrics->load_avg[2],
            &active,
            &total
        );
        metrics->active_processes = active;
        fclose(f);
    }

    f = fopen("/proc/uptime", "r");
    if (f) {
        float uptime_sec = 0;
        fscanf(f, "%f", &uptime_sec);
        metrics->uptime_seconds = (long)uptime_sec;
        fclose(f);
    }

    f = fopen("/proc/stat", "r");
    if (f) {
        char line[256];
        long context_switches = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "ctxt %ld", &context_switches) == 1) break;
        }
        fclose(f);

        long now = time(NULL);
        long delta_time = now - prev_time;
        long delta_context = context_switches - prev_context_switches;

        if (delta_time > 0)
            metrics->context_switch_rate = (float)delta_context / delta_time;
        else
            metrics->context_switch_rate = 0.0f;

        prev_context_switches = context_switches;
        prev_time = now;
    }           
}

// ============================================================
// FLAGS
// ============================================================
static void compute_flags(MachineMetrics *metrics){
    metrics->is_overloaded = 0;
    
    if(metrics->cpu_usage > OVERLOAD_CPU_THRESHOLD){
        metrics->is_overloaded = 1;
        return;
    } 
    
    if(metrics->mem_total_gb > 0.0f) {
        float mem_usage_percent = (metrics->mem_used_gb / metrics->mem_total_gb) * 100.0f;
        if(mem_usage_percent > OVERLOAD_MEM_THRESHOLD)
            metrics->is_overloaded = 1;
    }        
}