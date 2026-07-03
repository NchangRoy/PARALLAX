#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reduce aggregator: multiplies partial products from each worker.
   Name must be unique - Execution_Master/utils/master_exec.c already
   defines its own default "sum_reduce", so avoid reusing that. */
void *my_product_reduce(void *a, void *b) {
    if (!a && !b) return NULL;
    long long val_a = a ? atoll((char *)a) : 1;
    long long val_b = b ? atoll((char *)b) : 1;
    long long result = val_a * val_b;
    char *res = malloc(64);
    sprintf(res, "%lld", result);
    return res;
}

__attribute__((annotate("vcpus:2")))
__attribute__((annotate("reduce:my_product_reduce")))
void *product_array(void *data, size_t total_size) {
    int *arr = (int *)data;
    int count = (int)(total_size / sizeof(int));
    long long prod = 1;
    for (int i = 0; i < count; i++) {
        prod *= arr[i];
    }
    printf("[WorkerTask] Got %d elements, first=%d last=%d, partial product=%lld\n",
           count, count > 0 ? arr[0] : 0, count > 0 ? arr[count-1] : 0, prod);
    char *result = malloc(64);
    sprintf(result, "%lld", prod);
    return result;
}

int main() {
    printf("[SubmittedProg] Starting distributed product test...\n");

    int payload[10];
    for (int i = 0; i < 10; i++) {
        payload[i] = i + 1;   /* 1*2*...*10 = 3628800 */
    }

    product_array(payload, sizeof(payload));

    printf("[SubmittedProg] Done.\n");
    return 0;
}
