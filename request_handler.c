/*
 * request_handler.c
 * 
 * Gestion des requêtes provenant du Master Node.
 * Ce module implémente les différents types de requêtes que le Master Node peut envoyer
 * et formule les réponses appropriées contenant les données d'état des nœuds.
 */

#include <stdio.h>         /* Entrée/sortie standard */
#include <string.h>        /* Manipulation de chaînes et comparaisons */
#include <stdint.h>        /* Types entiers de taille fixe (uint8_t, etc.) */
#include "local_state.h"   /* Structures de données */
#include "master_servant.h" /* Interface du serveur */

/* Types de requêtes possibles (identifiants de 1 byte) */
#define REQ_GET_ALL_NODES       0x01  /* Requête : récupérer TOUS les nœuds */
#define REQ_GET_AVAILABLE_NODES 0x02  /* Requête : récupérer les nœuds disponibles (ACTIF) */
#define REQ_GET_NODE_STATUS     0x03  /* Requête : récupérer le statut d'un nœud spécifique */
#define REQ_GET_MONITORING      0x04  /* Requête : récupérer les statistiques de monitoring */


/* Déclarations anticipées des fonctions de traitement spécialisées */
void send_all_nodes(int client_fd, LocalStateStorage *state);
void send_available_nodes(int client_fd, LocalStateStorage *state);
void send_node_status(int client_fd, LocalStateStorage *state);
void send_monitoring_data(int client_fd, LocalStateStorage *state);

/*
 * handle_request()
 * Point d'entrée pour le traitement de toute requête entrante.
 * 
 * Paramètres :
 *   client_fd : Descripteur du socket client
 *   state : Pointeur vers le stockage local des nœuds
 * 
 * Fonction :
 *   - Lire le 1er byte qui identifie le type de requête
 *   - Dispatcher vers le handler approprié selon le type
 */
void handle_request(int client_fd, LocalStateStorage *state) {
    uint8_t req_type;
    recv(client_fd, &req_type, 1, 0);  /* Recevoir l'identificateur de requête (1 byte) */

    /* Dispatcher selon le type de requête */
    switch (req_type) {
        case REQ_GET_ALL_NODES:
            send_all_nodes(client_fd, state);
            break;
        case REQ_GET_AVAILABLE_NODES:
            send_available_nodes(client_fd, state);
            break;
        case REQ_GET_NODE_STATUS:
            send_node_status(client_fd, state);
            break;
        case REQ_GET_MONITORING:
            send_monitoring_data(client_fd, state);
            break;
        default:
            printf("[MasterServant] Requête inconnue: 0x%02X\n", req_type);
    }
}

/*
 * send_all_nodes()
 * Envoie TOUS les nœuds du cluster au client.
 * 
 * Paramètres :
 *   client_fd : Socket pour envoyer les données
 *   state : État contenant tous les nœuds
 * 
 * Protocole de réponse :
 *   - 1 byte : Nombre de nœuds (count)
 *   - N lignes : Chaque nœud formaté comme "UUID:...|IP:...|CPU:...|RAM:...|SCORE:...|STATUS:...\n"
 */
void send_all_nodes(int client_fd, LocalStateStorage *state) {
    char buffer[4096];
    int  offset = 0;

    pthread_mutex_lock(&state->lock);  /* Verrouiller pour accès thread-safe */

    uint8_t count = (uint8_t)state->node_count;  /* Nombre de nœuds */

    /* Itérer sur tous les nœuds et les formater */
    for (int i = 0; i < state->node_count; i++) {
        NodeState *n = &state->nodes[i];
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
            "UUID:%s|IP:%s|CPU:%.2f|RAM:%.2f|SCORE:%.2f|STATUS:%d\n",
            n->uuid, n->ip, n->cpu_libre, n->ram_libre, n->score, n->status);
    }

    pthread_mutex_unlock(&state->lock);  /* Déverrouiller */

    /* Envoyer au client : d'abord le nombre de nœuds, puis les données */
    send(client_fd, &count, 1, 0);
    send(client_fd, buffer, offset, 0);
}

/*
 * send_available_nodes()
 * Envoie uniquement les nœuds DISPONIBLES (état NODE_ACTIF) du cluster.
 * Filtre les nœuds qui ne sont pas en état NODE_ACTIF.
 * 
 * Paramètres :
 *   client_fd : Socket pour envoyer les données
 *   state : État contenant tous les nœuds
 * 
 * Protocole de réponse :
 *   - 1 byte : Nombre de nœuds disponibles
 *   - N lignes : Chaque nœud disponible formaté
 */
