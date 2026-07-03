// __parallax_callback_host__ = "127.0.0.1"
// __parallax_callback_port__ = "5000"
// __parallax_prog_name__ = "7c8ccb31-91b2-41f7-b90a-ec174bfb4f11"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Custom aggregator: return the larger of two string-encoded integers */
void *max_reduce(void *a, void *b) {
    if (!a && !b) return NULL;
    long long val_a = a ? atoll((char *)a) : (long long)INT_MIN;
    long long val_b = b ? atoll((char *)b) : (long long)INT_MIN;
    char *res = malloc(32);
    sprintf(res, "%lld", val_a > val_b ? val_a : val_b);
    return res;
}

/* Find the maximum integer in a scattered chunk.
 * Workers each receive a slice of the array and return their local max.
 * max_reduce then picks the global max across all partial results.
 */
__attribute__((annotate("vcpus:2")))
__attribute__((annotate("reduce:max_reduce")))
void *find_max(int *arr, size_t n) {
    int count = (int)(n / sizeof(int));
    int max = INT_MIN;
    for (int i = 0; i < count; i++) {
        if (arr[i] > max)
            max = arr[i];
    }
    printf("[Worker] Local max = %d  (%d elements scanned)\n", max, count);
    char *result = malloc(32);
    sprintf(result, "%d", max);
    return result;
}

int main(void) {
    printf("[Main] find_max task starting...\n");

    /* 60 integers spanning -30..+29, global max is 29 */
    int data[60];
    for (int i = 0; i < 60; i++)
        data[i] = i - 30;

    printf("[Main] Dataset: %d integers from %d to %d, expected max = 29\n",
           60, data[0], data[59]);

    find_max(data, sizeof(data));

    printf("[Main] Done.\n");
    return 0;
}
