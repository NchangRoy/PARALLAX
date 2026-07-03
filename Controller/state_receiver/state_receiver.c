#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/msg.h>
#include <pthread.h>
#include <stdatomic.h>

#include "node.h"
#include "../../parallax/state_message.h"
#include "state_receiver.h"
#include "ms_queue.h"
#include "network_agent.h"
#include "../../Agent_Init/init.h"
#include "../../Receptionnist/reception.h"




#include <stdio.h>
#include <time.h>
#include "state_message.h"

// ══════════════════════════════════════════════════════════════════════════
//  HANDLERS DE MESSAGES
// ════════════════════════════════════════════════════════════════════════════

/**
 * Enregistre un nouveau nœud dans la table globale lors de la réception d'un MSG_HELLO.
 */
static void register_node(MachineMetrics* msg) {
    if (!msg || strlen(msg->uuid) == 0) return;
    pthread_mutex_lock(&g_node_table.lock);

    NodeInfo* node = node_table_find(&g_node_table, msg->uuid);
    if (node) {
        printf("[StateReceiver] Nœud %s déjà enregistré. Mise à jour des coordonnées et activation.\n",
               msg->uuid);
        strncpy(node->ip, msg->ip, sizeof(node->ip) - 1);
        node->ip[sizeof(node->ip) - 1] = '\0';
        node->port = msg->port;
        node->role = msg->role;
        node->status = NODE_ACTIF;
        node->last_heartbeat = time(NULL);
        
        pthread_mutex_unlock(&g_node_table.lock);
        return;
    }

    node = node_table_add(&g_node_table, msg->uuid, msg->ip, msg->port);
    if (node) {
        node->role = msg->role;
        printf("[StateReceiver] Nouveau nœud : uuid=%s ip=%s role=%d\n",
               node->uuid, node->ip, node->role);
    }
    pthread_mutex_unlock(&g_node_table.lock);
}

