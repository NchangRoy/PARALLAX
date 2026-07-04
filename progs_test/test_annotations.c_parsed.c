/* === Parallax: embedded program source (auto-generated) === */
#include <string.h>
#include "parallax/parallax_param.h"
extern void execute_fxn(ParallaxParam *, int, char *, ParallaxExecutionCtx *, const char *, const char *);
void * sum_array_v2_generated(void * data, size_t total_size);
static const char *__parallax_prog_code__ = "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include \"parallax/parallax_param.h\"\n\nvoid * sum_array_v2(void * data, size_t total_size) {\n    int *arr = (int *)data;\n    int count = (int)(total_size / sizeof(int));\n    long long sum = 0;\n    for (int i = 0; i < count; i++) {\n        sum += arr[i];\n    }\n    char *result = malloc(64);\n    sprintf(result, \"%lld\", sum);\n    return result;\n}\n\n\nvoid *sum_array_v2_worker(void *__arg) {\n    ParallaxParam *__p = (ParallaxParam *)__arg;\n    void * data = (void *)__p[0].data;\n    size_t total_size = *(size_t *)__p[1].data;\n    return (void *)sum_array_v2(data, total_size);\n}\n\n\n\ntypedef void *(*fn)(void *);\n\nfn matcher(char *name) {\n    if (strcmp(name, \"sum_array_v2_worker\") == 0) {\n        return (fn)sum_array_v2_worker;\n    }\n    return NULL;\n}\n\nint main() { return 0; }\n";
static const char *__parallax_prog_name__ = "test_annotations";

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
    sum_array_v2_generated(payload, sizeof(payload));
    return 0;
}

void * sum_array_v2_generated(void * data, size_t total_size) {
    ParallaxParam __parallax_params[2];
    __parallax_params[0].data = (void *)data;
    __parallax_params[0].size = total_size;
    __parallax_params[0].distribution = PARALLAX_SCATTER;
    __parallax_params[0].index = 0;
    strncpy(__parallax_params[0].type_name, "void *", 63);
    __parallax_params[1].data = (void *)&total_size;
    __parallax_params[1].size = sizeof(total_size);
    __parallax_params[1].distribution = PARALLAX_SIZE_OF;
    __parallax_params[1].index = 1;
    strncpy(__parallax_params[1].type_name, "size_t", 63);
    ParallaxExecutionCtx __parallax_ctx;
    __parallax_ctx.expected_node_count = 2;
    strncpy(__parallax_ctx.aggregator_name, "my_sum_reduce_v2", 63);
    __parallax_ctx.aggregator_name[63] = '\0';
    __parallax_ctx.min_ram_mb = 64;
    __parallax_ctx.align = 1;
    execute_fxn(__parallax_params, 2, "sum_array_v2_worker", &__parallax_ctx, __parallax_prog_code__, __parallax_prog_name__);
    return NULL;
}


typedef void *(*fn)(void *);

fn matcher(char *name) {
    if (strcmp(name, "my_sum_reduce_v2") == 0) {
        return (fn)my_sum_reduce_v2;
    }
    return NULL;
}
