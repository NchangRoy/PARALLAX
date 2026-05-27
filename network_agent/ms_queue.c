#include"ms_queue.h"
#include<sys/ipc.h>
#include<sys/msg.h>
#include<stdio.h>
#include<stdint.h>
#include<string.h>
#include<stdlib.h>

//create a message queue for a particualr message tytpe
map_registry * registry=NULL;
int create_mq(char *msg_type, int msg_len)
{       
    //initiliaze map registry for mapping queue to msg  type
    if (registry == NULL) {
        registry = malloc(sizeof(map_registry));
        if (!registry) return -1;

        registry->counter = 0;
        registry->base_path = "/tmp";
        registry->head = NULL;
    }

    int id = registry->counter++;
    //create key
    key_t key = ftok(registry->base_path, (int)(id & 0xFF));
    //create mesage queue
    int msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid < 0) {
        perror("Error creating queue");
        return -1;
    }
    //save to map registry
    map_entry *entry = malloc(sizeof(map_entry));
    if (!entry) return -1;

    strncpy(entry->mq_path, registry->base_path, sizeof(entry->mq_path));
    entry->mq_path[sizeof(entry->mq_path)-1] = '\0';

    strncpy(entry->msg_type, msg_type, sizeof(entry->msg_type));
    entry->msg_type[sizeof(entry->msg_type)-1] = '\0';

    entry->msg_len = msg_len;
    entry->queue_id = id;

    if (registry->head == NULL) {
        registry->head = create_node(entry);
    } else {
        node * new_node=create_node(entry);
        
        push_back(registry->head, new_node);
    }

    return msgid;
}


map_entry * find_by_msg_type(char *msg_type)
{
    node *temp = registry->head;

    while (temp != NULL) {
        map_entry *entry = (map_entry *)temp->data;

        if (strcmp(entry->msg_type, msg_type) == 0) {
            return entry;
        }

        temp = temp->next;
    }

    return NULL;
}

void destroy_queues()
{
    if (!registry || !registry->head)
        return;

    node *temp = registry->head;

    while (temp != NULL) {

        map_entry *entry = (map_entry *)temp->data;

        if (msgctl(entry->queue_id, IPC_RMID, NULL) == 0) {
            printf("Removed queue: %s (id=%d)\n",
                   entry->msg_type,
                   entry->queue_id);
        } else {
            perror("msgctl IPC_RMID");
        }

        temp = temp->next;
    }
}