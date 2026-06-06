#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orchestrator.h"
#include "../../parallax/state_message.h" // Contient la structure MachineMetrics

#define MOCK_NODE_COUNT 4

/**
 * Génère un tableau de fausses métriques de machines pour simuler le cluster.
 * Remplace l'ancien get_node_details().
 */
MachineMetrics *get_mock_machine_metrics() {
    MachineMetrics *metrics = malloc(sizeof(MachineMetrics) * MOCK_NODE_COUNT);
    if (metrics == NULL) {
        return NULL;
    }

    for (int i = 0; i < MOCK_NODE_COUNT; i++) {
        memset(&metrics[i], 0, sizeof(MachineMetrics));

        // Identité de base
        snprintf(metrics[i].uuid, sizeof(metrics[i].uuid), "mock-node-%d", i);
        strcpy(metrics[i].ip, "127.0.0.1");
        metrics[i].port = 8000 + i;
        metrics[i].type = MSG_STATECAPTURE; // Type générique pour les métriques

        // Caractéristiques matérielles fixes
        metrics[i].cpu_cores = 8;
        metrics[i].cpu_threads_per_core = 2;
        metrics[i].cpu_freq_mhz = 3200.0f;
        strcpy(metrics[i].cpu_model, "Mock CPU");
        metrics[i].mem_total_mb = 16000;
        metrics[i].disk_total_mb = 512 * 1024; // Converti en Mo pour correspondre à la structure
        strcpy(metrics[i].disk_mount, "/");
        strcpy(metrics[i].network_iface, "lo");

        // Métriques dynamiques (simulées)
        metrics[i].cpu_usage = 0.10f * i; // Charge CPU croissante d'une machine à l'autre
        metrics[i].mem_usage = 0.20f;     // 20% d'utilisation RAM constante
        metrics[i].mem_used_mb = 3200;
        metrics[i].mem_available_mb = 12800.0f;

        metrics[i].disk_usage = 0.30f;
        metrics[i].disk_used_mb = 120 * 1024;

        metrics[i].queue_len = i;
        metrics[i].score = 1.0f - metrics[i].cpu_usage;
        metrics[i].load_avg[0] = 0.5f;
        metrics[i].load_avg[1] = 0.4f;
        metrics[i].load_avg[2] = 0.3f;
        metrics[i].is_overloaded = (metrics[i].cpu_usage > 0.85f) ? 1 : 0;
        metrics[i].timestamp = time(NULL);
    }

    return metrics;
}

/**
 * Crée une répartition de tâches (task_assignment) équilibrée par nœud.
 * Utilise directement la structure MachineMetrics reçue du StateReceiver.
 */
task_assignment *create_assignments(
    void *data,
    size_t total_size,
    const char *function,
    MachineMetrics *metrics,
    int node_count
) {
    // 1. Validations de sécurité initiales (utilisation exclusive de metrics)
    if (!data || !metrics || node_count <= 0) {
        return NULL;
    }

    task_assignment *assignments = malloc(sizeof(task_assignment) * node_count);
    if (!assignments) {
        return NULL;
    }

    float *weights = malloc(sizeof(float) * node_count);
    if (!weights) {
        free(assignments);
        return NULL;
    }

    float total_weight = 0.0f;

    // 2. Calcul des poids basés sur les métriques dynamiques réelles
    for (int i = 0; i < node_count; i++) {
        // CPU libre = 1.0 - fraction consommée
        float available_cpu = 1.0f - metrics[i].cpu_usage;
        // RAM libre = 1.0 - fraction consommée
        float available_ram = 1.0f - metrics[i].mem_usage;

        /*
         * Moyenne pondérée standard :
         * 60% d'importance attribuée au CPU disponible
         * 40% d'importance attribuée à la RAM disponible
         */
        weights[i] = (available_cpu * 0.6f) + (available_ram * 0.4f);

        // Sécurité pour éviter un poids à zéro si une machine est à genoux
        if (weights[i] < 0.01f) {
            weights[i] = 0.01f;
        }

        total_weight += weights[i];
    }

    /*
     * 3. Découpage du jeu de données
     * IMPORTANT : Le découpage se fait ici par éléments (int) et non par octets directs
     */
    int *base = (int *)data;
    size_t total_elements = total_size / sizeof(int);
    size_t offset = 0;

    for (int i = 0; i < node_count; i++) {
        float ratio = weights[i] / total_weight;

        // Nombre d'éléments entiers attribués à cette machine
        size_t portion = (size_t)(total_elements * ratio);

        // Le dernier nœud ramasse le reliquat pour éviter les pertes dues aux arrondis
        if (i == node_count - 1) {
            portion = total_elements - offset;
        }

        // Construction du Chunk de données
        chunk_data *chunk = malloc(sizeof(chunk_data));
        if (!chunk) {
            continue;
        }
        chunk->chunk = &base[offset];
        chunk->chunk_size = portion * sizeof(int); // Taille convertie en octets pour l'envoi réseau

        // Construction du descripteur de tâche
        task_descriptor *task = malloc(sizeof(task_descriptor));
        if (!task) {
            free(chunk);
            continue;
        }

        strncpy(task->function_name, function, sizeof(task->function_name) - 1);
        task->function_name[sizeof(task->function_name) - 1] = '\0';

        assignments[i].task = task;
        assignments[i].chunk = chunk;

        // Construction de l'adresse de destination (WorkerNode) à partir de MachineMetrics
        worker_node *wn = malloc(sizeof(worker_node));
        if (wn) {
            snprintf(wn->uuid, sizeof(wn->uuid), "%s", metrics[i].uuid);
            snprintf(wn->ip, sizeof(wn->ip), "%s", metrics[i].ip);
            wn->port = metrics[i].port;
            assignments[i].target_node = wn;
        }

        // Avancement de l'index dans le grand tableau de données initial
        offset += portion;
    }

    free(weights);
    return assignments;
}