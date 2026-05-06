/*
 * fault_tolerance.c
 * Implémentation du module de gestion des pannes
 * Parties : utilitaires, initialisation, heartbeat, détection
 */

#include "fault_tolerance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ─────────────────────────────────────────────────────────────
 * SECTION 1 — UTILITAIRES INTERNES
 * ───────────────────────────────────────────────────────────── */

/*
 * ft_elapsed_ms
 * Retourne le nombre de millisecondes écoulées depuis 'since'.
 * Utilise CLOCK_MONOTONIC pour éviter les sauts d'horloge système.
 */
int64_t ft_elapsed_ms(const struct timespec *since)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int64_t diff_s  = (int64_t)(now.tv_sec  - since->tv_sec);
    int64_t diff_ns = (int64_t)(now.tv_nsec - since->tv_nsec);
    return diff_s * 1000 + diff_ns / 1000000;
}

/*
 * ft_sleep_ms
 * Sommeil portable en millisecondes, robuste aux interruptions EINTR.
 */
static void ft_sleep_ms(uint32_t ms)
{
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    /* nanosleep peut être interrompu par un signal : relancer */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/*
 * ft_generate_uuid
 * Génère un UUID v4 pseudo-aléatoire à partir de /dev/urandom.
 * Format : xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 */
void ft_generate_uuid(char *out)
{
    uint8_t rnd[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
        /* Fallback : rand() si /dev/urandom indisponible */
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)rand();
    }
    if (fd >= 0) close(fd);

    /* Ajustement des bits version/variant UUID v4 */
    rnd[6] = (rnd[6] & 0x0F) | 0x40;
    rnd[8] = (rnd[8] & 0x3F) | 0x80;

    snprintf(out, UUID_STR_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             rnd[0],  rnd[1],  rnd[2],  rnd[3],
             rnd[4],  rnd[5],
             rnd[6],  rnd[7],
             rnd[8],  rnd[9],
             rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
}


static const char *fault_type_str(FaultType t)
{
    switch (t) {
        case FAULT_HEARTBEAT_LOSS:  return "HEARTBEAT_LOSS";
        case FAULT_CPU_OVERLOAD:    return "CPU_OVERLOAD";
        case FAULT_MEM_OVERLOAD:    return "MEM_OVERLOAD";
        case FAULT_NETWORK_BROKEN:  return "NETWORK_BROKEN";
        case FAULT_TASK_TIMEOUT:    return "TASK_TIMEOUT";
        case FAULT_TASK_CRASH:      return "TASK_CRASH";
        case FAULT_MASTER_DOWN:     return "MASTER_DOWN";
        case FAULT_SECONDARY_DOWN:  return "SECONDARY_DOWN";
        case FAULT_DISK_FULL:       return "DISK_FULL";
        case FAULT_SOCKET_ERROR:    return "SOCKET_ERROR";
        default:                    return "UNKNOWN";
    }
}

static const char *recovery_str(RecoveryAction a)
{
    switch (a) {
        case RECOVERY_RETRY_TASK:        return "RETRY_TASK";
        case RECOVERY_MIGRATE_TASK:      return "MIGRATE_TASK";
        case RECOVERY_ELECT_NEW_MASTER:  return "ELECT_NEW_MASTER";
        case RECOVERY_PROMOTE_SECONDARY: return "PROMOTE_SECONDARY";
        case RECOVERY_REMOVE_NODE:       return "REMOVE_NODE";
        case RECOVERY_THROTTLE_LOAD:     return "THROTTLE_LOAD";
        case RECOVERY_RECONNECT:         return "RECONNECT";
        default:                         return "UNKNOWN";
    }
}

static const char *node_role_str(NodeRole r)
{
    switch (r) {
        case NODE_ROLE_MASTER:    return "MASTER";
        case NODE_ROLE_WORKER:    return "WORKER";
        case NODE_ROLE_SECONDARY: return "SECONDARY";
        default:                  return "UNKNOWN";
    }
}

static const char *node_state_str(NodeState s)
{
    switch (s) {
        case NODE_STATE_DISCONNECTED: return "DISCONNECTED";
        case NODE_STATE_ACTIVE:       return "ACTIVE";
        case NODE_STATE_OVERLOADED:   return "OVERLOADED";
        case NODE_STATE_FAILED:       return "FAILED";
        case NODE_STATE_MAINTENANCE:  return "MAINTENANCE";
        default:                      return "UNKNOWN";
    }
}

/*
 * ft_log_fault
 * Journalise une panne sur stderr avec horodatage ISO 8601.
 * Appelle le callback on_fault si enregistré.
 */
void ft_log_fault(NodeContext *ctx, const FaultEvent *evt)
{
    char timebuf[32];
    struct tm *tm_info = localtime(&evt->timestamp);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", tm_info);

    fprintf(stderr,
            "[%s] FAULT %-18s | role=%-9s | node=%s | detail=%s | recovery=%s\n",
            timebuf,
            fault_type_str(evt->type),
            node_role_str(evt->node_role),
            evt->node_uuid,
            evt->detail,
            recovery_str(evt->suggested_action));

    if (ctx->on_fault)
        ctx->on_fault(evt);
}

/* ─────────────────────────────────────────────────────────────
 * SECTION 3 — CHANGEMENT D'ÉTAT D'UN NŒUD
 * ───────────────────────────────────────────────────────────── */

/*
 * ft_node_set_state
 * Transition atomique d'état avec log et callback.
 * La transition est ignorée si l'état est déjà identique.
 */
void ft_node_set_state(NodeContext *ctx, ClusterNode *node,
                       NodeState new_state, const char *reason)
{
    pthread_mutex_lock(&node->lock);
    NodeState old_state = node->state;

    if (old_state == new_state) {
        pthread_mutex_unlock(&node->lock);
        return;
    }

    node->state = new_state;
    pthread_mutex_unlock(&node->lock);

    fprintf(stderr,
            "[STATE] node=%s role=%-9s  %s → %s  reason=%s\n",
            node->uuid,
            node_role_str(node->role),
            node_state_str(old_state),
            node_state_str(new_state),
            reason ? reason : "n/a");

    if (ctx->on_state_change)
        ctx->on_state_change(node, old_state, new_state);
}

/* ─────────────────────────────────────────────────────────────
 * SECTION 4 — INITIALISATION / DESTRUCTION
 * ───────────────────────────────────────────────────────────── */

/*
 * ft_init
 * Initialise le contexte du nœud local.
 * - Génère un UUID unique
 * - Positionne l'état initial
 * - Initialise les mutex
 */
int ft_init(NodeContext *ctx, NodeRole role, const char *ip, uint16_t port)
{
    if (!ctx || !ip) return -1;

    memset(ctx, 0, sizeof(NodeContext));

    ft_generate_uuid(ctx->self.uuid);
    snprintf(ctx->self.name, sizeof(ctx->self.name), "node-%s", ctx->self.uuid);
    strncpy(ctx->self.ip, ip, MAX_IP_LEN - 1);
    ctx->self.port       = port;
    ctx->self.role       = role;
    ctx->self.state      = NODE_STATE_DISCONNECTED;
    ctx->self.socket_fd  = -1;
    ctx->running         = true;

    /* Initialisation de toutes les ressources à zéro */
    clock_gettime(CLOCK_MONOTONIC, &ctx->self.last_heartbeat);

    if (pthread_mutex_init(&ctx->self.lock, NULL) != 0) {
        perror("pthread_mutex_init(self.lock)");
        return -1;
    }
    if (pthread_mutex_init(&ctx->cluster_lock, NULL) != 0) {
        perror("pthread_mutex_init(cluster_lock)");
        pthread_mutex_destroy(&ctx->self.lock);
        return -1;
    }

    /* Initialisation des mutex de chaque slot worker */
    for (int i = 0; i < MAX_WORKERS; i++) {
        ctx->workers[i].socket_fd = -1;
        if (pthread_mutex_init(&ctx->workers[i].lock, NULL) != 0) {
            /* Nettoyage en cas d'échec partiel */
            for (int j = 0; j < i; j++)
                pthread_mutex_destroy(&ctx->workers[j].lock);
            pthread_mutex_destroy(&ctx->cluster_lock);
            pthread_mutex_destroy(&ctx->self.lock);
            return -1;
        }
    }

    fprintf(stderr,
            "[INIT] Node UUID=%s role=%s ip=%s port=%u\n",
            ctx->self.uuid, node_role_str(role), ip, port);
    return 0;
}

/*
 * ft_destroy
 * Libère toutes les ressources du contexte.
 */
void ft_destroy(NodeContext *ctx)
{
    if (!ctx) return;

    ctx->running = false;

    /* Fermeture des sockets ouvertes */
    if (ctx->self.socket_fd >= 0) {
        close(ctx->self.socket_fd);
        ctx->self.socket_fd = -1;
    }
    for (int i = 0; i < ctx->worker_count; i++) {
        if (ctx->workers[i].socket_fd >= 0) {
            close(ctx->workers[i].socket_fd);
            ctx->workers[i].socket_fd = -1;
        }
        pthread_mutex_destroy(&ctx->workers[i].lock);
    }

    pthread_mutex_destroy(&ctx->cluster_lock);
    pthread_mutex_destroy(&ctx->self.lock);
}

/* ─────────────────────────────────────────────────────────────
 * SECTION 5 — HEARTBEAT
 * ───────────────────────────────────────────────────────────── */

/*
 * heartbeat_send_loop
 * Thread d'envoi de heartbeat : s'exécute toutes les
 * HEARTBEAT_INTERVAL_MS ms tant que le contexte tourne.
 * Envoie simplement l'UUID local via UDP (remplaçable par gRPC).
 */
static void *heartbeat_send_loop(void *arg)
{
    NodeContext *ctx = (NodeContext *)arg;

    /* Création d'un socket UDP pour l'envoi */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("heartbeat socket");
        return NULL;
    }

    while (ctx->running) {
        /* -- envoi au maître -- */
        if (ctx->self.role != NODE_ROLE_MASTER &&
            ctx->master.state == NODE_STATE_ACTIVE)
        {
            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family      = AF_INET;
            dest.sin_port        = htons(ctx->master.port);
            inet_pton(AF_INET, ctx->master.ip, &dest.sin_addr);

            /* Le payload heartbeat = UUID (37 octets) */
            sendto(sock, ctx->self.uuid, UUID_STR_LEN, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
        }

        /* Mise à jour du propre heartbeat local */
        clock_gettime(CLOCK_MONOTONIC, &ctx->self.last_heartbeat);

        ft_sleep_ms(HEARTBEAT_INTERVAL_MS);
    }

    close(sock);
    return NULL;
}

/*
 * heartbeat_check_loop
 * Thread de surveillance : vérifie périodiquement que chaque nœud
 * connu a bien envoyé un heartbeat récent.
 * Si ce n'est pas le cas → ft_check_all_nodes() est appelé.
 */
static void *heartbeat_check_loop(void *arg)
{
    NodeContext *ctx = (NodeContext *)arg;

    while (ctx->running) {
        ft_sleep_ms(HEARTBEAT_INTERVAL_MS);
        ft_check_all_nodes(ctx);
    }
    return NULL;
}

static pthread_t g_hb_send_thread;
static pthread_t g_hb_check_thread;

int ft_heartbeat_start(NodeContext *ctx)
{
    if (pthread_create(&g_hb_send_thread, NULL, heartbeat_send_loop, ctx)) {
        perror("pthread_create heartbeat_send");
        return -1;
    }
    if (pthread_create(&g_hb_check_thread, NULL, heartbeat_check_loop, ctx)) {
        perror("pthread_create heartbeat_check");
        ctx->running = false;
        pthread_join(g_hb_send_thread, NULL);
        return -1;
    }
    pthread_detach(g_hb_send_thread);
    pthread_detach(g_hb_check_thread);
    fprintf(stderr, "[HEARTBEAT] Surveillance démarrée (interval=%d ms, timeout=%d ms)\n",
            HEARTBEAT_INTERVAL_MS, HEARTBEAT_TIMEOUT_MS);
    return 0;
}

void ft_heartbeat_stop(NodeContext *ctx)
{
    ctx->running = false;
}

/*
 * ft_heartbeat_received
 * Appelé par la couche réseau lorsqu'un heartbeat arrive.
 * Met à jour le timestamp du nœud émetteur.
 */
void ft_heartbeat_received(NodeContext *ctx, const char *node_uuid)
{
    if (!ctx || !node_uuid) return;

    pthread_mutex_lock(&ctx->cluster_lock);

    for (int i = 0; i < ctx->worker_count; i++) {
        if (strncmp(ctx->workers[i].uuid, node_uuid, UUID_STR_LEN) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &ctx->workers[i].last_heartbeat);

            /* Si le nœud revenait d'une panne → le marquer actif */
            if (ctx->workers[i].state == NODE_STATE_FAILED ||
                ctx->workers[i].state == NODE_STATE_DISCONNECTED)
            {
                pthread_mutex_unlock(&ctx->cluster_lock);
                ft_node_set_state(ctx, &ctx->workers[i],
                                  NODE_STATE_ACTIVE,
                                  "heartbeat_received_after_failure");
                return;
            }
            break;
        }
    }

    /* Vérifier aussi le secondaire */
    if (strncmp(ctx->secondary.uuid, node_uuid, UUID_STR_LEN) == 0)
        clock_gettime(CLOCK_MONOTONIC, &ctx->secondary.last_heartbeat);

    pthread_mutex_unlock(&ctx->cluster_lock);
}

