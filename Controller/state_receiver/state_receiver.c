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
#include "persistence.h"
#include "ms_queue.h"
#include "network_agent.h"


// ══════════════════════════════════════════════════════════════════════════
//  HANDLERS DE MESSAGES
// ════════════════════════════════════════════════════════════════════════════

/**
 * Enregistre un nouveau nœud dans la table globale lors de la réception d'un MSG_HELLO.
 */
static void register_node(MachineMetrics* msg) {
    pthread_mutex_lock(&g_node_table.lock);

    if (node_table_find(&g_node_table, msg->uuid)) {
        printf("[StateReceiver] Nœud %s déjà enregistré, HELLO ignoré\n",
               msg->uuid);
        pthread_mutex_unlock(&g_node_table.lock);
        return;
    }

    NodeInfo* node = node_table_add(&g_node_table, msg->uuid, msg->ip, msg->port);
    if (node)
        printf("[StateReceiver] ✓ Nouveau nœud : uuid=%s ip=%s\n",
               node->uuid, node->ip);

    // Snapshot initial sur disque (le pointeur de noeud est copié pour minimiser la zone critique)
    if (node) {
        NodeInfo snapshot = *node;
        pthread_mutex_unlock(&g_node_table.lock);
        persist_node_snapshot(&snapshot);
    } else {
        pthread_mutex_unlock(&g_node_table.lock);
    }
}

/**
 * Met à jour les métriques d'un nœud lors de la réception d'un MSG_HEARTBEAT.
 */
static void update_heartbeat(MachineMetrics* msg) {
    pthread_mutex_lock(&g_node_table.lock);

    NodeInfo* node = node_table_find(&g_node_table, msg->uuid);
    if (!node) {
        printf("[StateReceiver] ⚠ Heartbeat nœud inconnu %s\n", msg->uuid);
        pthread_mutex_unlock(&g_node_table.lock);
        return;
    }

    // Mise à jour de l'état du noeud
    node->last_heartbeat       = time(NULL);
    node->metrics.cpu_usage    = msg->cpu_usage;
    node->metrics.ram_usage    = msg->mem_usage;
    node->metrics.ram_used_mb  = msg->mem_used_mb;
    node->metrics.disk_usage   = msg->disk_usage;
    node->metrics.disk_used_mb = msg->disk_used_mb;
    node->metrics.queue_len    = msg->queue_len;
    node->metrics.score        = msg->score;
    node->metrics.load_avg[0]  = msg->load_avg[0];
    node->metrics.load_avg[1]  = msg->load_avg[1];
    node->metrics.load_avg[2]  = msg->load_avg[2];

    // On marque simplement le nœud comme actif puisqu'on a reçu un heartbeat
    // (La détection précise de surcharge/panne sera gérée par un autre module)
    node->status = NODE_ACTIF;

    // Copie locale pour sortir de la section critique avant l'I/O asynchrone
    char        uuid_copy[64];
    time_t      ts    = node->last_heartbeat;
    NodeMetrics mcopy = node->metrics;
    strncpy(uuid_copy, node->uuid, sizeof(uuid_copy) - 1);

    pthread_mutex_unlock(&g_node_table.lock);

    // Buffer RAM — flush asynchrone par le thread flusher
    metrics_buf_push(uuid_copy, ts, &mcopy);
}

/**
 * Initialise le hardware d'un nœud lors de la réception d'un MSG_HEARTBEAT_INIT.
 */
static void init_heartbeat(MachineMetrics* msg) {
    pthread_mutex_lock(&g_node_table.lock);

    NodeInfo* node = node_table_find(&g_node_table, msg->uuid);
    if (!node) {
        printf("[StateReceiver] ⚠ HEARTBEAT_INIT nœud inconnu %s\n", msg->uuid);
        pthread_mutex_unlock(&g_node_table.lock);
        return;
    }

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

    // Copie locale avant de libérer le mutex
    NodeInfo snapshot = *node;
    char        uuid_copy[64];
    time_t      ts    = node->last_heartbeat;
    NodeMetrics mcopy = node->metrics;
    strncpy(uuid_copy, node->uuid, sizeof(uuid_copy) - 1);

    pthread_mutex_unlock(&g_node_table.lock);

    // Snapshot hardware sur disque (une seule fois)
    persist_node_snapshot(&snapshot);

    // Métriques initiales dans le buffer RAM
    metrics_buf_push(uuid_copy, ts, &mcopy);
}




