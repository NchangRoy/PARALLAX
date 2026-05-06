/*
 * fault_worker_secondary.c
 * Gestion des pannes côté Worker et côté Nœud Secondaire
 */

#include "fault_tolerance.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ═══════════════════════════════════════════════════════════════
 * PARTIE A — WORKER
 * ═══════════════════════════════════════════════════════════════ */

/*
 * worker_open_socket
 * Ouvre et connecte un socket TCP vers le maître.
 * Retourne le fd sur succès, -1 sur erreur.
 * Toutes les erreurs POSIX sont reportées.
 */
static int worker_open_socket(const ClusterNode *master)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[WORKER] socket()");
        return -1;
    }

    /* SO_REUSEADDR pour éviter TIME_WAIT */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Timeout de connexion de 5 s */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(master->port);
    if (inet_pton(AF_INET, master->ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "[WORKER] Adresse IP invalide: %s\n", master->ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[WORKER] connect()");
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * ft_worker_reconnect
 * Tente de se reconnecter au maître après une perte de connexion.
 * Utilise un backoff exponentiel borné pour éviter de surcharger
 * le réseau lors d'une tempête de reconnexions.
 *
 * Séquence :
 *   tentative 1 → attendre RECONNECT_DELAY_MS
 *   tentative 2 → attendre 2 × RECONNECT_DELAY_MS
 *   ...
 *   tentative n → attendre min(2^n × RECONNECT_DELAY_MS, 30 000 ms)
 */
int ft_worker_reconnect(NodeContext *ctx)
{
    if (!ctx) return -1;
    if (ctx->self.role != NODE_ROLE_WORKER) return -1;

    ft_node_set_state(ctx, &ctx->self,
                      NODE_STATE_DISCONNECTED, "reconnect_initiated");

    uint32_t delay_ms = RECONNECT_DELAY_MS;

    for (int attempt = 1; attempt <= MAX_RECONNECT_ATTEMPTS; attempt++) {

        fprintf(stderr,
                "[WORKER] Reconnexion tentative %d/%d dans %u ms...\n",
                attempt, MAX_RECONNECT_ATTEMPTS, delay_ms);

        /* Fermer l'ancienne socket proprement */
        pthread_mutex_lock(&ctx->self.lock);
        if (ctx->self.socket_fd >= 0) {
            close(ctx->self.socket_fd);
            ctx->self.socket_fd = -1;
        }
        pthread_mutex_unlock(&ctx->self.lock);

        /* Attente backoff */
        struct timespec ts = {
            .tv_sec  = delay_ms / 1000,
            .tv_nsec = (delay_ms % 1000) * 1000000L
        };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
            ;

        /* Tentative de connexion */
        int fd = worker_open_socket(&ctx->master);
        if (fd >= 0) {
            pthread_mutex_lock(&ctx->self.lock);
            ctx->self.socket_fd          = fd;
            ctx->self.reconnect_attempts = 0;
            clock_gettime(CLOCK_MONOTONIC, &ctx->self.last_heartbeat);
            pthread_mutex_unlock(&ctx->self.lock);

            ft_node_set_state(ctx, &ctx->self,
                              NODE_STATE_ACTIVE, "reconnected_to_master");

            fprintf(stderr,
                    "[WORKER] Reconnecté au maître %s:%u\n",
                    ctx->master.ip, ctx->master.port);
            return 0;
        }

        /* Backoff exponentiel, plafonné à 30 s */
        delay_ms = (delay_ms * 2 < 30000) ? delay_ms * 2 : 30000;
        ctx->self.reconnect_attempts = attempt;

        FaultEvent evt = {
            .type             = FAULT_SOCKET_ERROR,
            .node_role        = NODE_ROLE_WORKER,
            .timestamp        = time(NULL),
            .suggested_action = RECOVERY_RECONNECT
        };
        FT_COPY_UUID(evt.node_uuid, ctx->self.uuid);
        snprintf(evt.detail, LOG_BUFFER_SIZE,
                 "Tentative %d/%d échouée vers %s:%u",
                 attempt, MAX_RECONNECT_ATTEMPTS,
                 ctx->master.ip, ctx->master.port);
        ft_log_fault(ctx, &evt);
    }

    /* Toutes les tentatives épuisées */
    ft_node_set_state(ctx, &ctx->self,
                      NODE_STATE_FAILED,
                      "max_reconnect_attempts_reached");

    FaultEvent evt = {
        .type             = FAULT_NETWORK_BROKEN,
        .node_role        = NODE_ROLE_WORKER,
        .timestamp        = time(NULL),
        .suggested_action = RECOVERY_REMOVE_NODE
    };
    FT_COPY_UUID(evt.node_uuid, ctx->self.uuid);
    snprintf(evt.detail, LOG_BUFFER_SIZE,
             "Échec après %d tentatives. Nœud retiré du cluster.",
             MAX_RECONNECT_ATTEMPTS);
    ft_log_fault(ctx, &evt);

    return -1;
}

/*
 * ft_worker_handle_overload
 * Réaction du worker lorsqu'il détecte lui-même sa surcharge.
 *
 * Stratégie :
 *   1. Passer en état OVERLOADED → ne plus accepter de nouvelles tâches
 *   2. Notifier le maître via socket
 *   3. Attendre que la charge redescende (sondage toutes les 500 ms)
 *   4. Repasser en ACTIVE quand les seuils sont respectés
 */
int ft_worker_handle_overload(NodeContext *ctx)
{
    if (!ctx) return -1;
    if (ctx->self.role != NODE_ROLE_WORKER) return -1;

    ft_node_set_state(ctx, &ctx->self,
                      NODE_STATE_OVERLOADED, "self_detected_overload");

    FaultEvent evt = {
        .type             = FAULT_CPU_OVERLOAD,
        .node_role        = NODE_ROLE_WORKER,
        .timestamp        = time(NULL),
        .suggested_action = RECOVERY_THROTTLE_LOAD
    };
    FT_COPY_UUID(evt.node_uuid, ctx->self.uuid);
    snprintf(evt.detail, LOG_BUFFER_SIZE,
             "cpu=%.1f%% ram_used=%uMB/%uMB — throttling activé",
             ctx->self.resources.cpu_load * 100.0f,
             ctx->self.resources.ram_total_mb - ctx->self.resources.ram_available_mb,
             ctx->self.resources.ram_total_mb);
    ft_log_fault(ctx, &evt);

    /*
     * Boucle d'attente : sortir dès que la charge repasse sous les seuils.
     * En production, les mesures viendraient de /proc/stat et /proc/meminfo.
     */
    int poll_count = 0;
    while (ctx->running && ft_is_overloaded(&ctx->self)) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000L }; /* 500 ms */
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
            ;

        poll_count++;
        if (poll_count % 10 == 0) {
            fprintf(stderr,
                    "[WORKER] Toujours en surcharge depuis %d s...\n",
                    poll_count / 2);
        }
    }

    if (ctx->running) {
        ft_node_set_state(ctx, &ctx->self,
                          NODE_STATE_ACTIVE, "overload_resolved");
        fprintf(stderr, "[WORKER] Surcharge résolue, retour en ACTIVE.\n");
    }

    return 0;
}

