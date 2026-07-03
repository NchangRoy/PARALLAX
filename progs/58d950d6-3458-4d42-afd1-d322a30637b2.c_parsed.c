/* === Parallax: embedded program source (auto-generated) === */
#include <string.h>
#include "parallax/parallax_param.h"
extern void execute_fxn(ParallaxParam *, int, char *, ParallaxExecutionCtx *, const char *, const char *);
void * product_array_generated(void * data, size_t total_size);
static const char *__parallax_prog_code__ = "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include \"parallax/parallax_param.h\"\n\nvoid * product_array(void * data, size_t total_size) {\n    int *arr = (int *)data;\n    int count = (int)(total_size / sizeof(int));\n    long long prod = 1;\n    for (int i = 0; i < count; i++) {\n        prod *= arr[i];\n    }\n    printf(\"[WorkerTask] Got %d elements, first=%d last=%d, partial product=%lld\\n\",\n           count, count > 0 ? arr[0] : 0, count > 0 ? arr[count-1] : 0, prod);\n    char *result = malloc(64);\n    sprintf(result, \"%lld\", prod);\n    return result;\n}\n\n\nvoid *product_array_worker(void *__arg) {\n    ParallaxParam *__p = (ParallaxParam *)__arg;\n    void * data = (void *)__p[0].data;\n    size_t total_size = *(size_t *)__p[1].data;\n    return (void *)product_array(data, total_size);\n}\n\n\n\ntypedef void *(*fn)(void *);\n\nfn matcher(char *name) {\n    if (strcmp(name, \"product_array_worker\") == 0) {\n        return (fn)product_array_worker;\n    }\n    return NULL;\n}\n\nint main() { return 0; }\n";
static const char *__parallax_prog_name__ = "58d950d6-3458-4d42-afd1-d322a30637b2";

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

    product_array_generated(payload, sizeof(payload));

    printf("[SubmittedProg] Done.\n");
    return 0;
}

void * product_array_generated(void * data, size_t total_size) {
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
    strncpy(__parallax_ctx.aggregator_name, "my_product_reduce", 63);
    __parallax_ctx.aggregator_name[63] = '\0';
    execute_fxn(__parallax_params, 2, "product_array_worker", &__parallax_ctx, __parallax_prog_code__, __parallax_prog_name__);
    return NULL;
}


typedef void *(*fn)(void *);

fn matcher(char *name) {
    if (strcmp(name, "my_product_reduce") == 0) {
        return (fn)my_product_reduce;
    }
    return NULL;
}
