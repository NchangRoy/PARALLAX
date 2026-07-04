/* =============================================================================
 * parallax_run_local.c
 *
 * A complete end-to-end local simulation of Parallax's distributed execution.
 * It simulates:
 *   1. Master parameter packaging and look-ahead size computation.
 *   2. Orchestrator segmentation and serialization for each node.
 *   3. Compilation of the worker binary with logic.c.
 *   4. Spawning worker processes with the serialized payload over a pipe.
 *   5. Collecting and reducing the final results.
 * ============================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "parallax/parallax_param.h"
#include "Execution_Master/utils/orchestrator.h"

// Define the sum_reduce function
void *sum_reduce(void *a, void *b) {
    if (!a && !b) return NULL;
    long long val_a = a ? atoll((char*)a) : 0;
    long long val_b = b ? atoll((char*)b) : 0;
    char *res = malloc(64);
    sprintf(res, "%lld", val_a + val_b);
    return res;
}

int main(void) {
    printf("[Main] Starting end-to-end Parallax local execution test...\n");

    const int N = 100;
    int *arr = malloc(N * sizeof(int));
    for (int i = 0; i < N; i++) {
        arr[i] = i + 1; // 1..100, expected total = 5050
    }

    // ── Step 1: Build Master Parameter List ──
    ParallaxParam params[2];

    // Param 0: Array to scatter
    params[0].data = arr;
    params[0].size = N * sizeof(int);
    params[0].distribution = PARALLAX_SCATTER;
    params[0].index = 0;
    strcpy(params[0].type_name, "int *");

    // Param 1: Size companion
    size_t total_size = N * sizeof(int);
    params[1].data = &total_size;
    params[1].size = sizeof(total_size);
    params[1].distribution = PARALLAX_SIZE_OF;
    params[1].index = 1;
    strcpy(params[1].type_name, "size_t");

    // ── Step 2: Get Mock Metrics and Create Assignments ──
    int node_count = 2;
    MachineMetrics *metrics = get_mock_machine_metrics();
    if (!metrics) {
        fprintf(stderr, "Failed to get mock metrics\n");
        free(arr);
        return 1;
    }

    printf("[Main] Splitting task across %d nodes using orchestrator...\n", node_count);
    task_assignment *assignments = create_assignments(
        params, 2, "sum_worker", metrics, node_count, 0
    );

    if (!assignments) {
        fprintf(stderr, "Failed to create assignments\n");
        free(metrics);
        free(arr);
        return 1;
    }

    // ── Step 3: Compile Worker Binary with Deserializer ──
    printf("[Main] Compiling the parsed binary with Execution_Worker/logic.c...\n");
    int compile_status = system(
        "gcc Parser/Tests/test.c_parsed.c Execution_Worker/logic.c "
        "-Wl,-e,worker_entry -I. -o Parser/Tests/test_worker_binary -pthread"
    );
    if (compile_status != 0) {
        fprintf(stderr, "Compilation failed!\n");
        return 1;
    }
    printf("[Main] Compilation successful.\n");

    // ── Step 4: Execute on Simulated Nodes ──
    char *node_results[2] = {NULL, NULL};

    for (int i = 0; i < node_count; i++) {
        printf("\n--- Executing Node %d (%s:%d) ---\n", i, metrics[i].uuid, metrics[i].port);
        chunk_data *chunk = assignments[i].chunk;

        // Write serialized parameters to a temporary file
        char temp_file[128];
        snprintf(temp_file, sizeof(temp_file), "Parser/Tests/node_%d_data.bin", i);
        FILE *fp = fopen(temp_file, "wb");
        if (!fp) {
            perror("fopen temp_file");
            continue;
        }
        fwrite(chunk->chunk, 1, chunk->chunk_size, fp);
        fclose(fp);

        // Create pipe to read result back
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            perror("pipe");
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Child: Redirect stdout to /dev/null to keep logs clean, or let it print
            close(pipefd[0]);
            char fd_str[16];
            snprintf(fd_str, sizeof(fd_str), "%d", pipefd[1]);

            char *args[] = {
                "./Parser/Tests/test_worker_binary",
                "sum_worker",
                temp_file,
                fd_str,
                NULL
            };
            execvp(args[0], args);
            perror("execvp");
            exit(1);
        }

        // Parent
        close(pipefd[1]);
        char res_buf[64];
        memset(res_buf, 0, sizeof(res_buf));
        read(pipefd[0], res_buf, sizeof(res_buf) - 1);
        close(pipefd[0]);

        int status;
        waitpid(pid, &status, 0);

        printf("[Main] Node %d finished. Raw result: %s\n", i, res_buf);
        node_results[i] = strdup(res_buf);
    }

    // ── Step 5: Reduce and Verify the Result ──
    printf("\n======================================================\n");
    printf("                  AGGREGATION & REDUCE                \n");
    printf("======================================================\n");
    for (int i = 0; i < node_count; i++) {
        printf("  Node %d partial result: %s\n", i, node_results[i] ? node_results[i] : "NULL");
    }

    void *reduced = sum_reduce(node_results[0], node_results[1]);
    printf("  --------------------------------------------------\n");
    printf("  FINAL AGGREGATED SUM: %s\n", reduced ? (char *)reduced : "NULL");
    printf("======================================================\n\n");

    // Clean up
    for (int i = 0; i < node_count; i++) {
        if (node_results[i]) free(node_results[i]);
        char temp_file[128];
        snprintf(temp_file, sizeof(temp_file), "Parser/Tests/node_%d_data.bin", i);
        unlink(temp_file);
    }
    system("rm -f Parser/Tests/test_worker_binary");
    if (reduced) free(reduced);

    for (int i = 0; i < node_count; i++) {
        if (assignments[i].target_node) free(assignments[i].target_node);
        if (assignments[i].task) free(assignments[i].task);
        if (assignments[i].chunk) {
            if (assignments[i].chunk->chunk) free(assignments[i].chunk->chunk);
            free(assignments[i].chunk);
        }
    }
    free(assignments);
    free(metrics);
    free(arr);

    return 0;
}
