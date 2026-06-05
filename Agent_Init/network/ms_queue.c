/* ==========================================================================
 * ms_queue.c  —  System V message queue registry
 *
 * Maps logical string names (msg_type) to System V IPC message queue IDs.
 * All queues created through this module are tracked in a global linked-list
 * registry so they can be looked up, individually deleted, or bulk-destroyed.
 * ========================================================================== */

#include "ms_queue.h"
#include <sys/ipc.h>
#include <sys/msg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Global registry — single linked list of all active map_entry records.
 * -------------------------------------------------------------------------- */

map_registry *registry = NULL;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/*
 * destroy_map_entry
 *
 * Description: Callback passed to destroy_list() to free a single map_entry
 *              that was allocated on the heap.
 *
 * Input:  data  — void* pointing to a heap-allocated map_entry.
 * Output: none
 */
static void destroy_map_entry(void *data)
{
    free(data);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/*
 * create_mq
 *
 * Description: Creates a new System V message queue and registers it under a
 *              logical name (msg_type). If msg_type is NULL a random 7-char
 *              alphanumeric name is generated automatically. The registry is
 *              lazily initialised on the first call.
 *
 * Input:  msg_type — logical name for the queue, or NULL for a random name.
 *         msg_len  — advisory maximum message length (stored in registry,
 *                    not enforced by the kernel).
 *
 * Output: Pointer to the registered msg_type string on success, NULL on error.
 */
char *create_mq(char *msg_type, int msg_len)
{
    /* Lazy-init the global registry */
    if (registry == NULL) {
        registry = (map_registry *)malloc(sizeof(map_registry));
        if (!registry) return NULL;

        registry->counter   = 0;
        registry->base_path = "/tmp";
        registry->head      = NULL;

        /* Seed rand() once per process so random names differ across runs */
        srand((unsigned int)(time(NULL) ^ (unsigned int)getpid()));
    }

    /* Generate a random name when the caller passes NULL */
    char *random_msg_type = (char *)malloc(8);
    if (msg_type == NULL) {
        const char charset[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; i < 7; i++) {
            int key = rand() % (int)(sizeof(charset) - 1);
            random_msg_type[i] = charset[key];
        }
        random_msg_type[7] = '\0';
        msg_type = random_msg_type;
    }

    registry->counter++;

    /* Create the kernel-level queue with IPC_PRIVATE for process isolation */
    int msgid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);
    if (msgid < 0) {
        perror("create_mq: msgget");
        free(random_msg_type);
        return NULL;
    }

    /* Build and insert a new registry entry */
    map_entry *entry = (map_entry *)malloc(sizeof(map_entry));
    if (!entry) {
        free(random_msg_type);
        return NULL;
    }

    strncpy(entry->mq_path,  registry->base_path, sizeof(entry->mq_path)  - 1);
    entry->mq_path[sizeof(entry->mq_path) - 1] = '\0';

    strncpy(entry->msg_type, msg_type,             sizeof(entry->msg_type) - 1);
    entry->msg_type[sizeof(entry->msg_type) - 1] = '\0';

    entry->msg_len  = msg_len;
    entry->queue_id = msgid;

    if (registry->head == NULL) {
        registry->head = create_node(entry);
    } else {
        node *new_node = create_node(entry);
        push_back(registry->head, new_node);
    }

    return entry->msg_type;
}

/*
 * find_by_msg_type
 *
 * Description: Searches the registry for the queue associated with a given
 *              logical name.
 *
 * Input:  msg_type — the logical name to look up.
 *
 * Output: Pointer to the matching map_entry on success, NULL if not found or
 *         if the registry has not been initialised yet.
 */
map_entry *find_by_msg_type(char *msg_type)
{
    if (!registry)
        return NULL;

    node *temp = registry->head;
    while (temp != NULL) {
        map_entry *entry = (map_entry *)temp->data;
        if (strcmp(entry->msg_type, msg_type) == 0)
            return entry;
        temp = temp->next;
    }

    return NULL;
}

/*
 * delete_mq
 *
 * Description: Destroys a single System V queue by its logical name, removes
 *              its entry from the registry, and frees the associated memory.
 *              Use this to clean up one-shot reply queues after their response
 *              has been consumed so that IPC queue limits are not exhausted.
 *
 * Input:  msg_type — logical name of the queue to delete.
 *
 * Output: 0 on success, -1 if the name was not found or the registry is NULL.
 */
int delete_mq(const char *msg_type)
{
    if (!registry || !msg_type)
        return -1;

    node      *temp   = registry->head;
    node      *target = NULL;
    map_entry *entry  = NULL;

    while (temp != NULL) {
        entry = (map_entry *)temp->data;
        if (strcmp(entry->msg_type, msg_type) == 0) {
            target = temp;
            break;
        }
        temp = temp->next;
    }

    if (!target)
        return -1;

    if (msgctl(entry->queue_id, IPC_RMID, NULL) == 0) {
        printf("Deleted queue: %s (id=%d)\n", entry->msg_type, entry->queue_id);
    } else {
        perror("delete_mq: msgctl IPC_RMID");
    }

    delete_node(&registry->head, target);
    free(entry);
    return 0;
}

/*
 * destroy_queues
 *
 * Description: Destroys all System V queues tracked by the registry, frees
 *              every entry and the registry itself, and resets the global
 *              pointer to NULL. Call this on clean process shutdown to avoid
 *              leaving orphaned IPC resources in the kernel.
 *
 * Input:  none
 * Output: none
 */
void destroy_queues(void)
{
    if (!registry)
        return;

    node *temp = registry->head;
    while (temp != NULL) {
        map_entry *entry = (map_entry *)temp->data;
        if (msgctl(entry->queue_id, IPC_RMID, NULL) == 0) {
            printf("Removed queue: %s (id=%d)\n",
                   entry->msg_type, entry->queue_id);
        } else {
            perror("destroy_queues: msgctl IPC_RMID");
        }
        temp = temp->next;
    }

    destroy_list(&registry->head, destroy_map_entry);
    free(registry);
    registry = NULL;
}