/*
 * ft_worker_retry_task
 * Tente de ré-exécuter une tâche échouée sur le même worker.
 * Incrémente le compteur de tentatives.
 * Retourne 0 si la tâche doit être relancée, -1 si abandonnée.
 */
int ft_worker_retry_task(NodeContext *ctx, Task *task)
{
    if (!ctx || !task) return -1;

    task->retry_count++;

    if (task->retry_count > MAX_TASK_RETRIES) {
        task->state = TASK_STATE_FAILED;

        FaultEvent evt = {
            .type             = FAULT_TASK_CRASH,
            .node_role        = NODE_ROLE_WORKER,
            .timestamp        = time(NULL),
            .suggested_action = RECOVERY_MIGRATE_TASK
        };
        FT_COPY_UUID(evt.node_uuid, ctx->self.uuid);
        snprintf(evt.detail, LOG_BUFFER_SIZE,
                 "Tâche=%s abandonnée après %d tentatives",
                 task->task_id, task->retry_count);
        ft_log_fault(ctx, &evt);

        return -1; /* Le maître doit décider de la suite */
    }

    task->state = TASK_STATE_PENDING;
    clock_gettime(CLOCK_MONOTONIC, &task->assigned_at);

    fprintf(stderr,
            "[WORKER] Retry tâche=%s tentative %d/%d\n",
            task->task_id, task->retry_count, MAX_TASK_RETRIES);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PARTIE B — NŒUD SECONDAIRE
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ft_secondary_sync_state
 * Synchronise l'état du cluster depuis le maître primaire.
 * Cette opération doit être appelée :
 *   - À l'initialisation du secondaire
 *   - Périodiquement (ex. toutes les 500 ms) pour rester à jour
 *   - Après une reconnexion réseau
 *
 * En production : envoi d'un message SYNC_REQUEST au maître,
 * qui renvoie son TableauEtatCluster sérialisé.
 * Ici : copie en mémoire partagée (même processus, test unitaire).
 */
int ft_secondary_sync_state(NodeContext *ctx)
{
    if (!ctx) return -1;
    if (ctx->self.role != NODE_ROLE_SECONDARY) return -1;

    if (!ft_is_node_alive(&ctx->master)) {
        fprintf(stderr,
                "[SECONDARY] Impossible de synchroniser : maître injoignable\n");
        return -1;
    }

    /*
     * Point d'extension réseau :
     *   send_sync_request(ctx->master.socket_fd);
     *   recv_cluster_state(ctx->master.socket_fd, &snapshot);
     *   apply_snapshot(ctx, &snapshot);
     */

    clock_gettime(CLOCK_MONOTONIC, &ctx->self.last_heartbeat);
    fprintf(stderr,
            "[SECONDARY] État synchronisé depuis maître=%s\n",
            ctx->master.uuid);
    return 0;
}

/*
 * ft_secondary_promote_to_master
 * Promeut ce nœud secondaire en maître primaire.
 *
 * Étapes :
 *   1. Changer le rôle local → MASTER
 *   2. Démarrer l'écoute des workers (reprise de la file de tâches)
 *   3. Broadcaster aux workers le changement de maître
 *   4. Déclencher une élection pour désigner un nouveau secondaire
 */
int ft_secondary_promote_to_master(NodeContext *ctx)
{
    if (!ctx) return -1;
    if (ctx->self.role != NODE_ROLE_SECONDARY) return -1;

    fprintf(stderr,
            "[SECONDARY] Promotion en MAÎTRE (uuid=%s)\n", ctx->self.uuid);

    /* Transition de rôle atomique */
    pthread_mutex_lock(&ctx->self.lock);
    ctx->self.role = NODE_ROLE_MASTER;
    pthread_mutex_unlock(&ctx->self.lock);

    ft_node_set_state(ctx, &ctx->self,
                      NODE_STATE_ACTIVE, "promoted_to_master");

    /*
     * Notifier tous les workers du changement de maître.
     * En production : broadcast UDP ou message gRPC MASTER_CHANGED
     * contenant la nouvelle IP/port du maître.
     */
    pthread_mutex_lock(&ctx->cluster_lock);
    int count = ctx->worker_count;
    pthread_mutex_unlock(&ctx->cluster_lock);

    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&ctx->workers[i].lock);
        NodeState ws = ctx->workers[i].state;
        pthread_mutex_unlock(&ctx->workers[i].lock);

        if (ws == NODE_STATE_ACTIVE || ws == NODE_STATE_OVERLOADED) {
            fprintf(stderr,
                    "[SECONDARY→MASTER] Notification worker=%s : nouveau maître\n",
                    ctx->workers[i].uuid);
            /*
             * En production :
             *   send_master_changed(ctx->workers[i].socket_fd,
             *                       ctx->self.ip, ctx->self.port);
             */
        }
    }

    /* Mettre à jour le champ master dans le contexte local */
    pthread_mutex_lock(&ctx->cluster_lock);
    memcpy(&ctx->master, &ctx->self, sizeof(ClusterNode));
    pthread_mutex_unlock(&ctx->cluster_lock);

    /* Déclencher élection d'un nouveau secondaire */
    ft_master_trigger_election(ctx);

    if (ctx->on_recovery) {
        FaultEvent evt = {
            .type             = FAULT_MASTER_DOWN,
            .node_role        = NODE_ROLE_SECONDARY,
            .timestamp        = time(NULL),
            .suggested_action = RECOVERY_PROMOTE_SECONDARY
        };
        FT_COPY_UUID(evt.node_uuid, ctx->self.uuid);
        snprintf(evt.detail, LOG_BUFFER_SIZE,
                 "Secondaire promu maître, %d workers notifiés", count);
        ctx->on_recovery(&evt, RECOVERY_PROMOTE_SECONDARY);
    }

    return 0;
}

