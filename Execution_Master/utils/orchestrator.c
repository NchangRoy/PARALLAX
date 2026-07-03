#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "orchestrator.h"
#include "../../parallax/state_message.h" // Contient la structure MachineMetrics
#include "../../parallax/parallax_param.h"

#define MOCK_NODE_COUNT 4

/**
 * Génère un tableau de fausses métriques de machines pour simuler le cluster.
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
        metrics[i].disk_total_mb = 512 * 1024;
        strcpy(metrics[i].disk_mount, "/");
        strcpy(metrics[i].network_iface, "lo");

        // Métriques dynamiques (simulées)
        metrics[i].cpu_usage = 0.10f * i;
        metrics[i].mem_usage = 0.20f;
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

/* Helper to serialize a ParallaxParam list plus parameter data payloads */
static void *serialize_params(ParallaxParam *params, int param_count, size_t *out_size) {
    size_t total_size = sizeof(int);
    for (int i = 0; i < param_count; i++) {
        total_size += sizeof(ParallaxParam);
        if (params[i].size > 0 && params[i].data) {
            total_size += params[i].size;
        }
    }

    void *buf = malloc(total_size);
    if (!buf) {
        *out_size = 0;
        return NULL;
    }

    char *ptr = (char *)buf;
    // 1. Write param count
    memcpy(ptr, &param_count, sizeof(int));
    ptr += sizeof(int);

    // 2. Write metadata and data for each param
    for (int i = 0; i < param_count; i++) {
        memcpy(ptr, &params[i], sizeof(ParallaxParam));
        ptr += sizeof(ParallaxParam);
        if (params[i].size > 0 && params[i].data) {
            memcpy(ptr, params[i].data, params[i].size);
            ptr += params[i].size;
        }
    }

    *out_size = total_size;
    return buf;
}

/**
 * Crée une répartition de tâches (task_assignment) équilibrée par nœud.
 * Segmente et sérialise les paramètres selon leur type de distribution.
 */
task_assignment *create_assignments(
    ParallaxParam *params,
    int param_count,
    const char *function,
    MachineMetrics *metrics,
    int node_count
) {
    if (!params || param_count <= 0 || !metrics || node_count <= 0) {
        return NULL;
    }

    // 1. Locate SCATTER param and its own SIZE_OF companion specifically.
    //
    // A submission can have more than one SIZE_OF param (e.g. one pointer
    // that's SCATTERed plus another that's BROADCAST, each with its own
    // size field - see test_matvec_dist.c). Only the SCATTER param's own
    // companion needs adjusting per node (to the post-split chunk size);
    // every other SIZE_OF param belongs to a BROADCAST pointer, whose data
    // is never split, so its value must stay exactly as the caller set it.
    //
    // The parser (Parser/parser.cpp) always places a SIZE_OF param
    // immediately after the pointer it describes, so the SCATTER's own
    // companion - if any - is simply the next slot.
    int scatter_idx = -1;
    for (int p = 0; p < param_count; p++) {
        if (params[p].distribution == PARALLAX_SCATTER) {
            scatter_idx = p;
            break;
        }
    }

    if (scatter_idx < 0) {
        fprintf(stderr, "[Orchestrator] Error: No SCATTER parameter found!\n");
        return NULL;
    }

    int size_idx = -1;
    if (scatter_idx + 1 < param_count &&
        params[scatter_idx + 1].distribution == PARALLAX_SIZE_OF) {
        size_idx = scatter_idx + 1;
    }

    void *scatter_data = params[scatter_idx].data;
    size_t scatter_size = params[scatter_idx].size;
    if (!scatter_data || scatter_size <= 0) {
        fprintf(stderr, "[Orchestrator] Error: SCATTER parameter has no data or size!\n");
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
        float available_cpu = 1.0f - metrics[i].cpu_usage;
        float available_ram = 1.0f - metrics[i].mem_usage;

        weights[i] = (available_cpu * 0.6f) + (available_ram * 0.4f);
        if (weights[i] < 0.01f) {
            weights[i] = 0.01f;
        }
        total_weight += weights[i];
    }

    // Determine element size for the SCATTER pointer
    size_t elem_size = sizeof(int); // default
    if (strstr(params[scatter_idx].type_name, "double")) {
        elem_size = sizeof(double);
    } else if (strstr(params[scatter_idx].type_name, "float")) {
        elem_size = sizeof(float);
    } else if (strstr(params[scatter_idx].type_name, "char")) {
        elem_size = sizeof(char);
    } else if (strstr(params[scatter_idx].type_name, "short")) {
        elem_size = sizeof(short);
    } else if (strstr(params[scatter_idx].type_name, "long")) {
        elem_size = sizeof(long);
    }

    size_t total_elements = scatter_size / elem_size;
    size_t offset_elements = 0;

    // 3. Segment and serialize parameters for each node
    for (int i = 0; i < node_count; i++) {
        float ratio = weights[i] / total_weight;
        size_t portion_elements = (size_t)(total_elements * ratio);

        // Last node takes the remainder
        if (i == node_count - 1) {
            portion_elements = total_elements - offset_elements;
        }

        size_t portion_bytes = portion_elements * elem_size;
        size_t offset_bytes = offset_elements * elem_size;

        // Build worker-specific parameters list
        ParallaxParam *local_params = malloc(sizeof(ParallaxParam) * param_count);
        memcpy(local_params, params, sizeof(ParallaxParam) * param_count);

        // Customize the SCATTER parameter
        local_params[scatter_idx].data = (char *)scatter_data + offset_bytes;
        local_params[scatter_idx].size = portion_bytes;

        // Customize the SIZE_OF parameter if present
        union {
            size_t sz;
            int i;
            long l;
            char bytes[8];
        } comp_val;
        comp_val.sz = portion_bytes;

        if (size_idx >= 0) {
            local_params[size_idx].data = &comp_val;
        }

        // Serialize the local parameters into a single chunk
        size_t serialized_size = 0;
        void *serialized_buf = serialize_params(local_params, param_count, &serialized_size);

        chunk_data *chunk = malloc(sizeof(chunk_data));
        if (chunk) {
            chunk->chunk = serialized_buf;
            chunk->chunk_size = (int)serialized_size;
        }

        task_descriptor *task = malloc(sizeof(task_descriptor));
        if (task) {
            strncpy(task->function_name, function, sizeof(task->function_name) - 1);
            task->function_name[sizeof(task->function_name) - 1] = '\0';
        }

        assignments[i].task = task;
        assignments[i].chunk = chunk;

        worker_node *wn = malloc(sizeof(worker_node));
        if (wn) {
            snprintf(wn->uuid, sizeof(wn->uuid), "%s", metrics[i].uuid);
            snprintf(wn->ip, sizeof(wn->ip), "%s", metrics[i].ip);
            wn->port = metrics[i].port;
            assignments[i].target_node = wn;
        }

        free(local_params);
        offset_elements += portion_elements;
    }

    free(weights);
    return assignments;
}