// Compiler avec : gcc ... -lpthread

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <mach/mach.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>

// ============================================================
// CPU USAGE
// ============================================================
static void read_cpu_usage(MachineMetrics *metrics) {
    static unsigned long long prev_idle = 0, prev_total = 0;

    host_cpu_load_info_data_t cpu_info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&cpu_info, &count) != KERN_SUCCESS) {
        metrics->cpu_usage = 0.0f;
        return;
    }

    unsigned long long user   = cpu_info.cpu_ticks[CPU_STATE_USER];
    unsigned long long system = cpu_info.cpu_ticks[CPU_STATE_SYSTEM];
    unsigned long long idle   = cpu_info.cpu_ticks[CPU_STATE_IDLE];
    unsigned long long nice   = cpu_info.cpu_ticks[CPU_STATE_NICE];

    unsigned long long total = user + system + idle + nice;
    unsigned long long delta_total = total - prev_total;
    unsigned long long delta_idle  = idle  - prev_idle;

    if (delta_total > 0)
        metrics->cpu_usage = (1.0f - (float)delta_idle / delta_total) * 100.0f;
    else
        metrics->cpu_usage = 0.0f;

    prev_total = total;
    prev_idle  = idle;
}

// ============================================================
// MEMORY
// ============================================================
static void read_memory(MachineMetrics *metrics) {
    // Total RAM via sysctl
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    uint64_t mem_total = 0;
    size_t len = sizeof(mem_total);
    sysctl(mib, 2, &mem_total, &len, NULL, 0);
    metrics->mem_total_gb = mem_total / (1024.0f * 1024.0f * 1024.0f);

    // Used/free via mach
    vm_size_t page_size;
    host_page_size(mach_host_self(), &page_size);

    vm_statistics64_data_t vm_stats;
    mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64,
                      (host_info64_t)&vm_stats, &vm_count);

    uint64_t free_mem = (vm_stats.free_count + vm_stats.inactive_count) * page_size;
    uint64_t used_mem = mem_total - free_mem;

    metrics->mem_free_gb      = free_mem / (1024.0f * 1024.0f * 1024.0f);
    metrics->mem_used_gb      = used_mem / (1024.0f * 1024.0f * 1024.0f);
    metrics->mem_available_gb = metrics->mem_free_gb;
}

// ============================================================
// DISK
// ============================================================
static void read_disk(MachineMetrics *metrics) {
    // Espace disque via statvfs (identique à Linux)
    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        metrics->disk_total_gb = (st.f_blocks * st.f_frsize) / (1024.0f * 1024.0f * 1024.0f);
        metrics->disk_free_gb  = (st.f_bfree  * st.f_frsize) / (1024.0f * 1024.0f * 1024.0f);
    }

    // IO usage : non disponible nativement sans IOKit (framework Objective-C)
    // On laisse à 0 — à implémenter avec IOKit si nécessaire
    metrics->disk_usage_percent = 0.0f;
}

// ============================================================
// NETWORK
// ============================================================
static void read_network(MachineMetrics *metrics) {
    static long long prev_rx = 0, prev_tx = 0;
    static long prev_time = 0;

    // Lecture via sysctl net.if.total (somme de toutes les interfaces sauf lo)
    struct ifmibdata ifmd;
    int mib[6] = { CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, 0, IFDATA_GENERAL };
    size_t len = sizeof(ifmd);

    int if_count = 0;
    size_t if_count_len = sizeof(if_count);
    int mib_count[4] = { CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_SYSTEM };
    sysctl(mib_count, 4, &if_count, &if_count_len, NULL, 0);

    long long rx_bytes = 0, tx_bytes = 0;
    for (int i = 1; i <= if_count; i++) {
        mib[4] = i;
        len = sizeof(ifmd);
        if (sysctl(mib, 6, &ifmd, &len, NULL, 0) == 0) {
            if (strcmp(ifmd.ifmd_name, "lo0") == 0) continue; // skip loopback
            rx_bytes += ifmd.ifmd_data.ifi_ibytes;
            tx_bytes += ifmd.ifmd_data.ifi_obytes;
        }
    }

    long now = time(NULL);
    long delta_time  = now - prev_time;
    long long delta_bytes = (rx_bytes - prev_rx) + (tx_bytes - prev_tx);

    if (delta_time > 0)
        metrics->network_bandwidth_mbps = (delta_bytes / (float)delta_time) / (1024.0f * 1024.0f);
    else
        metrics->network_bandwidth_mbps = 0.0f;

    prev_rx   = rx_bytes;
    prev_tx   = tx_bytes;
    prev_time = now;

    // Connexions actives via sysctl net.inet.tcp.pcbcount
    int tcp_count = 0;
    size_t tcp_len = sizeof(tcp_count);
    // Fallback simple : lire via fichier si disponible
    FILE *f = popen("sysctl -n net.inet.tcp.pcbcount 2>/dev/null", "r");
    if (f) {
        fscanf(f, "%d", &tcp_count);
        pclose(f);
    }
    metrics->active_connections = tcp_count;
}

// ============================================================
// SYSTEM
// ============================================================
static void read_system(MachineMetrics *metrics) {
    static long prev_time = 0;
    static long long prev_ctxt = 0;

    // Load average (POSIX — fonctionne sur macOS)
    double loadavg[3];
    if (getloadavg(loadavg, 3) != -1) {
        metrics->load_avg[0] = (float)loadavg[0];
        metrics->load_avg[1] = (float)loadavg[1];
        metrics->load_avg[2] = (float)loadavg[2];
    }

    // Uptime via sysctl
    struct timeval boottime;
    size_t len = sizeof(boottime);
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0) {
        metrics->uptime_seconds = time(NULL) - boottime.tv_sec;
    }

    // Processus actifs via sysctl
    int nprocs = 0;
    size_t nprocs_len = sizeof(nprocs);
    int mib_proc[2] = { CTL_KERN, KERN_NPROCS };
    if (sysctl(mib_proc, 2, &nprocs, &nprocs_len, NULL, 0) == 0)
        metrics->active_processes = nprocs;

    // Context switches via sysctl vm.stats.sys.v_swtch
    // Non disponible directement sur macOS sans kstat
    // On utilise une approximation via mach task info
    metrics->context_switch_rate = 0.0f; // TODO: implémenter via mach_task_info
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