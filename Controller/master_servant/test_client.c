/*
 * test_client.c
 * 
 * Client de test pour le serveur Master Servant.
 * Ce programme teste les 4 types de requêtes supportées par le serveur.
 * À exécuter APRÈS le démarrage du serveur (main.c).
 */

#include <stdio.h>      /* Entrée/sortie standard */
#include <string.h>     /* Manipulation de chaînes */
#include <stdint.h>     /* Types entiers de taille fixe */

/* Inclusions conditionnelles des headers socket en fonction de l'OS */
#ifdef _WIN32
    #include <winsock2.h>     /* API Winsock2 pour Windows */
    #include <ws2tcpip.h>     /* Fonctions IPv6 et gestion d'adresses */
#else
    #include <sys/socket.h>   /* API sockets POSIX */
    #include <netinet/in.h>   /* Structures pour protocoles Internet */
    #include <arpa/inet.h>    /* Conversion d'adresses entre formats */
    #include <unistd.h>       /* Appels système POSIX */
#endif

#define PORT 9090              /* Port du serveur Master Servant */
#define SERVER_IP "127.0.0.1"  /* Adresse IP du serveur (localhost) */

/*
 * connect_to_server()
 * Établit une connexion TCP au serveur Master Servant.
 * 
 * Retour :
 *   Descripteur de socket connecté au serveur
 * 
 * Fonction :
 *   - Crée un socket TCP
 *   - Configure l'adresse serveur (IP et port)
 *   - Se connecte au serveur
 *   - Retourne le socket pour communication ultérieure
 */
int connect_to_server() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);  /* Créer socket TCP */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;  /* IPv4 */
    addr.sin_port   = htons(PORT);  /* Port en format réseau */
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);  /* Convertir IP string en binaire */
    connect(sock, (struct sockaddr *)&addr, sizeof(addr));  /* Se connecter */
    return sock;  /* Retourner le socket connecté */
}

/*
 * test_get_all_nodes()
 * Test de la requête GET_ALL_NODES (0x01).
 * Récupère et affiche TOUS les nœuds du cluster.
 */
void test_get_all_nodes() {
    printf("\n--- TEST : GET_ALL_NODES ---\n");
    int sock = connect_to_server();  /* Établir la connexion au serveur */

    /* Envoyer la requête GET_ALL_NODES */
    uint8_t req = 0x01;  /* Type de requête : GET_ALL_NODES */
    send(sock, &req, 1, 0);  /* Envoyer le byte d'identificateur */

    /* Recevoir le nombre de nœuds */
    uint8_t count;
    recv(sock, &count, 1, 0);
    printf("Nombre de noeuds : %d\n", count);

    /* Recevoir et afficher les données des nœuds */
    char buffer[4096] = {0};
    recv(sock, buffer, sizeof(buffer), 0);
    printf("%s\n", buffer);

    /* Fermer la connexion (dépendant de l'OS) */
#ifdef _WIN32
    closesocket(sock);  /* Windows */
#else
    close(sock);        /* Unix/Linux */
#endif
}

/*
 * test_get_available_nodes()
 * Test de la requête GET_AVAILABLE_NODES (0x02).
 * Récupère et affiche uniquement les nœuds DISPONIBLES (état NODE_ACTIF).
 */
void test_get_available_nodes() {
    printf("\n--- TEST : GET_AVAILABLE_NODES ---\n");
    int sock = connect_to_server();  /* Établir la connexion au serveur */

    /* Envoyer la requête GET_AVAILABLE_NODES */
    uint8_t req = 0x02;  /* Type de requête : GET_AVAILABLE_NODES */
    send(sock, &req, 1, 0);  /* Envoyer le byte d'identificateur */

    /* Recevoir le nombre de nœuds disponibles */
    uint8_t count;
    recv(sock, &count, 1, 0);
    printf("Noeuds disponibles : %d\n", count);

    /* Recevoir et afficher les données des nœuds disponibles */
    char buffer[4096] = {0};
    recv(sock, buffer, sizeof(buffer), 0);
    printf("%s\n", buffer);

    /* Fermer la connexion (dépendant de l'OS) */
#ifdef _WIN32
    closesocket(sock);  /* Windows */
#else
    close(sock);        /* Unix/Linux */
#endif
}

/*
 * test_get_monitoring()
 * Test de la requête GET_MONITORING (0x04).
 * Récupère et affiche les statistiques de monitoring globales du cluster.
 */
void test_get_monitoring() {
    printf("\n--- TEST : GET_MONITORING_DATA ---\n");
    int sock = connect_to_server();  /* Établir la connexion au serveur */

    /* Envoyer la requête GET_MONITORING */
    uint8_t req = 0x04;  /* Type de requête : GET_MONITORING */
    send(sock, &req, 1, 0);  /* Envoyer le byte d'identificateur */

    /* Recevoir et afficher les données de monitoring */
    char buffer[512] = {0};
    recv(sock, buffer, sizeof(buffer), 0);
    printf("%s\n", buffer);

    /* Fermer la connexion (dépendant de l'OS) */
#ifdef _WIN32
    closesocket(sock);  /* Windows */
#else
    close(sock);        /* Unix/Linux */
#endif
}

/*
 * main()
 * Fonction principale du client de test.
 * Lance les 3 tests de requêtes supportées par le serveur.
 * Exécution : démarrer d'abord le serveur (main.c), puis ce client.
 */
int main(void) {
    /* Initialisation de Winsock2 sur Windows */
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    /* Exécuter les tests de requêtes */
    test_get_all_nodes();           /* Test 1 : Récupérer TOUS les nœuds */
    test_get_available_nodes();     /* Test 2 : Récupérer les nœuds disponibles */
    test_get_monitoring();          /* Test 3 : Récupérer les statistiques */

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}