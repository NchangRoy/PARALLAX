/*
 * fault_tolerance.h
 * Gestion des pannes pour un cluster de calcul distribué (ENSPY)
 *
 * Ce module couvre les trois rôles de nœuds :
 *   - Nœud Maître       : orchestration, élection, migration de tâches
 *   - Nœud Worker       : exécution locale, surcharge, reconnexion
 *   - Nœud Secondaire   : réplication d'état, promotion en maître
 */

#ifndef FAULT_TOLERANCE_H
#define FAULT_TOLERANCE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ─────────────────────────────────────────────────────────────
 * CONSTANTES CONFIGURABLES
 * ───────────────────────────────────────────────────────────── */

#define MAX_WORKERS             64
#define MAX_TASKS               1024
#define MAX_JOBS                256
#define MAX_TASK_RETRIES        3
#define HEARTBEAT_INTERVAL_MS   1000      /* envoi heartbeat toutes les 1 s    */
#define HEARTBEAT_TIMEOUT_MS    3000      /* nœud mort si absent 3 s           */
#define RECONNECT_DELAY_MS      2000      /* attente entre deux tentatives      */
#define MAX_RECONNECT_ATTEMPTS  5
#define CPU_OVERLOAD_THRESHOLD  0.85f     /* 85 % de charge CPU = surcharge     */
#define MEM_OVERLOAD_THRESHOLD  0.90f     /* 90 % de RAM utilisée = surcharge   */
#define ELECTION_TIMEOUT_MS     5000      /* délai avant déclenchement élection */
#define MAX_PAYLOAD_SIZE        65536     /* taille max payload d'une tâche     */
#define UUID_STR_LEN            37        /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0" */
#define MAX_IP_LEN              46
#define LOG_BUFFER_SIZE         512

/* ─────────────────────────────────────────────────────────────
 * ÉNUMÉRATIONS D'ÉTAT
 * ───────────────────────────────────────────────────────────── */

typedef enum {
    NODE_STATE_DISCONNECTED  = 0,
    NODE_STATE_ACTIVE        = 1,
    NODE_STATE_OVERLOADED    = 2,
    NODE_STATE_FAILED        = 3,
    NODE_STATE_MAINTENANCE   = 4
} NodeState;

typedef enum {
    TASK_STATE_PENDING   = 0,
    TASK_STATE_ASSIGNED  = 1,
    TASK_STATE_RUNNING   = 2,
    TASK_STATE_DONE      = 3,
    TASK_STATE_FAILED    = 4,
    TASK_STATE_MIGRATED  = 5
} TaskState;

typedef enum {
    JOB_STATE_SUBMITTED    = 0,
    JOB_STATE_DECOMPOSING  = 1,
    JOB_STATE_RUNNING      = 2,
    JOB_STATE_DONE         = 3,
    JOB_STATE_FAILED       = 4,
    JOB_STATE_CANCELLED    = 5
} JobState;

typedef enum {
    NODE_ROLE_MASTER    = 0,
    NODE_ROLE_WORKER    = 1,
    NODE_ROLE_SECONDARY = 2
} NodeRole;

typedef enum {
    FAULT_NONE             = 0,
    FAULT_HEARTBEAT_LOSS   = 1,   /* perte de heartbeat                     */
    FAULT_CPU_OVERLOAD     = 2,   /* CPU > seuil                            */
    FAULT_MEM_OVERLOAD     = 3,   /* RAM > seuil                            */
    FAULT_NETWORK_BROKEN   = 4,   /* lien réseau rompu                      */
    FAULT_TASK_TIMEOUT     = 5,   /* tâche dépasse latenceMaxMs             */
    FAULT_TASK_CRASH       = 6,   /* worker crash pendant l'exécution       */
    FAULT_MASTER_DOWN      = 7,   /* maître principal hors-ligne            */
    FAULT_SECONDARY_DOWN   = 8,   /* secondaire hors-ligne                  */
    FAULT_DISK_FULL        = 9,
    FAULT_SOCKET_ERROR     = 10
} FaultType;

typedef enum {
    RECOVERY_RETRY_TASK        = 0,   /* relancer la tâche sur le même worker  */
    RECOVERY_MIGRATE_TASK      = 1,   /* migrer vers un autre worker           */
    RECOVERY_ELECT_NEW_MASTER  = 2,   /* déclencher protocole d'élection       */
    RECOVERY_PROMOTE_SECONDARY = 3,   /* promouvoir le secondaire              */
    RECOVERY_REMOVE_NODE       = 4,   /* retirer le nœud du cluster            */
    RECOVERY_THROTTLE_LOAD     = 5,   /* ne plus envoyer de tâches au nœud     */
    RECOVERY_RECONNECT         = 6    /* tenter une reconnexion                */
} RecoveryAction;

