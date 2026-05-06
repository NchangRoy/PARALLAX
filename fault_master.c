/*
 * fault_master.c
 * Gestion des pannes côté Nœud Maître
 *
 * Responsabilités :
 *   1. Détecter la panne d'un worker et migrer ses tâches
 *   2. Choisir le meilleur worker cible pour la migration
 *   3. Déclencher le protocole d'élection si nécessaire
 *   4. Gérer les timeouts de tâches individuelles
 */

#include "fault_tolerance.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ─────────────────────────────────────────────────────────────
 * Fonctions internes du maître
 * ───────────────────────────────────────────────────────────── */

/*
 * master_score_worker
 * Score de disponibilité d'un worker (plus grand = meilleur).
 * score = (1 - cpu_load) × (ram_available / ram_total)
 * Un worker surcharté ou en panne a un score de 0.
 */
static float master_score_worker(const ClusterNode *w)
{
    if (!w) return 0.0f;
    if (w->state != NODE_STATE_ACTIVE) return 0.0f;
    if (w->resources.ram_total_mb == 0) return 0.0f;

    float cpu_avail = 1.0f - w->resources.cpu_load;
    float mem_ratio = (float)w->resources.ram_available_mb
                    / (float)w->resources.ram_total_mb;

    return cpu_avail * mem_ratio;
}

/*
 * master_best_available_worker
 * Retourne l'index du worker avec le meilleur score, en
 * excluant le worker passé en paramètre (qui vient de tomber).
 * Retourne -1 si aucun worker disponible.
 */
static int master_best_available_worker(NodeContext *ctx,
                                        const char *exclude_uuid)
{
    int   best_idx   = -1;
    float best_score = -1.0f;

    pthread_mutex_lock(&ctx->cluster_lock);
    int count = ctx->worker_count;
    pthread_mutex_unlock(&ctx->cluster_lock);

    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&ctx->workers[i].lock);
        bool skip = (strncmp(ctx->workers[i].uuid,
                             exclude_uuid, UUID_STR_LEN) == 0);
        float score = master_score_worker(&ctx->workers[i]);
        pthread_mutex_unlock(&ctx->workers[i].lock);

        if (!skip && score > best_score) {
            best_score = score;
            best_idx   = i;
        }
    }
    return best_idx;
}

/*
 * master_collect_failed_tasks
 * Collecte les tâches RUNNING ou ASSIGNED appartenant au worker
 * défaillant. Ces tâches doivent être remises en attente ou migrées.
 * Retourne le nombre de tâches récupérées.
 */
static int master_collect_failed_tasks(NodeContext *ctx,
                                       const char *failed_uuid,
                                       Task **out_tasks,
                                       int max_out)
{
    int found = 0;

    pthread_mutex_lock(&ctx->cluster_lock);
    for (int i = 0; i < ctx->task_count && found < max_out; i++) {
        Task *t = &ctx->task_queue[i];
        if ((t->state == TASK_STATE_RUNNING ||
             t->state == TASK_STATE_ASSIGNED) &&
            strncmp(t->assigned_worker, failed_uuid, UUID_STR_LEN) == 0)
        {
            out_tasks[found++] = t;
        }
    }
    pthread_mutex_unlock(&ctx->cluster_lock);
    return found;
}

/* ─────────────────────────────────────────────────────────────
 * API publique — Maître
 * ───────────────────────────────────────────────────────────── */

/*
 * ft_master_handle_worker_failure
 * Point d'entrée principal quand un worker est déclaré en panne.
 *
 * Algorithme :
 *   1. Récupérer toutes les tâches du worker défaillant
 *   2. Trouver le meilleur worker cible
 *   3. Migrer les tâches vers ce worker
 *   4. Si aucun worker disponible → mettre les tâches en PENDING
 *      pour réassignation future
 *   5. Retirer le worker du cluster actif
 */
