/*
 * local_state.h
 * 
 * En-tête définissant les structures de données pour stocker l'état local des nœuds.
 * Ce module représente le stockage central des informations sur tous les nœuds du cluster.
 * Les données sont protégées par un mutex pour assurer la thread-safety.
 */

#ifndef LOCAL_STATE_H
#define LOCAL_STATE_H

#include <time.h>      /* Pour time_t et les fonctions de gestion du temps */
#include <pthread.h>   /* Pour les mutexes de synchronisation inter-threads */

/* Inclusion conditionnelle des headers socket en fonction de l'OS */
#ifdef _WIN32
    #include <winsock2.h>     /* API Winsock2 pour Windows */
    #include <ws2tcpip.h>     /* Support IPv6 et TCP/IP sur Windows */
#else
    #include <sys/socket.h>   /* API sockets POSIX */
    #include <netinet/in.h>   /* Structures pour protocoles Internet */
    #include <unistd.h>       /* Appels système POSIX */
#endif

/* Nombre maximum de nœuds pouvant être gérés dans le cluster */
#define MAX_NODES 32

/* Énumération des états possibles d'un nœud dans le cluster */
typedef enum {
    NODE_ACTIF,       /* Le nœud fonctionne normalement et est disponible */
    NODE_SUSPECTED,   /* Le nœud est suspecté d'être défaillant */
    NODE_FAILED,      /* Le nœud a échoué et n'est pas disponible */
    NODE_SURCHARGE    /* Le nœud est surchargé (CPU/RAM élevé, faible disponibilité) */
} NodeStatus;

/* Structure représentant l'état d'un nœud du cluster */
typedef struct {
    char        uuid[37];        /* Identifiant unique du nœud (UUID format) */
    char        ip[16];          /* Adresse IP du nœud (format dotted quad XXX.XXX.XXX.XXX) */
    float       cpu_libre;       /* Pourcentage de CPU disponible (0.0 à 1.0) */
    float       ram_libre;       /* Pourcentage de RAM disponible (0.0 à 1.0) */
    float       cpu_freq;        /* Fréquence CPU en GHz */
    float       latence;         /* Latence réseau en millisecondes */
    float       score;           /* Score de performance du nœud (0.0 à 1.0) */
    NodeStatus  status;          /* État actuel du nœud */
    time_t      last_heartbeat;  /* Timestamp du dernier heartbeat reçu */
} NodeState;

/* Structure représentant le stockage local central de tous les nœuds */
typedef struct {
    NodeState       nodes[MAX_NODES];   /* Tableau de tous les nœuds gérés */
    int             node_count;         /* Nombre actuel de nœuds en service */
    pthread_mutex_t lock;               /* Mutex pour protéger l'accès concurrent aux données */
} LocalStateStorage;

#endif