MachineMetrics * get_all_active_workers(void) {
    pthread_mutex_lock(&g_node_table.lock);
    
    // First count how many active workers we have
    int active_workers_count = 0;
    NodeInfo *curr = g_node_table.head;
    while (curr != NULL) {
        if (curr->role == 1 /* ROLE_WORKER */ && curr->status != NODE_EN_PANNE) {
            active_workers_count++;
        }
        curr = curr->next;
    }
    
    if (active_workers_count == 0) {
        pthread_mutex_unlock(&g_node_table.lock);
        return NULL;
    }
    
    // Allocate active_workers_count + 1 for sentinel
    MachineMetrics *metrics = malloc((active_workers_count + 1) * sizeof(MachineMetrics));
    if (!metrics) {
        pthread_mutex_unlock(&g_node_table.lock);
        return NULL;
    }
    memset(metrics, 0, (active_workers_count + 1) * sizeof(MachineMetrics));
    
    curr = g_node_table.head;
    int i = 0;
    while (curr != NULL && i < active_workers_count) {
        if (curr->role == 1 /* ROLE_WORKER */ && curr->status != NODE_EN_PANNE) {
            strncpy(metrics[i].uuid, curr->uuid, sizeof(metrics[i].uuid) - 1);
            strncpy(metrics[i].ip, curr->ip, sizeof(metrics[i].ip) - 1);
            metrics[i].port = curr->port;
            
            metrics[i].cpu_usage = curr->metrics.cpu_usage;
            metrics[i].mem_usage = curr->metrics.ram_usage;
            metrics[i].mem_used_mb = curr->metrics.ram_used_mb;
            metrics[i].disk_usage = curr->metrics.disk_usage;
            metrics[i].disk_used_mb = curr->metrics.disk_used_mb;
            metrics[i].queue_len = curr->metrics.queue_len;
            metrics[i].score = curr->metrics.score;
            metrics[i].load_avg[0] = curr->metrics.load_avg[0];
            metrics[i].load_avg[1] = curr->metrics.load_avg[1];
            metrics[i].load_avg[2] = curr->metrics.load_avg[2];
            
            metrics[i].cpu_cores = curr->hardware.cpu_cores;
            metrics[i].cpu_threads_per_core = curr->hardware.cpu_threads_per_core;
            metrics[i].cpu_freq_mhz = curr->hardware.cpu_freq_mhz;
            strncpy(metrics[i].cpu_model, curr->hardware.cpu_model, sizeof(metrics[i].cpu_model) - 1);
            metrics[i].mem_total_mb = curr->hardware.ram_total_mb;
            metrics[i].disk_total_mb = curr->hardware.disk_total_gb;
            strncpy(metrics[i].disk_mount, curr->hardware.disk_mount, sizeof(metrics[i].disk_mount) - 1);
            strncpy(metrics[i].network_iface, curr->hardware.network_iface, sizeof(metrics[i].network_iface) - 1);
            
            metrics[i].timestamp = curr->last_heartbeat;
            metrics[i].role = curr->role;
            
            i++;
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_node_table.lock);
    return metrics;
}


/* Returns ALL nodes (every role) with status encoded in active_connections */
static MachineMetrics *get_all_cluster_nodes(void) {
    pthread_mutex_lock(&g_node_table.lock);

    int count = 0;
    for (NodeInfo *n = g_node_table.head; n; n = n->next) count++;

    if (count == 0) {
        pthread_mutex_unlock(&g_node_table.lock);
        return NULL;
    }

    MachineMetrics *metrics = malloc((count + 1) * sizeof(MachineMetrics));
    if (!metrics) {
        pthread_mutex_unlock(&g_node_table.lock);
        return NULL;
    }
    memset(metrics, 0, (count + 1) * sizeof(MachineMetrics));

    int i = 0;
    for (NodeInfo *curr = g_node_table.head; curr && i < count; curr = curr->next) {
        strncpy(metrics[i].uuid, curr->uuid, sizeof(metrics[i].uuid) - 1);
        strncpy(metrics[i].ip, curr->ip, sizeof(metrics[i].ip) - 1);
        metrics[i].port       = curr->port;
        metrics[i].role       = curr->role;
        metrics[i].cpu_usage  = curr->metrics.cpu_usage;
        metrics[i].mem_usage  = curr->metrics.ram_usage;
        metrics[i].mem_used_mb  = curr->metrics.ram_used_mb;
        metrics[i].disk_usage   = curr->metrics.disk_usage;
        metrics[i].disk_used_mb = curr->metrics.disk_used_mb;
        metrics[i].queue_len    = curr->metrics.queue_len;
        metrics[i].score        = curr->metrics.score;
        metrics[i].load_avg[0]  = curr->metrics.load_avg[0];
        metrics[i].load_avg[1]  = curr->metrics.load_avg[1];
        metrics[i].load_avg[2]  = curr->metrics.load_avg[2];
        metrics[i].cpu_cores            = curr->hardware.cpu_cores;
        metrics[i].cpu_threads_per_core = curr->hardware.cpu_threads_per_core;
        metrics[i].cpu_freq_mhz         = curr->hardware.cpu_freq_mhz;
        strncpy(metrics[i].cpu_model, curr->hardware.cpu_model, sizeof(metrics[i].cpu_model) - 1);
        metrics[i].mem_total_mb  = curr->hardware.ram_total_mb;
        metrics[i].disk_total_mb = curr->hardware.disk_total_gb;
        strncpy(metrics[i].disk_mount, curr->hardware.disk_mount, sizeof(metrics[i].disk_mount) - 1);
        strncpy(metrics[i].network_iface, curr->hardware.network_iface, sizeof(metrics[i].network_iface) - 1);
        metrics[i].timestamp = curr->last_heartbeat;
        /* Encode NodeStatus into active_connections (0=ACTIF,1=SUSPECT,2=EN_PANNE,...) */
        metrics[i].active_connections = (int)curr->status;
        i++;
    }

    pthread_mutex_unlock(&g_node_table.lock);
    return metrics;
}

void *get_all_xtics(void *arg) {
    (void)arg;
    create_mq("ALL_NODES", sizeof(queued_message));
    map_entry *mq = find_by_msg_type("ALL_NODES");
    if (!mq) return NULL;

    queued_message qmsg;
    printf("[Controller] ALL_NODES handler started\n");
    while (1) {
        ssize_t ret = msgrcv(mq->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT);
        if (ret == -1) { usleep(100000); continue; }

        char reply_ip[16];
        strncpy(reply_ip, qmsg.sender_ip, sizeof(reply_ip) - 1);
        reply_ip[sizeof(reply_ip) - 1] = '\0';
        int reply_port = qmsg.sender_port;
        printf("[Controller] ALL_NODES query from %s:%d\n", reply_ip, reply_port);

        MachineMetrics *metrics = get_all_cluster_nodes();
        int count = 0;
        if (metrics)
            while (strlen(metrics[count].uuid) > 0) count++;

        /* Append the controller's own entry */
        int total = count + 1;
        MachineMetrics *all = realloc(metrics, (total + 1) * sizeof(MachineMetrics));
        if (!all) { free(metrics); continue; }
        memset(&all[count], 0, sizeof(MachineMetrics));
        strncpy(all[count].uuid, get_agent_uuid(), sizeof(all[count].uuid) - 1);
        char ctrl_iface[16] = {0};
        load_network_interface(ctrl_iface, sizeof(ctrl_iface));
        get_local_ip(all[count].ip, sizeof(all[count].ip), ctrl_iface);
        all[count].port             = 9000;
        all[count].role             = ROLE_CONTROLLER;
        all[count].active_connections = NODE_ACTIF;
        all[count].timestamp        = time(NULL);
        memset(&all[total], 0, sizeof(MachineMetrics)); /* sentinel */

        size_t payload_size = total * sizeof(MachineMetrics);
        message_t *resp = malloc(sizeof(message_t) + payload_size);
        if (!resp) { free(all); continue; }
        memset(resp, 0, sizeof(message_t) + payload_size);
        strncpy(resp->type, qmsg.recv_type, sizeof(resp->type) - 1);
        resp->size = payload_size;
        memcpy(resp->data, all, payload_size);
        free(all);

        send_msg(reply_ip, reply_port, "outgoing", resp);
        free(resp);
    }
    return NULL;
}

void * get_machine_xtics(void * arg){
    
    (void)arg;
    create_mq("NODES", sizeof(queued_message));
    map_entry * node_query_mq = find_by_msg_type("NODES");
    if (!node_query_mq) return NULL;

    queued_message qmsg;
    printf("Alive and kickin\n");
    while(1){
        ssize_t ret = msgrcv(node_query_mq->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        //printf("recieved nothing in %d \n",node_query_mq->queue_id);
       
        if (ret == -1) {
            usleep(100000);
            continue;
        }

        // Use sender_ip and sender_port from the message header
        // These are set automatically by the network agent on the sender's side
        char reply_ip[16];
        strncpy(reply_ip, qmsg.sender_ip, sizeof(reply_ip) - 1);
        reply_ip[sizeof(reply_ip) - 1] = '\0';
        int reply_port = qmsg.sender_port;
        
        printf("[Controller] NODES query received from %s:%d\n", reply_ip, reply_port);
       
        MachineMetrics * metrics = get_all_active_workers();
        int count = 0;
        if (metrics) {
            while (strlen(metrics[count].uuid) > 0) {
                count++;
            }
        }
       
        size_t payload_size = count * sizeof(MachineMetrics);
        size_t total_size = sizeof(message_t) + payload_size;
       
        message_t * resp = malloc(total_size);
        if (!resp) {
            free(metrics);
            continue;
        }
        memset(resp, 0, total_size);
        strncpy(resp->type, qmsg.recv_type, sizeof(resp->type) - 1);
        strncpy(resp->recv_type, qmsg.recv_type, sizeof(resp->recv_type) - 1);
        resp->size = payload_size;
        if (payload_size > 0) {
            memcpy(resp->data, metrics, payload_size);
        }
        free(metrics);

        send_msg(reply_ip, reply_port, "outgoing", resp);
        free(resp);
    }
    return NULL;
}

void print_machine_metrics(const MachineMetrics *m)
{
    if (m == NULL) {
        printf("MachineMetrics is NULL\n");
        return;
    }

    printf("========================================\n");
    printf("         MACHINE METRICS REPORT         \n");
    printf("========================================\n");

    printf("UUID                : %s\n", m->uuid);
    printf("IP Address          : %s\n", m->ip);
    printf("Port                : %d\n", m->port);
    printf("Message Type        : %d\n", m->type);
    printf("Role                : %d\n", m->role);

    printf("\n========== CPU ==========\n");
    printf("CPU Usage           : %.2f %%\n", m->cpu_usage);
    printf("Load Average        : %.2f %.2f %.2f\n",
           m->load_avg[0],
           m->load_avg[1],
           m->load_avg[2]);

    printf("CPU Cores           : %d\n", m->cpu_cores);
    printf("Threads/Core        : %d\n", m->cpu_threads_per_core);
    printf("CPU Frequency       : %.2f MHz\n", m->cpu_freq_mhz);
    printf("CPU Model           : %s\n", m->cpu_model);

    printf("\n========== MEMORY ==========\n");
    printf("Memory Usage        : %.2f %%\n", m->mem_usage);
    printf("Memory Available    : %.2f MB\n", m->mem_available_mb);
    printf("Memory Used         : %ld MB\n", m->mem_used_mb);
    printf("Memory Total        : %ld MB\n", m->mem_total_mb);

    printf("\n========== DISK ==========\n");
    printf("Disk Usage          : %.2f %%\n", m->disk_usage);
    printf("Disk Used           : %ld MB\n", m->disk_used_mb);
    printf("Disk Total          : %ld MB\n", m->disk_total_mb);
    printf("Disk Mount          : %s\n", m->disk_mount);

    printf("\n========== NETWORK ==========\n");
    printf("Bandwidth           : %.2f Mbps\n",
           m->network_bandwidth_mbps);

    printf("Active Connections  : %d\n", m->active_connections);
    printf("Network Interface   : %s\n", m->network_iface);

    printf("\n========== SYSTEM ==========\n");
    printf("Active Processes    : %d\n", m->active_processes);
    printf("Context Switch Rate : %.2f\n", m->context_switch_rate);
    printf("Uptime              : %ld seconds\n", m->uptime_seconds);

    printf("\n========== COMPUTED ==========\n");
    printf("Queue Length        : %d\n", m->queue_len);
    printf("Score               : %.2f\n", m->score);
    printf("Overloaded          : %s\n",
           m->is_overloaded ? "YES" : "NO");

    printf("\n========== TIMESTAMP ==========\n");
    printf("Timestamp           : %s", ctime(&m->timestamp));

    printf("========================================\n");
}



/**
 * Met à jour le heartbeat d'un nœud lors de la réception d'un MSG_HEARTBEAT.
 * Met simplement à jour le timestamp et vérifie la disponibilité.
 */
static void update_heartbeat(MachineHeartbeat* hb, const char* sender_ip, int sender_port) {
    if (!hb || strlen(hb->uuid) == 0) return;
    pthread_mutex_lock(&g_node_table.lock);

    NodeInfo* node = node_table_find(&g_node_table, hb->uuid);
    if (!node) {
        printf("[HEARTBEAT] ⚠ Heartbeat from unknown node %s, auto-registering...\n", hb->uuid);
        node = node_table_add(&g_node_table, hb->uuid, sender_ip, sender_port);
        if (!node) {
            pthread_mutex_unlock(&g_node_table.lock);
            return;
        }
        node->role = hb->role;
    }
    
    // Update only status, role, and heartbeat timestamp (do not overwrite IP/port for already registered nodes)
    node->status = NODE_ACTIF;
    node->last_heartbeat = time(NULL);
    if (hb->role != ROLE_UNKNOWN) {
        node->role = hb->role;
    }

    printf("[HB] %.8s... %s\n", hb->uuid, node->ip);
    pthread_mutex_unlock(&g_node_table.lock);
}

/*
 * Recomputes scores for all nodes using the cluster-relative formula.
 * C^cpu = cores * threads * freq * (1 - cpu%/100) * 1/(1 + load1m)
 * C^ram = mem_available_mb
 * C^net = network_bandwidth_mbps
 * C^queue = 1 / (1 + queue_len)
 * score = 0 if overloaded, else α*norm_cpu + β*norm_ram + γ*norm_net + δ*queue
 * where norm_X = X / max_X across all active nodes  (α=0.5 β=0.3 γ=0.1 δ=0.1)
 */
static void recompute_all_scores(void) {
    pthread_mutex_lock(&g_node_table.lock);

    float max_cpu = 0.0f, max_ram = 0.0f, max_net = 0.0f;

    for (NodeInfo *n = g_node_table.head; n; n = n->next) {
        if (n->status == NODE_EN_PANNE) continue;

        float c_cpu = n->hardware.cpu_cores
                    * n->hardware.cpu_threads_per_core
                    * n->hardware.cpu_freq_mhz
                    * (1.0f - n->metrics.cpu_usage / 100.0f)
                    * (1.0f / (1.0f + n->metrics.load_avg[0]));
        float c_ram = n->metrics.mem_available_mb;
        float c_net = n->metrics.network_bandwidth_mbps;

        if (c_cpu > max_cpu) max_cpu = c_cpu;
        if (c_ram > max_ram) max_ram = c_ram;
        if (c_net > max_net) max_net = c_net;
    }

    for (NodeInfo *n = g_node_table.head; n; n = n->next) {
        if (n->metrics.is_overloaded) {
            n->metrics.score = 0.0f;
            continue;
        }

        float c_cpu = n->hardware.cpu_cores
                    * n->hardware.cpu_threads_per_core
                    * n->hardware.cpu_freq_mhz
                    * (1.0f - n->metrics.cpu_usage / 100.0f)
                    * (1.0f / (1.0f + n->metrics.load_avg[0]));
        float c_ram = n->metrics.mem_available_mb;
        float c_net = n->metrics.network_bandwidth_mbps;
        float c_queue = 1.0f / (1.0f + (float)n->metrics.queue_len);

        float norm_cpu   = (max_cpu > 0.0f) ? c_cpu / max_cpu : 0.0f;
        float norm_ram   = (max_ram > 0.0f) ? c_ram / max_ram : 0.0f;
        float norm_net   = (max_net > 0.0f) ? c_net / max_net : 0.0f;

        n->metrics.score = 0.5f * norm_cpu
                         + 0.3f * norm_ram
                         + 0.1f * norm_net
                         + 0.1f * c_queue;
    }

    pthread_mutex_unlock(&g_node_table.lock);
}

/**
 * Met à jour les métriques d'un nœud lors de la réception d'un MSG_STATECAPTURE.
 */
static void update_metrics(MachineMetrics* msg) {
    if (!msg || strlen(msg->uuid) == 0) return;
    pthread_mutex_lock(&g_node_table.lock);

;
    NodeInfo* node = node_table_find(&g_node_table, msg->uuid);
    if (!node) {
        printf("[StateReceiver] ⚠ Nœud inconnu %s, auto-enregistrement...\n", msg->uuid);
        node = node_table_add(&g_node_table, msg->uuid, msg->ip, msg->port);
        if (!node) {
            pthread_mutex_unlock(&g_node_table.lock);
            return;
        }
    }
    
    // Update node identity (IP/Port might have changed or recovered from stale snapshot)
    strncpy(node->ip, msg->ip, sizeof(node->ip) - 1);
    node->ip[sizeof(node->ip) - 1] = '\0';
    node->port = msg->port;

    // Mise à jour de l'état du noeud
    node->last_heartbeat       = time(NULL);
    node->role                 = msg->role;  // Update role from metrics
    node->metrics.cpu_usage              = msg->cpu_usage;
    node->metrics.ram_usage              = msg->mem_usage;
    node->metrics.ram_used_mb            = msg->mem_used_mb;
    node->metrics.mem_available_mb       = msg->mem_available_mb;
    node->metrics.disk_usage             = msg->disk_usage;
    node->metrics.disk_used_mb           = msg->disk_used_mb;
    node->metrics.queue_len              = msg->queue_len;
    node->metrics.load_avg[0]            = msg->load_avg[0];
    node->metrics.load_avg[1]            = msg->load_avg[1];
    node->metrics.load_avg[2]            = msg->load_avg[2];
    node->metrics.network_bandwidth_mbps = msg->network_bandwidth_mbps;
    node->metrics.is_overloaded          = msg->is_overloaded;

    // On marque simplement le nœud comme actif puisqu'on a reçu un heartbeat
    // (La détection précise de surcharge/panne sera gérée par un autre module)
    node->status = NODE_ACTIF;

    pthread_mutex_unlock(&g_node_table.lock);
    recompute_all_scores();
}

/**
 * Initialise le hardware d'un nœud lors de la réception d'un MSG_STATECAPTURE_INIT.
 */
static void init_metrics(MachineMetrics* msg) {
    if (!msg || strlen(msg->uuid) == 0) return;
    pthread_mutex_lock(&g_node_table.lock);



    
    NodeInfo* node = node_table_find(&g_node_table, msg->uuid);
    if (!node) {
        printf("[StateReceiver] ⚠ Nœud inconnu %s, auto-enregistrement...\n", msg->uuid);
        node = node_table_add(&g_node_table, msg->uuid, msg->ip, msg->port);
        if (!node) {
            pthread_mutex_unlock(&g_node_table.lock);
            return;
        }
        node->role = msg->role;  // Set role for newly added node
    } else {
        node->role = msg->role;  // Update role from heartbeat_init
    }

    // Update node identity
    strncpy(node->ip, msg->ip, sizeof(node->ip) - 1);
    node->ip[sizeof(node->ip) - 1] = '\0';
    node->port = msg->port;

    // Mise à jour des informations matérielles si pas encore initialisées
    if (!node->hardware.initialized) {
        node->hardware.cpu_cores            = msg->cpu_cores;
        node->hardware.cpu_threads_per_core = msg->cpu_threads_per_core;
        node->hardware.cpu_freq_mhz         = msg->cpu_freq_mhz;
        node->hardware.ram_total_mb         = msg->mem_total_mb;
        node->hardware.disk_total_gb        = msg->disk_total_mb;
        strncpy(node->hardware.cpu_model,
                msg->cpu_model,     sizeof(node->hardware.cpu_model)     - 1);
        strncpy(node->hardware.disk_mount,
                msg->disk_mount,    sizeof(node->hardware.disk_mount)    - 1);
        strncpy(node->hardware.network_iface,
                msg->network_iface, sizeof(node->hardware.network_iface) - 1);
        node->hardware.initialized = 1;

        printf("[StateReceiver] ✓ Hardware nœud %s : %d cœurs %.0fMHz %ldMo RAM\n",
               node->uuid, node->hardware.cpu_cores,
               node->hardware.cpu_freq_mhz, node->hardware.ram_total_mb);
    }

    // Mise à jour classique de type heartbeat
    node->last_heartbeat    = time(NULL);
    node->metrics.cpu_usage = msg->cpu_usage;
    node->metrics.ram_usage = msg->mem_usage;
    node->metrics.queue_len = msg->queue_len;
    node->metrics.score     = msg->score;
    node->status            = NODE_ACTIF;

    pthread_mutex_unlock(&g_node_table.lock);
}




void * statecapture_init_func(void * arg){
        printf("[STATECAPTURE_INIT] Thread started\n");
        map_entry * statecapture_init_entry=(map_entry * )arg;
        queued_message qmsg;
        while(1){
            ssize_t ret = msgrcv(statecapture_init_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        if (ret == -1) {
            usleep(100000);   // 100ms — pas de message, on repoll
            continue;
        }

        // Le payload du network_agent est dans qmsg.data
        MachineMetrics *msg = (MachineMetrics *)qmsg.data;
        init_metrics(msg);
        }


}

void * heartbeat_receiver_func(void * arg){
        printf("[HEARTBEAT] Thread started\n");
        map_entry * heartbeat_entry = (map_entry *)arg;
        queued_message qmsg;
        
        while(1){
            ssize_t ret = msgrcv(heartbeat_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
            if (ret == -1) {
                usleep(100000);   // 100ms — pas de message, on repoll
                continue;
            }

            // Le payload du network_agent est dans qmsg.data
            MachineHeartbeat *hb = (MachineHeartbeat *)qmsg.data;
            update_heartbeat(hb, qmsg.sender_ip, qmsg.sender_port);
            }
        
        return NULL;
}

void * statecapture_func(void * arg){
        printf("[STATECAPTURE] Thread started\n");
        map_entry * statecapture_entry=(map_entry * )arg;
        queued_message qmsg;
        while(1){
            ssize_t ret = msgrcv(statecapture_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        if (ret == -1) {
            usleep(100000);   // 100ms — pas de message, on repoll
            continue;
        }

        // Le payload du network_agent est dans qmsg.data
        MachineMetrics *msg = (MachineMetrics *)qmsg.data;
        update_metrics(msg);
        }
        return NULL;
}


void * hello_func(void * arg){

    printf("[HELLO] Thread started\n");
    map_entry * hello_entry=(map_entry * )arg;
    queued_message qmsg;
    
    while(1){
        ssize_t ret = msgrcv(hello_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                         1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        if (ret == -1) {
            usleep(100000);   // 100ms — pas de message, on repoll
            continue;
        }

        // Ignore if this is a reply (starts with IP:) so we don't infinitely reply to a reply
        if (strncmp(qmsg.data, "IP:", 3) == 0) continue;

        // Le payload du network_agent est dans qmsg.data
        MachineMetrics *msg = (MachineMetrics *)qmsg.data;
        register_node(msg);
        printf("[HELLO] Received HELLO message from %s\n", msg->uuid);
        
        // Reply with our IP dynamically
        char iface[64] = {0};
        char my_ip[16] = {0};
        
        load_network_interface(iface, sizeof(iface));
        get_local_ip(my_ip, sizeof(my_ip), iface);
        message_t *reply = malloc(sizeof(message_t) + 64);
        strcpy(reply->type, HELLO_TYPE);
        strcpy(reply->recv_type, "");
        sprintf(reply->data, "IP:%s", my_ip);
        reply->size = strlen(reply->data) + 1;
        
        printf("Replying to HELLO with controller IP: %s sent to agent at %s:%d\n", my_ip, msg->ip, msg->port);
        // Reply directly to the agent's UDP port (msg->port + 1)
        send_broadcast_message(msg->port + 1, reply);
        free(reply);
    }
    return NULL;
}

void *node_log_receiver_func(void *arg) {
    (void)arg;
    create_mq(NODE_LOG_TYPE, sizeof(queued_message));
    map_entry *mq = find_by_msg_type(NODE_LOG_TYPE);
    if (!mq) return NULL;

    mkdir("node_logs", 0777);
    printf("[Controller] Node log receiver started\n");

    queued_message qmsg;
    while (1) {
        ssize_t ret = msgrcv(mq->queue_id, &qmsg,
                             sizeof(qmsg) - sizeof(long), 1L, IPC_NOWAIT);
        if (ret == -1) { usleep(100000); continue; }

        node_log_t *log = (node_log_t *)qmsg.data;

        char path[256];
        snprintf(path, sizeof(path), "node_logs/%s.log", log->uuid);
        FILE *f = fopen(path, "w"); /* overwrite — keep only the latest snapshot */
        if (f) {
            fwrite(log->log_content, 1, log->log_size, f);
            fclose(f);
            printf("[Controller] Node log updated: %s (%u bytes)\n",
                   log->uuid, log->log_size);
        }
    }
    return NULL;
}

void *get_node_log_func(void *arg) {
    (void)arg;
    create_mq(GET_NODE_LOG_TYPE, sizeof(queued_message));
    map_entry *mq = find_by_msg_type(GET_NODE_LOG_TYPE);
    if (!mq) return NULL;

    printf("[Controller] Get-node-log handler started\n");

    queued_message qmsg;
    while (1) {
        ssize_t ret = msgrcv(mq->queue_id, &qmsg,
                             sizeof(qmsg) - sizeof(long), 1L, IPC_NOWAIT);
        if (ret == -1) { usleep(100000); continue; }

        get_node_log_req_t *req = (get_node_log_req_t *)qmsg.data;

        node_log_t resp_log;
        memset(&resp_log, 0, sizeof(resp_log));
        strncpy(resp_log.uuid, req->node_uuid, sizeof(resp_log.uuid) - 1);

        char path[256];
        snprintf(path, sizeof(path), "node_logs/%s.log", req->node_uuid);
        FILE *f = fopen(path, "r");
        if (f) {
            resp_log.log_size = (uint32_t)fread(resp_log.log_content, 1,
                                                 sizeof(resp_log.log_content) - 1, f);
            fclose(f);
        }

        size_t pkt_size = sizeof(message_t) + sizeof(node_log_t);
        message_t *pkt = malloc(pkt_size);
        if (!pkt) continue;
        memset(pkt, 0, pkt_size);
        pkt->mq_type = 1;
        strcpy(pkt->type, qmsg.recv_type);
        pkt->size = sizeof(node_log_t);
        memcpy(pkt->data, &resp_log, sizeof(node_log_t));
        send_msg(qmsg.sender_ip, qmsg.sender_port, "outgoing", pkt);
        free(pkt);
    }
    return NULL;
}

void *log_relay_func(void *arg) {
    map_entry *log_entry = (map_entry *)arg;
    queued_message qmsg;

    printf("[Controller] Log relay thread started\n");

    while (1) {
        ssize_t ret = msgrcv(log_entry->queue_id, &qmsg,
                             sizeof(qmsg) - sizeof(long), 1L, IPC_NOWAIT);
        if (ret == -1) {
            usleep(100000);
            continue;
        }

        /* Find an active receptionist in the node table */
        char recep_ip[16] = "";
        int  recep_port   = 0;

        pthread_mutex_lock(&g_node_table.lock);
        for (NodeInfo *n = g_node_table.head; n; n = n->next) {
            if (n->role == ROLE_RECEPTIONIST && n->status != NODE_EN_PANNE) {
                strncpy(recep_ip, n->ip, sizeof(recep_ip) - 1);
                recep_port = n->port;
                break;
            }
        }
        pthread_mutex_unlock(&g_node_table.lock);

        if (recep_port == 0) {
            printf("[Controller] No receptionist connected — log dropped\n");
            continue;
        }

        /* Forward the log message as-is */
        size_t pkt_size = sizeof(message_t) + sizeof(prog_log_t);
        message_t *fwd = malloc(pkt_size);
        if (fwd) {
            memset(fwd, 0, pkt_size);
            fwd->mq_type = 1;
            strcpy(fwd->type, PROG_LOG_TYPE);
            fwd->size = sizeof(prog_log_t);
            memcpy(fwd->data, qmsg.data, sizeof(prog_log_t));
            send_msg(recep_ip, recep_port, "outgoing", fwd);
            free(fwd);
            printf("[Controller] Log forwarded to receptionist %s:%d\n",
                   recep_ip, recep_port);
        }
    }
    return NULL;
}

void * request_master_ip_func(void * arg) {
    map_entry * request_master_ip_entry = (map_entry *)arg;
    queued_message qmsg;
    
    while (1) {
        ssize_t ret = msgrcv(request_master_ip_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        if (ret == -1) {
            usleep(100000);   // 100ms
            continue;
        }

        MasterIPRequest *req = (MasterIPRequest *)qmsg.data;
        
        char master_ip[16] = "";
        char master_uuid[37] = "";
        int master_port = 0;
        
        pthread_mutex_lock(&g_node_table.lock);
        for (NodeInfo* node = g_node_table.head; node; node = node->next) {
            if (node->role == ROLE_MASTER && node->status != NODE_EN_PANNE) {
                strncpy(master_ip, node->ip, sizeof(master_ip) - 1);
                strncpy(master_uuid, node->uuid, sizeof(master_uuid) - 1);
                master_port = node->port;
                break;
            }
        }
        pthread_mutex_unlock(&g_node_table.lock);
        
        MasterIPResponse resp;
        memset(&resp, 0, sizeof(MasterIPResponse));
        if (master_port != 0) {
            strncpy(resp.master_uuid, master_uuid, sizeof(resp.master_uuid) - 1);
            strncpy(resp.master_ip, master_ip, sizeof(resp.master_ip) - 1);
            resp.master_port = master_port;
        } else {
            strncpy(resp.master_uuid, "NONE", sizeof(resp.master_uuid) - 1);
            strncpy(resp.master_ip, "NONE", sizeof(resp.master_ip) - 1);
            resp.master_port = 0;
        }
        
        message_t *reply = malloc(sizeof(message_t) + sizeof(MasterIPResponse));
        if (reply) {
            memset(reply, 0, sizeof(message_t) + sizeof(MasterIPResponse));
            strcpy(reply->type, PROVIDE_MASTER_IP_TYPE);
            strcpy(reply->recv_type, "");
            reply->size = sizeof(MasterIPResponse);
            memcpy(reply->data, &resp, sizeof(MasterIPResponse));
            
            send_msg(req->receptionist_ip, req->receptionist_port, "outgoing", reply);
            free(reply);
        }
    }
    return NULL;
}

// ════════════════════════════════════════════════════════════════════════════
//  HEARTBEAT MONITOR THREAD - Detects stale/failed nodes
// ════════════════════════════════════════════════════════════════════════════
// Runs periodically and updates node status based on heartbeat timeout

#define HEARTBEAT_MONITOR_INTERVAL 1  // Check every 1 second

static atomic_int heartbeat_monitor_running = 0;

void* heartbeat_monitor_thread_run(void* arg) {
    (void)arg;
    printf("[HEARTBEAT_MONITOR] Thread started\n");
    
    while (atomic_load(&heartbeat_monitor_running)) {
        time_t now = time(NULL);
        pthread_mutex_lock(&g_node_table.lock);
        
        for (NodeInfo* node = g_node_table.head; node; node = node->next) {
            time_t silence_duration = now - node->last_heartbeat;
            
            // Determine new status based on silence duration
            NodeStatus new_status = NODE_ACTIF;
            
            if (silence_duration > T_FAILED_SEC) {
                new_status = NODE_EN_PANNE;  // > 8s = confirmed failed
            } else if (silence_duration > T_SUSPECT_SEC) {
                new_status = NODE_SUSPECT;   // > 4s = suspect
            }
            
            // Log status changes
            if (new_status != node->status) {
                const char* status_str[] = {"ACTIF", "SUSPECT", "EN_PANNE", "SURCHARGE", "EN_MAINTENANCE"};
                printf("[HEARTBEAT_MONITOR] Node %s status change: %s → %s (silence: %lds)\n",
                       node->uuid, status_str[node->status], status_str[new_status], silence_duration);
                node->status = new_status;
            }
        }
        
        pthread_mutex_unlock(&g_node_table.lock);
        sleep(HEARTBEAT_MONITOR_INTERVAL);
    }
    
    printf("[HEARTBEAT_MONITOR] Thread stopped\n");
    return NULL;
}

void heartbeat_monitor_stop(void) {
    atomic_store(&heartbeat_monitor_running, 0);
}

// ════════════════════════════════════════════════════════════════════════════
//  BOUCLE DU THREAD RECEIVER
// ════════════════════════════════════════════════════════════════════════════

static atomic_int  receiver_running = 0;

/**
 * Fonction principale du thread de réception d'état.
 */
void* state_receiver_thread_run(void* arg) {
    (void)arg;

    printf("[StateReceiver] Thread started\n");
    
    // Initialisation de la table globale
    node_table_init(&g_node_table);
    
    // (Note: La lecture depuis le disque au démarrage n'est pas gérée pour le moment. 
    //  On suppose le contrôleur toujours actif, et on reconstruit la RAM en temps réel 
    //  à partir des messages reçus).


    // Ouverture de la file de messages IPC via le network_agent
 



    
    char * mq_statecapture_init_str = create_mq(STATECAPTURE_INIT_TYPE, sizeof(queued_message));
    char * mq_statecapture_str = create_mq(STATECAPTURE_TYPE, sizeof(queued_message));
    char * mq_hello_str = create_mq(HELLO_TYPE, sizeof(queued_message));
    char * mq_heartbeat_str = create_mq(HB_TYPE, sizeof(queued_message));
    char * mq_request_master_ip_str = create_mq(REQUEST_MASTER_IP_TYPE, sizeof(queued_message));
    char * mq_prog_log_str = create_mq(PROG_LOG_TYPE, sizeof(queued_message));
    (void)mq_prog_log_str;

    if (mq_statecapture_init_str == NULL) {
        perror("[StateReceiver] create_mq()");
        return NULL;
    }
    
    map_entry * statecapture_init_entry = find_by_msg_type(mq_statecapture_init_str);
    map_entry * statecapture_entry = find_by_msg_type(mq_statecapture_str);
    map_entry * hello_entry = find_by_msg_type(mq_hello_str);
    map_entry * heartbeat_entry = find_by_msg_type(mq_heartbeat_str);
    map_entry * request_master_ip_entry = find_by_msg_type(mq_request_master_ip_str);
    map_entry * prog_log_entry = find_by_msg_type(mq_prog_log_str);
    
    if (!statecapture_init_entry) {
        fprintf(stderr, "[StateReceiver] STATECAPTURE_INIT queue not created \n");
        return NULL;
    }

    if (!statecapture_entry) {
        fprintf(stderr, "[StateReceiver] STATECAPTURE queue not created \n");
        return NULL;
    }

    if (!hello_entry) {
        fprintf(stderr, "[StateReceiver] HELLO queue not created \n");
        return NULL;
    }

    if (!heartbeat_entry) {
        fprintf(stderr, "[StateReceiver] HEARTBEAT queue not created \n");
        return NULL;
    }

    if (!request_master_ip_entry) {
        fprintf(stderr, "[StateReceiver] REQUEST_MASTER_IP queue not created \n");
        return NULL;
    }
    
    pthread_t statecapture_init_thread;
    pthread_t statecapture_thread;
    pthread_t hello_thread;
    pthread_t get_xtics_thread;
    pthread_t get_all_xtics_thread;
    pthread_t heartbeat_thread;
    pthread_t heartbeat_monitor_thread;
    pthread_t request_master_ip_thread;
    pthread_t log_relay_thread;
    pthread_t node_log_receiver_thread;
    pthread_t get_node_log_thread;

    pthread_create(&statecapture_init_thread, NULL, statecapture_init_func, (void *)statecapture_init_entry);
    pthread_create(&statecapture_thread, NULL, statecapture_func, (void *)statecapture_entry);
    pthread_create(&hello_thread, NULL, hello_func, (void *)hello_entry);
    pthread_create(&get_xtics_thread, NULL, get_machine_xtics, NULL);
    pthread_create(&get_all_xtics_thread, NULL, get_all_xtics, NULL);
    pthread_create(&heartbeat_thread, NULL, heartbeat_receiver_func, (void *)heartbeat_entry);
    pthread_create(&request_master_ip_thread, NULL, request_master_ip_func, (void *)request_master_ip_entry);
    pthread_create(&log_relay_thread, NULL, log_relay_func, (void *)prog_log_entry);
    pthread_create(&node_log_receiver_thread, NULL, node_log_receiver_func, NULL);
    pthread_create(&get_node_log_thread, NULL, get_node_log_func, NULL);
    
    // Start heartbeat monitor thread
    atomic_store(&heartbeat_monitor_running, 1);
    pthread_create(&heartbeat_monitor_thread, NULL, heartbeat_monitor_thread_run, NULL);


    
    
    atomic_store(&receiver_running, 1);

    while (atomic_load(&receiver_running)) {
        usleep(100000);
        
    }
    //kill threads here


    printf("[StateReceiver] Arrêté proprement\n");
    return NULL;
}

// ════════════════════════════════════════════════════════════════════════════
//  API PUBLIQUE
// ════════════════════════════════════════════════════════════════════════════

void state_receiver_stop(void) {
    atomic_store(&receiver_running, 0);
    heartbeat_monitor_stop();  // Stop the heartbeat monitor thread
    // pthread_join is done in init.c (the caller who created this thread)
    
    node_table_destroy(&g_node_table);
}