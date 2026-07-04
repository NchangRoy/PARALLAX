#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *my_sum_reduce_v2(void *a, void *b) {
    if (!a && !b) return NULL;
    long long val_a = a ? atoll((char *)a) : 0;
    long long val_b = b ? atoll((char *)b) : 0;
    char *res = malloc(64);
    sprintf(res, "%lld", val_a + val_b);
    return res;
}

__attribute__((annotate("cpus:2")))
__attribute__((annotate("reduce:my_sum_reduce_v2")))
__attribute__((annotate("ram:64")))
__attribute__((annotate("align:1")))
void *sum_array_v2(void *data, size_t total_size) {
    int *arr = (int *)data;
    int count = (int)(total_size / sizeof(int));
    long long sum = 0;
    for (int i = 0; i < count; i++) {
        sum += arr[i];
    }
    char *result = malloc(64);
    sprintf(result, "%lld", sum);
    return result;
}

int main() {
    int payload[100];
    for (int i = 0; i < 100; i++) payload[i] = i + 1;
    sum_array_v2(payload, sizeof(payload));
    return 0;
}
