/*
 * master_servant.c
 * 
 * Implémentation du serveur Master Servant.
 * Ce module est responsable de l'écoute des connexions entrantes et de la répartition
 * des requêtes vers les handlers appropriés.
 */

#include <stdio.h>         /* Entrée/sortie standard */
#include <stdlib.h>        /* Fonctions de la libraire standard */
#include <string.h>        /* Manipulation de chaînes */
#include "master_servant.h" /* Interface du module */
#include "local_state.h"    /* Structures de données */

/* Port d'écoute pour les requêtes du Master Node */
#define PORT 9090

/*
 * master_servant_thread()
 * Fonction principale du serveur Master Servant.
 * Crée un socket serveur TCP, l'attache au port défini et écoute les connexions.
 */
void *master_servant_thread(void *arg) {
    LocalStateStorage *state = (LocalStateStorage *)arg;  /* Cast du paramètre */

    /* Initialisation de Winsock2 pour Windows */
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    /* Créer le socket serveur TCP (AF_INET=IPv4, SOCK_STREAM=TCP) */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    /* Configuration de la structure d'adresse socket */
    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;        /* Famille : IPv4 */
    addr.sin_port        = htons(PORT);    /* Port en format réseau (big-endian) */
    addr.sin_addr.s_addr = INADDR_ANY;     /* Écouter sur toutes les interfaces (0.0.0.0) */

    /* Attacher le socket au port */
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    
    /* Mettre le socket en mode écoute avec une queue de 5 connexions */
    listen(server_fd, 5);

    printf("[MasterServant] En écoute sur le port %d\n", PORT);

    /* Boucle principale d'acceptation des connexions */
    while (1) {
        /* Accepter une connexion entrante */
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;  /* Ignorer les erreurs d'acceptation */

        /* Traiter la requête du client */
        handle_request(client_fd, state);

        /* Fermer le socket client (dépendant de l'OS) */
#ifdef _WIN32
        closesocket(client_fd);  /* Windows */
#else
        close(client_fd);        /* Unix/Linux */
#endif
    }

    /* Nettoyage (code non-atteint en pratique du fait de la boucle infinie) */
#ifdef _WIN32
    closesocket(server_fd);  /* Fermer le socket serveur sur Windows */
    WSACleanup();            /* Nettoyer Winsock */
#else
    close(server_fd);        /* Fermer le socket serveur sur Unix/Linux */
#endif

    return NULL;  /* Fin du thread */
}