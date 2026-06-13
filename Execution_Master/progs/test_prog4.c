#include "ms_queue.h"
#include "network_agent.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void execute_fxn(void *data, size_t total_size, char *fxn_name,
                        int node_count, char *prog_code, char *prog_name);

extern char controller_ip[16];

int main() {
  create_mq("NODES_TEST", 0);
  map_entry *node_mq = find_by_msg_type("NODES_TEST");
  printf("NODES mq created with id %d\n", node_mq->queue_id);
  int payload[100];
  for (int i = 0; i < 100; i++) {
    payload[i] = i + 1;
  }

  int expected_node_count = 2;
  printf("[TestExec] WAITING for Controller at %s:9000 to respond to NODES query...\n", controller_ip);

  char *worker_code = "#include <stdio.h>\n"
                      "#include <stdlib.h>\n"
                      "#include <string.h>\n"
                      "\n"
                      "typedef void *(*fn)(void *);\n"
                      "\n"
                      "void *sum_array(void *arg) {\n"
                      "    char *filename = (char*)arg;\n"
                      "    printf(\"[WorkerTask] Running sum_array on file: %s\\n\", filename);\n"
                      "    FILE *f = fopen(filename, \"rb\");\n"
                      "    if (!f) {\n"
                      "        printf(\"[WorkerTask] Failed to open file: %s\\n\", filename);\n"
                      "        return strdup(\"0\");\n"
                      "    }\n"
                      "    fseek(f, 0, SEEK_END);\n"
                      "    long size = ftell(f);\n"
                      "    fseek(f, 0, SEEK_SET);\n"
                      "    int count = size / sizeof(int);\n"
                      "    printf(\"[WorkerTask] File size: %ld bytes (%d integers)\\n\", size, count);\n"
                      "    int *arr = malloc(size);\n"
                      "    fread(arr, sizeof(int), count, f);\n"
                      "    fclose(f);\n"
                      "    long long sum = 0;\n"
                      "    for(int i = 0; i < count; i++) {\n"
                      "        sum += arr[i];\n"
                      "    }\n"
                      "    printf(\"[WorkerTask] Calculated sum: %lld\\n\", sum);\n"
                      "    free(arr);\n"
                      "    char *result = malloc(64);\n"
                      "    sprintf(result, \"%lld\", sum);\n"
                      "    return result;\n"
                      "}\n"
                      "\n"
                      "fn matcher(char *name) {\n"
                      "    if (strcmp(name, \"sum_array\") == 0) {\n"
                      "        return sum_array;\n"
                      "    }\n"
                      "    return NULL;\n"
                      "}\n"
                      "\n"
                      "int main() { return 0; }\n";

  execute_fxn(payload, sizeof(payload), "sum_array", expected_node_count, worker_code, "test_prog4");
  printf("\n[TestExec] execute_fxn completed successfully!\n");
  return 0;
}