int ft_master_handle_worker_failure(NodeContext *ctx, const char *worker_uuid)
{
    if (!ctx || !worker_uuid) return -1;
    if (ctx->self.role != NODE_ROLE_MASTER) return -1;

    fprintf(stderr,
            "[MASTER] Prise en charge panne worker=%s\n", worker_uuid);

    /* Étape 1 : collecter les tâches impactées */
    Task *failed_tasks[MAX_TASKS];
    int n = master_collect_failed_tasks(ctx, worker_uuid,
                                        failed_tasks, MAX_TASKS);

    fprintf(stderr,
            "[MASTER] %d tâches récupérées depuis worker=%s\n", n, worker_uuid);

    if (n == 0) {
        fprintf(stderr, "[MASTER] Aucune tâche active, panne sans impact.\n");
        return 0;
    }

    /* Étape 2 : trouver le meilleur worker cible */
    int target_idx = master_best_available_worker(ctx, worker_uuid);

    if (target_idx < 0) {
        /*
         * Aucun worker disponible : remettre les tâches en PENDING
         * pour une réassignation dès qu'un worker se reconnecte.
         */
        fprintf(stderr,
                "[MASTER] Aucun worker disponible. "
                "Tâches remises en PENDING.\n");

        pthread_mutex_lock(&ctx->cluster_lock);
        for (int i = 0; i < n; i++) {
            failed_tasks[i]->state           = TASK_STATE_PENDING;
            failed_tasks[i]->assigned_worker[0] = '\0';
        }
        pthread_mutex_unlock(&ctx->cluster_lock);

        FaultEvent evt = {
            .type             = FAULT_HEARTBEAT_LOSS,
            .node_role        = NODE_ROLE_MASTER,
            .timestamp        = time(NULL),
            .suggested_action = RECOVERY_RETRY_TASK
        };
        FT_COPY_UUID(evt.node_uuid, ctx->self.uuid);
        snprintf(evt.detail, LOG_BUFFER_SIZE,
                 "Cluster saturé : %d tâches en attente de réassignation", n);
        ft_log_fault(ctx, &evt);
        return -1;
    }

    /* Étape 3 : migrer les tâches vers le worker cible */
    return ft_master_migrate_tasks(ctx, worker_uuid,
                                   ctx->workers[target_idx].uuid);
}

/*
 * ft_master_migrate_tasks
 * Déplace toutes les tâches de 'failed_worker_uuid' vers
 * 'target_worker_uuid'.
 *
 * Note : dans une implémentation réseau complète, cette fonction
 * enverrait le payload de chaque tâche via socket au nouveau worker.
 * Ici, le transfert réseau est représenté par ft_send_task_to_worker().
 */
int ft_master_migrate_tasks(NodeContext *ctx,
                             const char *failed_worker_uuid,
                             const char *target_worker_uuid)
{
    if (!ctx || !failed_worker_uuid || !target_worker_uuid) return -1;

    int migrated = 0;
    int failed   = 0;

    pthread_mutex_lock(&ctx->cluster_lock);
    for (int i = 0; i < ctx->task_count; i++) {
        Task *t = &ctx->task_queue[i];

        if ((t->state == TASK_STATE_RUNNING ||
             t->state == TASK_STATE_ASSIGNED) &&
            strncmp(t->assigned_worker, failed_worker_uuid,
                    UUID_STR_LEN) == 0)
        {
            t->retry_count++;

            if (t->retry_count > MAX_TASK_RETRIES) {
                /* Tâche abandonnée après trop d'échecs */
                t->state = TASK_STATE_FAILED;
                fprintf(stderr,
                        "[MASTER] Tâche=%s abandonnée (retry=%d/%d)\n",
                        t->task_id, t->retry_count, MAX_TASK_RETRIES);
                failed++;
            } else {
                /* Migration */
                t->state = TASK_STATE_MIGRATED;
                memcpy(t->assigned_worker, target_worker_uuid, UUID_STR_LEN - 1);
                t->assigned_worker[UUID_STR_LEN - 1] = '\0';
                clock_gettime(CLOCK_MONOTONIC, &t->assigned_at);
                fprintf(stderr,
                        "[MASTER] Tâche=%s migrée → worker=%s (tentative %d/%d)\n",
                        t->task_id, target_worker_uuid,
                        t->retry_count, MAX_TASK_RETRIES);
                migrated++;
            }
        }
    }
    pthread_mutex_unlock(&ctx->cluster_lock);

    fprintf(stderr,
            "[MASTER] Migration terminée : %d migrées, %d abandonnées\n",
            migrated, failed);

    if (ctx->on_recovery) {
        FaultEvent evt = {
            .type             = FAULT_HEARTBEAT_LOSS,
            .node_role        = NODE_ROLE_MASTER,
            .timestamp        = time(NULL),
            .suggested_action = RECOVERY_MIGRATE_TASK
        };
        FT_COPY_UUID(evt.node_uuid, failed_worker_uuid);
        snprintf(evt.detail, LOG_BUFFER_SIZE,
                 "%d tâches migrées vers %s", migrated, target_worker_uuid);
        ctx->on_recovery(&evt, RECOVERY_MIGRATE_TASK);
    }

    return (failed == 0) ? 0 : -1;
}

