// __parallax_callback_host__ = "127.0.0.1"
// __parallax_callback_port__ = "5000"
// __parallax_callback_host__ = "127.0.0.1"
// __parallax_callback_port__ = "5000"
// __parallax_callback_host__ = "127.0.0.1"
// __parallax_callback_port__ = "5000"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* __parallax_prog_name__ = "test_sum" */

__attribute__((annotate("vcpus:2")))
__attribute__((annotate("reduce:sum_reduce")))
void *sum_array(void *data, size_t total_size) {
    int *arr = (int *)data;
    int count = (int)(total_size / sizeof(int));
    long long sum = 0;
    for (int i = 0; i < count; i++) {
        sum += arr[i];
    }
    printf("[WorkerTask] Partial sum: %lld over %d elements\n", sum, count);
    char *result = malloc(64);
    sprintf(result, "%lld", sum);
    return result;
}

int main() {
    printf("[SubmittedProg] Starting sum map-reduce...\n");

    int payload[100];
    for (int i = 0; i < 100; i++) {
        payload[i] = i + 1;  /* sum should be 5050 */
    }

    sum_array(payload, sizeof(payload));

    printf("[SubmittedProg] Done.\n");
    return 0;
}