/* ─────────────────────────────────────────────────────────────
 * STRUCTURES DE BASE
 * ───────────────────────────────────────────────────────────── */

/* Vecteur ressources d'un nœud (snapshot matériel) */
typedef struct {
    uint32_t  ram_total_mb;
    uint32_t  ram_available_mb;
    uint8_t   cpu_count;
    float     cpu_load;          /* 0.0 – 1.0                              */
    uint64_t  disk_free_mb;
} ResourceVector;

/* Événement de panne */
typedef struct {
    FaultType      type;
    char           node_uuid[UUID_STR_LEN];
    NodeRole       node_role;
    time_t         timestamp;
    char           detail[LOG_BUFFER_SIZE];
    RecoveryAction suggested_action;
} FaultEvent;

/* Tâche de calcul élémentaire */
typedef struct {
    char        task_id[UUID_STR_LEN];
    char        job_id[UUID_STR_LEN];
    uint8_t     payload[MAX_PAYLOAD_SIZE];
    uint32_t    payload_size;
    int         priority;           /* 1 (haute) → 5 (basse)              */
    TaskState   state;
    char        assigned_worker[UUID_STR_LEN];
    int         retry_count;
    uint64_t    latency_max_ms;
    struct timespec assigned_at;    /* pour détecter les timeouts          */
} Task;

/* Nœud du cluster */
typedef struct {
    char            uuid[UUID_STR_LEN];
    char            name[64];
    char            ip[MAX_IP_LEN];
    uint16_t        port;
    NodeRole        role;
    NodeState       state;
    ResourceVector  resources;
    struct timespec last_heartbeat;
    int             reconnect_attempts;
    int             socket_fd;       /* -1 si non connecté                 */
    pthread_mutex_t lock;            /* accès concurrent protégé           */
} ClusterNode;

/* Contexte complet du nœud local */
typedef struct {
    ClusterNode     self;
    ClusterNode     master;
    ClusterNode     secondary;
    ClusterNode     workers[MAX_WORKERS];
    int             worker_count;
    Task            task_queue[MAX_TASKS];
    int             task_count;
    pthread_mutex_t cluster_lock;
    volatile bool   running;
    /* callbacks injectables par l'application */
    void (*on_fault)(const FaultEvent *);
    void (*on_recovery)(const FaultEvent *, RecoveryAction);
    void (*on_state_change)(const ClusterNode *, NodeState old_state, NodeState new_state);
} NodeContext;

/* ─────────────────────────────────────────────────────────────
 * MACROS UTILITAIRES
 * ───────────────────────────────────────────────────────────── */

/* Copie UUID sans le warning -Wstringop-truncation */
#define FT_COPY_UUID(dst, src) \
    do { memcpy((dst), (src), UUID_STR_LEN - 1); (dst)[UUID_STR_LEN - 1] = '\0'; } while(0)

/* ─────────────────────────────────────────────────────────────
 * API PUBLIQUE
 * ───────────────────────────────────────────────────────────── */

/* Initialisation / destruction */
int  ft_init(NodeContext *ctx, NodeRole role, const char *ip, uint16_t port);
void ft_destroy(NodeContext *ctx);

/* Heartbeat */
int  ft_heartbeat_start(NodeContext *ctx);
void ft_heartbeat_stop(NodeContext *ctx);
void ft_heartbeat_received(NodeContext *ctx, const char *node_uuid);

/* Détection de panne */
void ft_check_all_nodes(NodeContext *ctx);
bool ft_is_node_alive(const ClusterNode *node);
bool ft_is_overloaded(const ClusterNode *node);

/* Récupération — Maître */
int  ft_master_handle_worker_failure(NodeContext *ctx, const char *worker_uuid);
int  ft_master_migrate_tasks(NodeContext *ctx,
                              const char *failed_worker_uuid,
                              const char *target_worker_uuid);
int  ft_master_trigger_election(NodeContext *ctx);
void ft_master_check_task_timeouts(NodeContext *ctx);

/* Récupération — Worker */
int  ft_worker_handle_overload(NodeContext *ctx);
int  ft_worker_reconnect(NodeContext *ctx);
int  ft_worker_retry_task(NodeContext *ctx, Task *task);

/* Récupération — Secondaire */
int  ft_secondary_sync_state(NodeContext *ctx);
int  ft_secondary_promote_to_master(NodeContext *ctx);
int  ft_secondary_handle_master_down(NodeContext *ctx);

/* Utilitaires */
void ft_node_set_state(NodeContext *ctx, ClusterNode *node,
                       NodeState new_state, const char *reason);
void ft_log_fault(NodeContext *ctx, const FaultEvent *evt);
void ft_generate_uuid(char *out);  /* out doit être UUID_STR_LEN octets   */
int64_t ft_elapsed_ms(const struct timespec *since);

#endif /* FAULT_TOLERANCE_H */
