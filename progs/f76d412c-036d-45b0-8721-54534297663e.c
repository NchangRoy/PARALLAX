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
        0, 0, 1,
        0, 0, 2,
        0, 0, 3,
        0, 0, 4
    };
    int vector[COLS] = { 1, 0, -1 };
    /* Expected per-row: row0=0-1=-1, row1=0-2=-2, row2=0-3=-3, row3=0-4=-4
       => result vector: -1,-2,-3,-4 (distinct per row, so a wrong row split
       or misordered chunk is now visible instead of hiding behind repeated
       -2 values) */

    matvec_mult(matrix, sizeof(matrix), vector, sizeof(vector));

    printf("[SubmittedProg] Done.\n");
    return 0;
}
