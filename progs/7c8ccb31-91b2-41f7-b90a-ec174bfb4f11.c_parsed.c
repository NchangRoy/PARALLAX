/* === Parallax: embedded program source (auto-generated) === */
#include <string.h>
#include "parallax/parallax_param.h"
extern void execute_fxn(ParallaxParam *, int, char *, ParallaxExecutionCtx *, const char *, const char *);
void * find_max_generated(int * arr, size_t n);
static const char *__parallax_prog_code__ = "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include \"parallax/parallax_param.h\"\n#include <limits.h>\n\nvoid * find_max(int * arr, size_t n) {\n    int count = (int)(n / sizeof(int));\n    int max = INT_MIN;\n    for (int i = 0; i < count; i++) {\n        if (arr[i] > max)\n            max = arr[i];\n    }\n    printf(\"[Worker] Local max = %d  (%d elements scanned)\\n\", max, count);\n    char *result = malloc(32);\n    sprintf(result, \"%d\", max);\n    return result;\n}\n\n\nvoid *find_max_worker(void *__arg) {\n    ParallaxParam *__p = (ParallaxParam *)__arg;\n    int * arr = (int *)__p[0].data;\n    size_t n = *(size_t *)__p[1].data;\n    return (void *)find_max(arr, n);\n}\n\n\n\ntypedef void *(*fn)(void *);\n\nfn matcher(char *name) {\n    if (strcmp(name, \"find_max_worker\") == 0) {\n        return (fn)find_max_worker;\n    }\n    return NULL;\n}\n\nint main() { return 0; }\n";
static const char *__parallax_prog_name__ = "7c8ccb31-91b2-41f7-b90a-ec174bfb4f11";

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

    find_max_generated(data, sizeof(data));

    printf("[Main] Done.\n");
    return 0;
}

void * find_max_generated(int * arr, size_t n) {
    ParallaxParam __parallax_params[2];
    __parallax_params[0].data = (void *)arr;
    __parallax_params[0].size = n;
    __parallax_params[0].distribution = PARALLAX_SCATTER;
    __parallax_params[0].index = 0;
    strncpy(__parallax_params[0].type_name, "int *", 63);
    __parallax_params[1].data = (void *)&n;
    __parallax_params[1].size = sizeof(n);
    __parallax_params[1].distribution = PARALLAX_SIZE_OF;
    __parallax_params[1].index = 1;
    strncpy(__parallax_params[1].type_name, "size_t", 63);
    ParallaxExecutionCtx __parallax_ctx;
    __parallax_ctx.expected_node_count = 2;
    strncpy(__parallax_ctx.aggregator_name, "max_reduce", 63);
    __parallax_ctx.aggregator_name[63] = '\0';
    execute_fxn(__parallax_params, 2, "find_max_worker", &__parallax_ctx, __parallax_prog_code__, __parallax_prog_name__);
    return NULL;
}


typedef void *(*fn)(void *);

fn matcher(char *name) {
    if (strcmp(name, "max_reduce") == 0) {
        return (fn)max_reduce;
    }
    return NULL;
}
