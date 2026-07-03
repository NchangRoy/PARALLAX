/* === Parallax: embedded program source (auto-generated) === */
#include <string.h>
#include "parallax/parallax_param.h"
extern void execute_fxn(ParallaxParam *, int, char *, ParallaxExecutionCtx *, const char *, const char *);
void * matvec_mult_generated(void * matrix_data, size_t matrix_size, void * vector_data, size_t vector_size);
static const char *__parallax_prog_code__ = "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include \"parallax/parallax_param.h\"\n\nvoid * matvec_mult(void * matrix_data, size_t matrix_size, void * vector_data, size_t vector_size) {\n    (void)vector_size;\n    const int cols = 3;\n    int *matrix = (int *)matrix_data;\n    int *vector = (int *)vector_data;\n    int rows = (int)(matrix_size / (cols * sizeof(int)));\n\n    printf(\"[WorkerTask] Computing %d row(s) x %d col(s) matrix-vector product\\n\", rows, cols);\n\n    char *result = malloc(256);\n    int pos = 0;\n    for (int i = 0; i < rows; i++) {\n        long long sum = 0;\n        for (int j = 0; j < cols; j++) {\n            sum += (long long)matrix[i * cols + j] * vector[j];\n        }\n        pos += snprintf(result + pos, 256 - pos, i > 0 ? \",%lld\" : \"%lld\", sum);\n    }\n    return result;\n}\n\n\nvoid *matvec_mult_worker(void *__arg) {\n    ParallaxParam *__p = (ParallaxParam *)__arg;\n    void * matrix_data = (void *)__p[0].data;\n    size_t matrix_size = *(size_t *)__p[1].data;\n    void * vector_data = (void *)__p[2].data;\n    size_t vector_size = *(size_t *)__p[3].data;\n    return (void *)matvec_mult(matrix_data, matrix_size, vector_data, vector_size);\n}\n\n\n\ntypedef void *(*fn)(void *);\n\nfn matcher(char *name) {\n    if (strcmp(name, \"matvec_mult_worker\") == 0) {\n        return (fn)matvec_mult_worker;\n    }\n    return NULL;\n}\n\nint main() { return 0; }\n";
static const char *__parallax_prog_name__ = "dc84d162-cc94-402b-863a-8fe008d27053";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 4
#define COLS 3

/* Reduce aggregator: concatenates each worker's partial output rows with
   ';'. With only one worker currently registered this just passes the
   single result through untouched. */
void *my_concat_reduce(void *a, void *b) {
    if (!a && !b) return NULL;
    if (!a) { char *r = malloc(strlen((char *)b) + 1); strcpy(r, (char *)b); return r; }
    if (!b) { char *r = malloc(strlen((char *)a) + 1); strcpy(r, (char *)a); return r; }
    size_t len = strlen((char *)a) + strlen((char *)b) + 2;
    char *res = malloc(len);
    snprintf(res, len, "%s;%s", (char *)a, (char *)b);
    return res;
}

/*
 * matrix_data / matrix_size: SCATTER pair - matrix rows split across workers.
 * vector_data: BROADCAST (2nd pointer param) - every worker gets the full
 * vector, since every row's dot product needs all of it.
 * vector_size: SIZE_OF companion for vector_data. Its only job is to make
 * the parser give vector_data a real (non-zero) size so its bytes actually
 * get serialized into the packet - Execution_Master/utils/orchestrator.c's
 * create_assignments() only tracks a single "size_idx" slot, so with two
 * SIZE_OF params the second one's *deserialized value* gets clobbered with
 * the matrix chunk's byte count when the task is dispatched. We never read
 * vector_size at runtime because of that - COLS is a compile-time constant
 * instead, so the corruption is harmless here.
 */
/* cols is a literal (not the file-scope COLS macro) because the parser only
   re-embeds this function's own body for the worker-side build - file-scope
   #defines from the rest of the source don't travel with it, so referencing
   COLS here would fail to compile on the worker ("COLS undeclared"). */
__attribute__((annotate("vcpus:2")))
__attribute__((annotate("reduce:my_concat_reduce")))
void *matvec_mult(void *matrix_data, size_t matrix_size, void *vector_data, size_t vector_size) {
    (void)vector_size;
    const int cols = 3;
    int *matrix = (int *)matrix_data;
    int *vector = (int *)vector_data;
    int rows = (int)(matrix_size / (cols * sizeof(int)));

    printf("[WorkerTask] Computing %d row(s) x %d col(s) matrix-vector product\n", rows, cols);

    char *result = malloc(256);
    int pos = 0;
    for (int i = 0; i < rows; i++) {
        long long sum = 0;
        for (int j = 0; j < cols; j++) {
            sum += (long long)matrix[i * cols + j] * vector[j];
        }
        pos += snprintf(result + pos, 256 - pos, i > 0 ? ",%lld" : "%lld", sum);
    }
    return result;
}

int main() {
    printf("[SubmittedProg] Starting distributed matrix-vector multiply...\n");

    int matrix[ROWS * COLS] = {
        1, 2, 3,
        4, 5, 6,
        7, 8, 9,
        10, 11, 12
    };
    int vector[COLS] = { 1, 0, -1 };
    /* Expected per-row: row0=1-3=-2, row1=4-6=-2, row2=7-9=-2, row3=10-12=-2
       => result vector: -2,-2,-2,-2 */

    matvec_mult_generated(matrix, sizeof(matrix), vector, sizeof(vector));

    printf("[SubmittedProg] Done.\n");
    return 0;
}

void * matvec_mult_generated(void * matrix_data, size_t matrix_size, void * vector_data, size_t vector_size) {
    ParallaxParam __parallax_params[4];
    __parallax_params[0].data = (void *)matrix_data;
    __parallax_params[0].size = matrix_size;
    __parallax_params[0].distribution = PARALLAX_SCATTER;
    __parallax_params[0].index = 0;
    strncpy(__parallax_params[0].type_name, "void *", 63);
    __parallax_params[1].data = (void *)&matrix_size;
    __parallax_params[1].size = sizeof(matrix_size);
    __parallax_params[1].distribution = PARALLAX_SIZE_OF;
    __parallax_params[1].index = 1;
    strncpy(__parallax_params[1].type_name, "size_t", 63);
    __parallax_params[2].data = (void *)vector_data;
    __parallax_params[2].size = vector_size;
    __parallax_params[2].distribution = PARALLAX_BROADCAST;
    __parallax_params[2].index = 2;
    strncpy(__parallax_params[2].type_name, "void *", 63);
    __parallax_params[3].data = (void *)&vector_size;
    __parallax_params[3].size = sizeof(vector_size);
    __parallax_params[3].distribution = PARALLAX_SIZE_OF;
    __parallax_params[3].index = 3;
    strncpy(__parallax_params[3].type_name, "size_t", 63);
    ParallaxExecutionCtx __parallax_ctx;
    __parallax_ctx.expected_node_count = 2;
    strncpy(__parallax_ctx.aggregator_name, "my_concat_reduce", 63);
    __parallax_ctx.aggregator_name[63] = '\0';
    execute_fxn(__parallax_params, 4, "matvec_mult_worker", &__parallax_ctx, __parallax_prog_code__, __parallax_prog_name__);
    return NULL;
}


typedef void *(*fn)(void *);

fn matcher(char *name) {
    if (strcmp(name, "my_concat_reduce") == 0) {
        return (fn)my_concat_reduce;
    }
    return NULL;
}