void send_available_nodes(int client_fd, LocalStateStorage *state) {
    char buffer[4096];
    int  offset = 0;

    pthread_mutex_lock(&state->lock);  /* Verrouiller pour accès thread-safe */

    int count = 0;  /* Compteur des nœuds disponibles */
    
    /* Itérer sur tous les nœuds et filtrer les disponibles */
    for (int i = 0; i < state->node_count; i++) {
        NodeState *n = &state->nodes[i];
        if (n->status != NODE_ACTIF) continue;  /* Ignorer les nœuds non-actifs */
        
        /* Formater et ajouter le nœud au buffer */
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
            "UUID:%s|IP:%s|CPU:%.2f|RAM:%.2f|SCORE:%.2f\n",
            n->uuid, n->ip, n->cpu_libre, n->ram_libre, n->score);
        count++;
    }

    pthread_mutex_unlock(&state->lock);  /* Déverrouiller */

    /* Envoyer au client : nombre de nœuds disponibles, puis les données */
    uint8_t n_count = (uint8_t)count;
    send(client_fd, &n_count, 1, 0);
    send(client_fd, buffer, offset, 0);
}

/*
 * send_node_status()
 * Envoie le statut d'un nœud spécifique identifié par son UUID.
 * 
 * Paramètres :
 *   client_fd : Socket pour envoyer les données
 *   state : État contenant tous les nœuds
 * 
 * Protocole de requête :
 *   - 36 bytes : UUID du nœud demandé
 * 
 * Protocole de réponse :
 *   - Si trouvé : ligne "UUID:...|IP:...|CPU:...|RAM:...|SCORE:...|STATUS:...\n"
 *   - Si non trouvé : "ERROR:NODE_NOT_FOUND\n"
 */
void send_node_status(int client_fd, LocalStateStorage *state) {
    char uuid_req[37];  /* Buffer pour stocker l'UUID demandé (36 + null terminator) */
    char buffer[512];
    int  offset = 0;

    /* Recevoir l'UUID du nœud demandé depuis le client (36 bytes) */
    recv(client_fd, uuid_req, 36, 0);
    uuid_req[36] = '\0';  /* Ajouter le null terminator */

    pthread_mutex_lock(&state->lock);  /* Verrouiller pour accès thread-safe */

    int found = 0;  /* Drapeau : 1 si le nœud est trouvé */
    
    /* Rechercher le nœud avec l'UUID correspondant */
    for (int i = 0; i < state->node_count; i++) {
        NodeState *n = &state->nodes[i];
        if (strncmp(n->uuid, uuid_req, 36) == 0) {
            /* UUID trouvé ! Formater les données du nœud */
            offset += snprintf(buffer, sizeof(buffer),
                "UUID:%s|IP:%s|CPU:%.2f|RAM:%.2f|SCORE:%.2f|STATUS:%d\n",
                n->uuid, n->ip, n->cpu_libre, n->ram_libre, n->score, n->status);
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&state->lock);  /* Déverrouiller */

    /* Si le nœud n'a pas été trouvé, envoyer un message d'erreur */
    if (!found) {
        snprintf(buffer, sizeof(buffer), "ERROR:NODE_NOT_FOUND\n");
        offset = strlen(buffer);
    }

    /* Envoyer la réponse au client */
    send(client_fd, buffer, offset, 0);
}

/*
 * send_monitoring_data()
 * Envoie les statistiques globales de monitoring du cluster.
 * Compte les nœuds par état et envoie un résumé au client.
 * 
 * Paramètres :
 *   client_fd : Socket pour envoyer les données
 *   state : État contenant tous les nœuds
 * 
 * Protocole de réponse :
 *   - Une ligne : "TOTAL:X|ACTIF:X|SUSPECTED:X|FAILED:X|SURCHARGE:X\n"
 *     où X sont les comptes de nœuds dans chaque état
 */
void send_monitoring_data(int client_fd, LocalStateStorage *state) {
    char buffer[4096];
    int  offset = 0;

    pthread_mutex_lock(&state->lock);  /* Verrouiller pour accès thread-safe */

    /* Compteurs pour chaque état de nœud */
    int actif = 0, suspected = 0, failed = 0, surcharge = 0;

    /* Itérer sur tous les nœuds et compter par état */
    for (int i = 0; i < state->node_count; i++) {
        switch (state->nodes[i].status) {
            case NODE_ACTIF:
                actif++;
                break;
            case NODE_SUSPECTED:
                suspected++;
                break;
            case NODE_FAILED:
                failed++;
                break;
            case NODE_SURCHARGE:
                surcharge++;
                break;
        }
    }

    /* Formater les statistiques dans le buffer */
    offset += snprintf(buffer, sizeof(buffer),
        "TOTAL:%d|ACTIF:%d|SUSPECTED:%d|FAILED:%d|SURCHARGE:%d\n",
        state->node_count, actif, suspected, failed, surcharge);

    pthread_mutex_unlock(&state->lock);  /* Déverrouiller */

    /* Envoyer les statistiques au client */
    send(client_fd, buffer, offset, 0);
}