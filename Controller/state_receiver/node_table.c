#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "node.h"

// Instance globale de la table des noeuds pour le contrôleur
NodeTable g_node_table;

/**
 * Initialise la table des nœuds (liste chaînée vide et mutex).
 */
void node_table_init(NodeTable* table) {
    table->head  = NULL;
    table->count = 0;
    pthread_mutex_init(&table->lock, NULL);
}

/**
 * Détruit la table des nœuds en libérant la mémoire de chaque élément.
 */
void node_table_destroy(NodeTable* table) {
    pthread_mutex_lock(&table->lock);
    NodeInfo* cur = table->head;
    while (cur) {
        NodeInfo* next = cur->next;
        free(cur);
        cur = next;
    }
    table->head  = NULL;
    table->count = 0;
    pthread_mutex_unlock(&table->lock);
    pthread_mutex_destroy(&table->lock);
}

/**
 * Recherche un nœud dans la table à partir de son UUID.
 * Doit être appelé sous le verrou (mutex) de la table.
 * Retourne le pointeur vers NodeInfo ou NULL si non trouvé.
 */
NodeInfo* node_table_find(NodeTable* table, const char* uuid) {
    for (NodeInfo* n = table->head; n; n = n->next)
        if (strcmp(n->uuid, uuid) == 0)
            return n;
    return NULL;
}

/**
 * Ajoute un nouveau nœud en tête de la liste chaînée.
 * Initialise les champs de base et définit le statut à NODE_ACTIF.
 * Doit être appelé sous le verrou (mutex) de la table.
 * Retourne le pointeur vers le nouveau nœud.
 */
NodeInfo* node_table_add(NodeTable* table, const char* uuid,
                          const char* ip, int port) {
    NodeInfo* node = calloc(1, sizeof(NodeInfo));
    if (!node) { perror("[NodeTable] calloc"); return NULL; }

    strncpy(node->uuid, uuid, sizeof(node->uuid) - 1);
    strncpy(node->ip,   ip,   sizeof(node->ip)   - 1);
    node->port           = port;
    node->status         = NODE_ACTIF;
    node->last_heartbeat = time(NULL);
    node->hardware.initialized = 0;
    memset(&node->metrics, 0, sizeof(NodeMetrics));

    // Insertion en tête (O(1))
    node->next   = table->head;
    table->head  = node;
    table->count++;
    return node;
}
