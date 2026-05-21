/*
 * main.c
 * 
 * Point d'entrée du programme - Contrôleur Agent.
 * Ce module initialise l'état des nœuds avec des données de test et démarre le thread
 * Master Servant qui écoute les requêtes du Master Node.
 */

#include <stdio.h>          /* Entrée/sortie standard */
#include <string.h>         /* Fonctions de manipulation de chaînes */
#include <pthread.h>        /* Programmation multi-threads POSIX */
#include "local_state.h"    /* Structures de données pour l'état des nœuds */
#include "master_servant.h" /* Interface du serveur Master Servant */

int main(void) {

    /* Initialisation de Winsock2 sur Windows pour les sockets */
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    /* Initialisation de la structure de stockage local */
    LocalStateStorage state;
    memset(&state, 0, sizeof(state));  /* Réinitialiser toute la mémoire à zéro */
    pthread_mutex_init(&state.lock, NULL);  /* Créer le mutex de synchronisation */

    /* 
     * DONNÉES DE TEST - Simulation des données que le State Receiver (Farelle) remplirait
     * Ceci représente un exemple de cluster avec 3 nœuds en différents états
     */
    pthread_mutex_lock(&state.lock);  /* Verrouiller l'accès au stockage */

    /* Premier nœud : performant et actif */
    strncpy(state.nodes[0].uuid, "aaaa-1111-bbbb-2222-cccc", 36);
    strncpy(state.nodes[0].ip,   "192.168.1.2", 15);
    state.nodes[0].cpu_libre = 0.75f;    /* 75% du CPU disponible */
    state.nodes[0].ram_libre = 0.60f;    /* 60% de la RAM disponible */
    state.nodes[0].cpu_freq  = 2.933f;   /* Fréquence CPU ~2.9 GHz */
    state.nodes[0].latence   = 3.2f;     /* Latence réseau : 3.2ms */
    state.nodes[0].score     = 0.80f;    /* Score de performance bon */
    state.nodes[0].status    = NODE_ACTIF; /* État : actif et disponible */

    /* Deuxième nœud : surchargé avec peu de ressources disponibles */
    strncpy(state.nodes[1].uuid, "dddd-3333-eeee-4444-ffff", 36);
    strncpy(state.nodes[1].ip,   "192.168.1.3", 15);
    state.nodes[1].cpu_libre = 0.30f;      /* Seulement 30% du CPU disponible */
    state.nodes[1].ram_libre = 0.20f;      /* Très peu de RAM (20% disponible) */
    state.nodes[1].cpu_freq  = 2.933f;
    state.nodes[1].latence   = 5.1f;       /* Latence plus élevée */
    state.nodes[1].score     = 0.40f;      /* Score faible */
    state.nodes[1].status    = NODE_SURCHARGE; /* État : surchargé */

    /* Troisième nœud : très performant avec beaucoup de ressources disponibles */
    strncpy(state.nodes[2].uuid, "gggg-5555-hhhh-6666-iiii", 36);
    strncpy(state.nodes[2].ip,   "192.168.1.4", 15);
    state.nodes[2].cpu_libre = 0.90f;      /* 90% du CPU disponible - excellent */
    state.nodes[2].ram_libre = 0.85f;      /* 85% de la RAM disponible - excellent */
    state.nodes[2].cpu_freq  = 1.0f;       /* Fréquence plus basse (possiblement en idle) */
    state.nodes[2].latence   = 8.0f;       /* Latence réseau un peu élevée */
    state.nodes[2].score     = 0.65f;      /* Score moyen-bon malgré la latence */
    state.nodes[2].status    = NODE_ACTIF; /* État : actif et disponible */

    /* Définir le nombre total de nœuds dans le cluster */
    state.node_count = 3;

    pthread_mutex_unlock(&state.lock);  /* Déverrouiller après la populating des données */

    /* LANCEMENT DU SERVEUR */
    /* Créer et lancer le thread Master Servant qui écoute les requêtes */
    pthread_t servant;
    pthread_create(&servant, NULL, master_servant_thread, &state);

    printf("[Main] Controller Agent démarré.\n");

    /* Attendre que le thread servant se termine (en practice, ce sera infini) */
    pthread_join(servant, NULL);

    /* Nettoyage : détruire le mutex */
    pthread_mutex_destroy(&state.lock);

    /* Nettoyage de Winsock sur Windows */
#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}