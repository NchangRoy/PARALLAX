#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "../Agent_Init/network/network_agent.h"
#include "../Agent_Init/network/ms_queue.h"
#include "utils/master_thread.h"

void init_client() {
    static network_agent_config cfg = {9008, "client_out"};
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, network_thread_run, &cfg);
    usleep(500000); 
    printf("[TestClient] Client network agent started on port %d.\n", cfg.port);
}

int main() {
    init_client();

    program_message_t prog;
    memset(&prog, 0, sizeof(prog));
    strcpy(prog.program_name, "test_prog4.c");
    
    // The C code payload we want the Master to compile and run
    strcpy(
        prog.code,
        "#include \"ms_queue.h\"\n"
        "#include \"network_agent.h\"\n"
        "#include <pthread.h>\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <unistd.h>\n"
        "\n"
        "extern void execute_fxn(void *data, size_t total_size, char *fxn_name,\n"
        "                        int node_count, char *prog_code, char *prog_name);\n"
        "\n"
        "extern char controller_ip[16];\n"
        "\n"
        "int main() {\n"
        "  create_mq(\"NODES_TEST\", 0);\n"
        "  map_entry *node_mq = find_by_msg_type(\"NODES_TEST\");\n"
        "  printf(\"NODES mq created with id %d\\n\", node_mq->queue_id);\n"
        "  int payload[100];\n"
        "  for (int i = 0; i < 100; i++) {\n"
        "    payload[i] = i + 1;\n"
        "  }\n"
        "\n"
        "  int expected_node_count = 2;\n"
        "  printf(\"[TestExec] WAITING for Controller at %s:9000 to respond to NODES query...\\n\", controller_ip);\n"
        "\n"
        "  char *worker_code = \"#include <stdio.h>\\n\"\n"
        "                      \"#include <stdlib.h>\\n\"\n"
        "                      \"#include <string.h>\\n\"\n"
        "                      \"\\n\"\n"
        "                      \"typedef void *(*fn)(void *);\\n\"\n"
        "                      \"\\n\"\n"
        "                      \"void *sum_array(void *arg) {\\n\"\n"
        "                      \"    char *filename = (char*)arg;\\n\"\n"
        "                      \"    printf(\\\"[WorkerTask] Running sum_array on file: %s\\\\n\\\", filename);\\n\"\n"
        "                      \"    FILE *f = fopen(filename, \\\"rb\\\");\\n\"\n"
        "                      \"    if (!f) {\\n\"\n"
        "                      \"        printf(\\\"[WorkerTask] Failed to open file: %s\\\\n\\\", filename);\\n\"\n"
        "                      \"        return strdup(\\\"0\\\");\\n\"\n"
        "                      \"    }\\n\"\n"
        "                      \"    fseek(f, 0, SEEK_END);\\n\"\n"
        "                      \"    long size = ftell(f);\\n\"\n"
        "                      \"    fseek(f, 0, SEEK_SET);\\n\"\n"
        "                      \"    int count = size / sizeof(int);\\n\"\n"
        "                      \"    printf(\\\"[WorkerTask] File size: %ld bytes (%d integers)\\\\n\\\", size, count);\\n\"\n"
        "                      \"    int *arr = malloc(size);\\n\"\n"
        "                      \"    fread(arr, sizeof(int), count, f);\\n\"\n"
        "                      \"    fclose(f);\\n\"\n"
        "                      \"    long long sum = 0;\\n\"\n"
        "                      \"    for(int i = 0; i < count; i++) {\\n\"\n"
        "                      \"        sum += arr[i];\\n\"\n"
        "                      \"    }\\n\"\n"
        "                      \"    printf(\\\"[WorkerTask] Calculated sum: %lld\\\\n\\\", sum);\\n\"\n"
        "                      \"    free(arr);\\n\"\n"
        "                      \"    char *result = malloc(64);\\n\"\n"
        "                      \"    sprintf(result, \\\"%lld\\\", sum);\\n\"\n"
        "                      \"    return result;\\n\"\n"
        "                      \"}\\n\"\n"
        "                      \"\\n\"\n"
        "                      \"fn matcher(char *name) {\\n\"\n"
        "                      \"    if (strcmp(name, \\\"sum_array\\\") == 0) {\\n\"\n"
        "                      \"        return sum_array;\\n\"\n"
        "                      \"    }\\n\"\n"
        "                      \"    return NULL;\\n\"\n"
        "                      \"}\\n\"\n"
        "                      \"\\n\"\n"
        "                      \"int main() { return 0; }\\n\";\n"
        "\n"
        "  execute_fxn(payload, sizeof(payload), \"sum_array\", expected_node_count, worker_code, \"test_prog4\");\n"
        "  printf(\"\\n[TestExec] execute_fxn completed successfully!\\n\");\n"
        "  return 0;\n"
        "}\n"
    );
    prog.code_size = strlen(prog.code);

    size_t total_size = sizeof(message_t) + sizeof(program_message_t);
    message_t *msg = malloc(total_size);
    memset(msg, 0, total_size);

    msg->mq_type = 1;
    memcpy(msg->type, "PROG", 4);
    memcpy(msg->recv_type, "RESP", 4);
    msg->size = sizeof(program_message_t);
    memcpy(msg->data, &prog, msg->size);

    printf("[TestClient] Sending test_prog4.c source code to Master at 127.0.0.1:9005...\n");
    send_msg("127.0.0.1", 9005, "client_out", msg);
    
    free(msg);
    usleep(500000); // Wait for packet to be fully sent
    printf("[TestClient] Message sent. Exiting client.\n");
    return 0;
}