/* ─────────────────────────────────────────────────────────────
 * SECTION 6 — DÉTECTION DE PANNES
 * ───────────────────────────────────────────────────────────── */

/*
 * ft_is_node_alive
 * Retourne true si le dernier heartbeat du nœud est dans le délai.
 */
bool ft_is_node_alive(const ClusterNode *node)
{
    if (!node) return false;
    if (node->state == NODE_STATE_MAINTENANCE) return true; /* pas surveillé */

    int64_t elapsed = ft_elapsed_ms(&node->last_heartbeat);
    return elapsed < (int64_t)HEARTBEAT_TIMEOUT_MS;
}

/*
 * ft_is_overloaded
 * Retourne true si le nœud dépasse l'un des seuils de charge.
 */
bool ft_is_overloaded(const ClusterNode *node)
{
    if (!node) return false;
    return (node->resources.cpu_load > CPU_OVERLOAD_THRESHOLD) ||
           ((float)node->resources.ram_available_mb /
            (float)(node->resources.ram_total_mb + 1)
            < (1.0f - MEM_OVERLOAD_THRESHOLD));
}

/*
 * ft_check_all_nodes
 * Parcourt tous les nœuds connus et déclenche les actions de
 * récupération appropriées selon le rôle du nœud défaillant.
 * Appelé périodiquement par heartbeat_check_loop.
 */
