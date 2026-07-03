/* === Parallax: embedded program source (auto-generated) === */
#include <string.h>
#include "parallax/parallax_param.h"
extern void execute_fxn(ParallaxParam *, int, char *, ParallaxExecutionCtx *, const char *, const char *);
void * sum_array_generated(void * data, size_t total_size);
static const char *__parallax_prog_code__ = "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include \"parallax/parallax_param.h\"\n\nvoid * sum_array(void * data, size_t total_size) {\n    int *arr = (int *)data;\n    int count = (int)(total_size / sizeof(int));\n    long long sum = 0;\n    for (int i = 0; i < count; i++) {\n        sum += arr[i];\n    }\n    /* This line proves distribution: each worker only sees ITS chunk,\n       so count will be < 100 and the values will differ per node's log. */\n    printf(\"[WorkerTask] Got %d elements, first=%d last=%d, partial sum=%lld\\n\",\n           count, count > 0 ? arr[0] : 0, count > 0 ? arr[count-1] : 0, sum);\n    char *result = malloc(64);\n    sprintf(result, \"%lld\", sum);\n    return result;\n}\n\n\nvoid *sum_array_worker(void *__arg) {\n    ParallaxParam *__p = (ParallaxParam *)__arg;\n    void * data = (void *)__p[0].data;\n    size_t total_size = *(size_t *)__p[1].data;\n    return (void *)sum_array(data, total_size);\n}\n\n\n\ntypedef void *(*fn)(void *);\n\nfn matcher(char *name) {\n    if (strcmp(name, \"sum_array_worker\") == 0) {\n        return (fn)sum_array_worker;\n    }\n    return NULL;\n}\n\nint main() { return 0; }\n";
static const char *__parallax_prog_name__ = "6ebbbb69-0df5-4a41-a1e1-3cd201305f60";

// __parallax_callback_host__ = "127.0.0.1"
// __parallax_callback_port__ = "5000"
// __parallax_prog_name__ = "6ebbbb69-0df5-4a41-a1e1-3cd201305f60"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reduce aggregator: MUST be defined here, name must match the reduce: annotation below.
   NOTE: don't call this "sum_reduce" - Execution_Master/utils/master_exec.c:15 already
   defines a function with that name as its built-in default reducer, and since the
   master links your submission against its own sources, it collides at link time. */
void *my_sum_reduce(void *a, void *b) {
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
__attribute__((annotate("reduce:my_sum_reduce")))
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

    sum_array_generated(payload, sizeof(payload));

    printf("[SubmittedProg] Done.\n");
    return 0;
}

void * sum_array_generated(void * data, size_t total_size) {
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
    strncpy(__parallax_ctx.aggregator_name, "my_sum_reduce", 63);
    __parallax_ctx.aggregator_name[63] = '\0';
    execute_fxn(__parallax_params, 2, "sum_array_worker", &__parallax_ctx, __parallax_prog_code__, __parallax_prog_name__);
    return NULL;
}


typedef void *(*fn)(void *);

fn matcher(char *name) {
    if (strcmp(name, "my_sum_reduce") == 0) {
        return (fn)my_sum_reduce;
    }
    return NULL;
}