/*
 * ft_secondary_handle_master_down
 * Orchestre la réponse complète à la panne du maître primaire.
 *
 * Séquence :
 *   1. Attendre ELECTION_TIMEOUT_MS (s'assurer que le maître est vraiment mort)
 *   2. Vérifier une dernière fois
 *   3. Si confirmé mort → se promouvoir
 */
int ft_secondary_handle_master_down(NodeContext *ctx)
{
    if (!ctx) return -1;
    if (ctx->self.role != NODE_ROLE_SECONDARY) return -1;

    fprintf(stderr,
            "[SECONDARY] Maître absent. Attente de confirmation %d ms...\n",
            ELECTION_TIMEOUT_MS);

    struct timespec ts = {
        .tv_sec  = ELECTION_TIMEOUT_MS / 1000,
        .tv_nsec = (ELECTION_TIMEOUT_MS % 1000) * 1000000L
    };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;

    /* Double-vérification après le délai */
    if (ft_is_node_alive(&ctx->master)) {
        fprintf(stderr,
                "[SECONDARY] Maître répond à nouveau. "
                "Annulation de la promotion.\n");
        ft_node_set_state(ctx, &ctx->master,
                          NODE_STATE_ACTIVE, "master_recovered");
        return 0;
    }

    /* Maître confirmé mort : prendre le relais */
    return ft_secondary_promote_to_master(ctx);
}