void ft_check_all_nodes(NodeContext *ctx)
{
    if (!ctx) return;

    /* ── Vérification des workers (vu du maître ou du secondaire) ── */
    pthread_mutex_lock(&ctx->cluster_lock);
    int wcount = ctx->worker_count;
    pthread_mutex_unlock(&ctx->cluster_lock);

    for (int i = 0; i < wcount; i++) {

        ClusterNode *w = &ctx->workers[i];
        pthread_mutex_lock(&w->lock);
        NodeState ws = w->state;
        bool alive    = ft_is_node_alive(w);
        bool overload = ft_is_overloaded(w);
        pthread_mutex_unlock(&w->lock);

        if (ws == NODE_STATE_FAILED || ws == NODE_STATE_DISCONNECTED)
            continue;  /* déjà signalé */

        if (!alive && ws == NODE_STATE_ACTIVE) {
            /* Panne : heartbeat manquant */
            FaultEvent evt = {
                .type             = FAULT_HEARTBEAT_LOSS,
                .node_role        = NODE_ROLE_WORKER,
                .timestamp        = time(NULL),
                .suggested_action = RECOVERY_MIGRATE_TASK
            };
            FT_COPY_UUID(evt.node_uuid, w->uuid);
            snprintf(evt.detail, LOG_BUFFER_SIZE,
                     "Heartbeat absent depuis >%d ms", HEARTBEAT_TIMEOUT_MS);

            ft_node_set_state(ctx, w, NODE_STATE_FAILED, "heartbeat_timeout");
            ft_log_fault(ctx, &evt);

            if (ctx->self.role == NODE_ROLE_MASTER)
                ft_master_handle_worker_failure(ctx, w->uuid);

        } else if (overload && ws == NODE_STATE_ACTIVE) {
            /* Surcharge détectée */
            FaultType ft = (w->resources.cpu_load > CPU_OVERLOAD_THRESHOLD)
                           ? FAULT_CPU_OVERLOAD : FAULT_MEM_OVERLOAD;
            FaultEvent evt = {
                .type             = ft,
                .node_role        = NODE_ROLE_WORKER,
                .timestamp        = time(NULL),
                .suggested_action = RECOVERY_THROTTLE_LOAD
            };
            FT_COPY_UUID(evt.node_uuid, w->uuid);
            snprintf(evt.detail, LOG_BUFFER_SIZE,
                     "cpu=%.1f%% ram_used=%uMB/%uMB",
                     w->resources.cpu_load * 100.0f,
                     w->resources.ram_total_mb - w->resources.ram_available_mb,
                     w->resources.ram_total_mb);

            ft_node_set_state(ctx, w, NODE_STATE_OVERLOADED, "resource_threshold");
            ft_log_fault(ctx, &evt);
        }
    }

    /* ── Vérification du maître (vu du secondaire) ── */
    if (ctx->self.role == NODE_ROLE_SECONDARY) {
        bool master_alive = ft_is_node_alive(&ctx->master);

        if (!master_alive &&
            ctx->master.state != NODE_STATE_FAILED &&
            ctx->master.state != NODE_STATE_DISCONNECTED)
        {
            FaultEvent evt = {
                .type             = FAULT_MASTER_DOWN,
                .node_role        = NODE_ROLE_MASTER,
                .timestamp        = time(NULL),
                .suggested_action = RECOVERY_PROMOTE_SECONDARY
            };
            FT_COPY_UUID(evt.node_uuid, ctx->master.uuid);
            snprintf(evt.detail, LOG_BUFFER_SIZE,
                     "Maître injoignable depuis >%d ms", HEARTBEAT_TIMEOUT_MS);

            ft_node_set_state(ctx, &ctx->master,
                              NODE_STATE_FAILED, "master_heartbeat_timeout");
            ft_log_fault(ctx, &evt);
            ft_secondary_handle_master_down(ctx);
        }
    }

    /* ── Vérification du secondaire (vu du maître) ── */
    if (ctx->self.role == NODE_ROLE_MASTER) {
        bool sec_alive = ft_is_node_alive(&ctx->secondary);

        if (!sec_alive &&
            ctx->secondary.state == NODE_STATE_ACTIVE)
        {
            FaultEvent evt = {
                .type             = FAULT_SECONDARY_DOWN,
                .node_role        = NODE_ROLE_SECONDARY,
                .timestamp        = time(NULL),
                .suggested_action = RECOVERY_ELECT_NEW_MASTER
            };
            FT_COPY_UUID(evt.node_uuid, ctx->secondary.uuid);
            snprintf(evt.detail, LOG_BUFFER_SIZE,
                     "Secondaire injoignable, cluster sans hot-standby");

            ft_node_set_state(ctx, &ctx->secondary,
                              NODE_STATE_FAILED, "secondary_heartbeat_timeout");
            ft_log_fault(ctx, &evt);
            /* Déclenchement d'une élection pour désigner un nouveau secondaire */
            ft_master_trigger_election(ctx);
        }
    }
}