/*
 * ft_master_check_task_timeouts
 * Vérifie que chaque tâche RUNNING n'a pas dépassé sa latence max.
 * Si c'est le cas, la tâche est marquée FAILED et remise en PENDING.
 * À appeler périodiquement (ex. toutes les secondes).
 */
void ft_master_check_task_timeouts(NodeContext *ctx)
{
    if (!ctx) return;

    pthread_mutex_lock(&ctx->cluster_lock);
    for (int i = 0; i < ctx->task_count; i++) {
        Task *t = &ctx->task_queue[i];

        if (t->state != TASK_STATE_RUNNING) continue;
        if (t->latency_max_ms == 0) continue;

        int64_t elapsed = ft_elapsed_ms(&t->assigned_at);
        if (elapsed > (int64_t)t->latency_max_ms) {

            fprintf(stderr,
                    "[MASTER] TIMEOUT tâche=%s elapsed=%ldms max=%lums\n",
                    t->task_id, (long)elapsed,
                    (unsigned long)t->latency_max_ms);

            t->retry_count++;
            if (t->retry_count > MAX_TASK_RETRIES) {
                t->state = TASK_STATE_FAILED;
            } else {
                t->state               = TASK_STATE_PENDING;
                t->assigned_worker[0]  = '\0';
            }

            FaultEvent evt = {
                .type             = FAULT_TASK_TIMEOUT,
                .node_role        = NODE_ROLE_MASTER,
                .timestamp        = time(NULL),
                .suggested_action = (t->state == TASK_STATE_FAILED)
                                    ? RECOVERY_REMOVE_NODE
                                    : RECOVERY_RETRY_TASK
            };
            FT_COPY_UUID(evt.node_uuid, t->assigned_worker);
            snprintf(evt.detail, LOG_BUFFER_SIZE,
                     "tâche=%s elapsed=%ldms", t->task_id, (long)elapsed);
            pthread_mutex_unlock(&ctx->cluster_lock);
            ft_log_fault(ctx, &evt);
            pthread_mutex_lock(&ctx->cluster_lock);
        }
    }
    pthread_mutex_unlock(&ctx->cluster_lock);
}

/*
 * ft_master_trigger_election
 * Déclenche le protocole d'élection d'un nouveau nœud secondaire
 * (ou d'un nouveau maître si appelé depuis un secondaire).
 *
 * Algorithme simplifié (Bully) :
 *   - Le nœud avec le score le plus élevé parmi les workers actifs
 *     est élu secondaire.
 *   - Il en est notifié via un message réseau (simulé ici par log).
 */
int ft_master_trigger_election(NodeContext *ctx)
{
    if (!ctx) return -1;

    fprintf(stderr, "[MASTER] Déclenchement protocole d'élection...\n");

    int best_idx   = master_best_available_worker(ctx, "");
    if (best_idx < 0) {
        fprintf(stderr,
                "[MASTER] Élection impossible : aucun worker disponible.\n");
        return -1;
    }

    ClusterNode *elected = &ctx->workers[best_idx];

    fprintf(stderr,
            "[MASTER] Élu nouveau secondaire : uuid=%s ip=%s\n",
            elected->uuid, elected->ip);

    /*
     * Dans l'implémentation réseau réelle, on enverrait ici un message
     * ELECT_SECONDARY au worker élu, qui passerait son rôle à SECONDARY
     * et démarrerait la synchronisation d'état.
     */
    pthread_mutex_lock(&ctx->cluster_lock);
    memcpy(&ctx->secondary, elected, sizeof(ClusterNode));
    pthread_mutex_unlock(&ctx->cluster_lock);

    ft_node_set_state(ctx, &ctx->secondary,
                      NODE_STATE_ACTIVE, "elected_as_secondary");

    if (ctx->on_recovery) {
        FaultEvent evt = {
            .type             = FAULT_SECONDARY_DOWN,
            .node_role        = NODE_ROLE_MASTER,
            .timestamp        = time(NULL),
            .suggested_action = RECOVERY_ELECT_NEW_MASTER
        };
        FT_COPY_UUID(evt.node_uuid, elected->uuid);
        snprintf(evt.detail, LOG_BUFFER_SIZE,
                 "Nouveau secondaire élu : %s", elected->uuid);
        ctx->on_recovery(&evt, RECOVERY_ELECT_NEW_MASTER);
    }

    return 0;
}