void * heartbeat_init_func(void * arg){
        //parse entry
        map_entry * heartbeat_entry=(map_entry * )arg;
        queued_message qmsg;
        while(1){
            ssize_t ret = msgrcv(heartbeat_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        if (ret == -1) {
            usleep(100000);   // 100ms — pas de message, on repoll
            continue;
        }

        // Le payload du network_agent est dans qmsg.data
        MachineMetrics *msg = (MachineMetrics *)qmsg.data;
        init_heartbeat(msg);
        printf("received a hearbeat\n");
        }
}

void * heartbeat_func(void * arg){
     map_entry * heartbeat_entry=(map_entry * )arg;
        queued_message qmsg;
        while(1){
            ssize_t ret = msgrcv(heartbeat_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        if (ret == -1) {
            usleep(100000);   // 100ms — pas de message, on repoll
            continue;
        }

        // Le payload du network_agent est dans qmsg.data
        MachineMetrics *msg = (MachineMetrics *)qmsg.data;
        update_heartbeat(msg);
        printf("received a hearbeat\n");
        }
}

void * hello_func(void * arg){
     map_entry * heartbeat_entry=(map_entry * )arg;
        queued_message qmsg;
        while(1){
            ssize_t ret = msgrcv(heartbeat_entry->queue_id, &qmsg, sizeof(qmsg) - sizeof(long),
                             1L, IPC_NOWAIT); // NETWORK_AGENT_MTYPE is 1L
        if (ret == -1) {
            usleep(100000);   // 100ms — pas de message, on repoll
            continue;
        }

        // Le payload du network_agent est dans qmsg.data
        MachineMetrics *msg = (MachineMetrics *)qmsg.data;
        register_node(msg);
        printf("received a hearbeat\n");
        }
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
    
    // Initialisation de la table globale
    node_table_init(&g_node_table);
    
    // (Note: La lecture depuis le disque au démarrage n'est pas gérée pour le moment. 
    //  On suppose le contrôleur toujours actif, et on reconstruit la RAM en temps réel 
    //  à partir des messages reçus).

    // Démarre le thread de persistence disque
    flusher_start();

    // Ouverture de la file de messages IPC via le network_agent
    char * mq_heartbeat_init_str = create_mq("MSG_HEARTBEAT_INIT", sizeof(queued_message));
    char * mq_heartbeat_str = create_mq("MSG_HEARTBEAT", sizeof(queued_message));
    char * mq_hello_str = create_mq("MSG_HELLO", sizeof(queued_message));
    



    if (mq_heartbeat_init_str == NULL) {
        perror("[StateReceiver] create_mq()");
        return NULL;
    }
    
    map_entry * hearbeat_init = find_by_msg_type(mq_heartbeat_init_str);
    map_entry * heartbeat=find_by_msg_type(mq_heartbeat_str);
    map_entry * hello=find_by_msg_type(mq_hello_str);
    if (!hearbeat_init) {
        fprintf(stderr, "[StateReceiver] Queue STAT introuvable\n");
        return NULL;
    }
    pthread_t hearbeat_init_thread;
    pthread_t heartbeat_thread;
    pthread_t hello_thread;
    pthread_create(&hearbeat_init_thread,NULL,heartbeat_init_func,(void * )hearbeat_init);
    pthread_create(&heartbeat_thread,NULL,heartbeat_func,(void * )heartbeat);
    pthread_create(&hello_thread,NULL,hello_func,(void * )hello);


    
    
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
    // pthread_join is done in init.c (the caller who created this thread)
    
    flusher_stop();    // flush final + arrêt du thread de persistence
    node_table_destroy(&g_node_table);
}