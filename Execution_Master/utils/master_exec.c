
#include"network_agent.h"
#include<string.h>
#include<stdio.h>
#include"ms_queue.h"
#include<sys/msg.h>
#include"state_message.h"
#include"orchestrator.h"
#include"parallax_team.h"
#include<unistd.h>
#include"net_utils.h"
extern char controller_ip[16];

void *sum_reduce(void *a, void *b) {
    if (!a && !b) return NULL;
    long long val_a = a ? atoll((char*)a) : 0;
    long long val_b = b ? atoll((char*)b) : 0;
    char *res = malloc(64);
    sprintf(res, "%lld", val_a + val_b);
    return res;
}

void execute_fxn(void * data ,size_t total_size , char * fxn_name,int node_count){
    //first get worker xtics from controller
    // Allocate message with room for data payload
    message_t *message = malloc(sizeof(message_t));
    memset(message, 0, sizeof(message_t));
    message->mq_type = 1;
    strcpy(message->type, "NODES");
    strcpy(message->recv_type, "NODES_TEST");
    
    // Resolve our actual LAN IP so the controller can reply across the network
    char iface[64] = {0};
    load_network_interface(iface, sizeof(iface));
    get_local_ip(message->sender_ip, sizeof(message->sender_ip), iface);
    message->sender_port = 9005;  // test agent listens on 9005 — controller replies here
    message->size = 0;
    
    printf("[MasterExec] Sending NODES query with reply address %s:%d\n", 
           message->sender_ip, message->sender_port);
    
    send_msg(controller_ip, 9000, "master_out", message);
    printf("[MasterExec] NODES query sent to %s:9000\n", controller_ip);
    free(message);


    //read nodes data that was sent to the NODES mq

    map_entry * node_mq=find_by_msg_type("NODES_TEST");
    queued_message received_msg;

    

    while(1){

        ssize_t size = msgrcv(node_mq->queue_id, &received_msg, sizeof(queued_message) - sizeof(long), 1L, 0);
        if(size < 0){
            continue;
        }
        break;




        
    }

    MachineMetrics * metrics = (MachineMetrics *)received_msg.data;

    // Count how many valid nodes were actually returned
    int actual_node_count = 0;
    while (strlen(metrics[actual_node_count].uuid) > 0) {
        actual_node_count++;
    }

    if (actual_node_count == 0) {
        printf("[MasterExec] Error: 0 nodes connected to Controller! Aborting task.\n");
        return;
    }
    printf("[DATA] received %d nodes\n",actual_node_count);
    for (int i = 0; i < actual_node_count; i++) {
        printf("Node %d: %s\n", i, metrics[i].uuid);
    }
  
    // Use the actual number of nodes instead of the hardcoded request
    node_count = (actual_node_count < node_count) ? actual_node_count : node_count;

    //create task assignments
    task_assignment * assignments = create_assignments(
                                        data,
                                        total_size,
                                        fxn_name,
                                        metrics,
                                        node_count
                                    );

    // display assignments
    for (int i = 0; i < node_count; i++) {
        chunk_data *chunk = (chunk_data *)assignments[i].chunk;
        int *chunk_values = (int *)chunk->chunk;
        size_t int_count = chunk->chunk_size / sizeof(int);

        printf("\n=================================\n");
        printf("NODE %d\n", i);
        printf("=================================\n");
        printf("uuid          : %s\n", metrics[i].uuid);
        printf("ip            : %s\n", metrics[i].ip);
        printf("cpu usage     : %.2f\n", metrics[i].cpu_usage);
        printf("ram usage     : %.2f\n", metrics[i].mem_usage);
        printf("function      : %s\n", assignments[i].task->function_name);
        printf("nb chunk bytes: %d\n", chunk->chunk_size);
        printf("chunk ints    : %zu\n", int_count);
        printf("data preview  : ");
        for (size_t j = 0; j < int_count && j < 5; j++) {
            printf("%d ", chunk_values[j]);
        }
        printf("\n");
    }

    team *t = create_and_assign_task(assignments, node_count);

    team_start(t);

    team_wait(t);

    // Use the reduce function
    t->reduce_fxn = sum_reduce;
    void *final_result = team_reduce(t);
    
    // Aggregate results from each thread
    printf("\n=================================\n");
    printf("AGGREGATED EXECUTION RESULTS\n");
    printf("=================================\n");
    
    printf("---------------------------------\n");
    printf("FINAL REDUCED RESULT: %s\n", final_result ? (char*)final_result : "(null)");
    printf("=================================\n\n");

    for (int i = 0; i < t->num_workers; i++) {
        if (t->results[i]) {
            printf("Node %d partial result: %s\n", i, (char *)t->results[i]);
            free(t->results[i]); // Free the duplicated string
            t->results[i] = NULL;
        } else {
            printf("Node %d partial result: (null)\n", i);
        }
    }
    
    if (final_result) free(final_result);

    team_destroy(t);
   

}

