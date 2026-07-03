// __parallax_callback_host__ = "127.0.0.1"
// __parallax_callback_port__ = "5000"
// __parallax_prog_name__ = "5219a94d-f2c7-47e0-89ca-989eeb70c6ab"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reduce aggregator: MUST be defined here, name must match the reduce: annotation below */
void *sum_reduce(void *a, void *b) {
    if (!a && !b) return NULL;
    long long val_a = a ? atoll((char *)a) : 0;
    long long val_b = b ? atoll((char *)b) : 0;
    char *res = malloc(64);
    sprintf(res, "%lld", val_a + val_b);
    return res;
}

/* vcpus:2 -> master will split the 100-int payload across 2 worker slots.
   You only have 1 worker registered right now, so master_exec.c will cap
   node_count down to actual_node_count (see Execution_Master/utils/master_exec.c:116-119)
   and print "[MasterExec] Requested N nodes but only M available" if it does. */
__attribute__((annotate("vcpus:2")))
__attribute__((annotate("reduce:sum_reduce")))
void *sum_array(void *data, size_t total_size) {
    int *arr = (int *)data;
    int count = (int)(total_size / sizeof(int));
    long long sum = 0;
    for (int i = 0; i < count; i++) {
        sum += arr[i];
    }
    /* This line proves distribution: each worker only sees ITS chunk,
       so count will be < 100 and the values will differ per node's log. */
    printf("[WorkerTask] Got %d elements, first=%d last=%d, partial sum=%lld\n",
           count, count > 0 ? arr[0] : 0, count > 0 ? arr[count-1] : 0, sum);
    char *result = malloc(64);
    sprintf(result, "%lld", sum);
    return result;
}

int main() {
    printf("[SubmittedProg] Starting distributed sum test...\n");

    int payload[100];
    for (int i = 0; i < 100; i++) {
        payload[i] = i + 1;   /* total sum should be 5050 */
    }

    sum_array(payload, sizeof(payload));

    printf("[SubmittedProg] Done.\n");
    return 0;
}
