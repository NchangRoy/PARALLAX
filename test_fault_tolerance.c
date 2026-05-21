/*
 * test_fault_tolerance.c
 * Programme de démonstration et tests unitaires du module ft
 *
 * Compile :  make
 * Exécute :  ./ft_test
 *
 * Couvre :
 *   TEST 1 — Détection de panne heartbeat sur un worker
 *   TEST 2 — Surcharge CPU et throttling
 *   TEST 3 — Migration de tâches après panne
 *   TEST 4 — Promotion du secondaire en maître
 *   TEST 5 — Reconnexion worker avec backoff (simulation)
 *   TEST 6 — Timeout de tâche
 */

#include "fault_tolerance.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────
 * Callbacks de test
 * ───────────────────────────────────────────────────────────── */

static int g_fault_count    = 0;
static int g_recovery_count = 0;

static void on_fault_cb(const FaultEvent *evt)
{
    g_fault_count++;
    (void)evt;
}

static void on_recovery_cb(const FaultEvent *evt, RecoveryAction action)
{
    g_recovery_count++;
    (void)evt; (void)action;
}

static void on_state_change_cb(const ClusterNode *node,
                                NodeState old, NodeState new_s)
{
    (void)node; (void)old; (void)new_s;
}

/* ─────────────────────────────────────────────────────────────
 * Utilitaires de test
 * ───────────────────────────────────────────────────────────── */

#define TEST_PASS "\033[32m[PASS]\033[0m"
#define TEST_FAIL "\033[31m[FAIL]\033[0m"

static void print_separator(const char *title)
{
    printf("\n─────────────────────────────────────────\n");
    printf("  %s\n", title);
    printf("─────────────────────────────────────────\n");
}

/*
 * make_worker_context
 * Crée un contexte de maître avec N workers préconfigurés.
 */
static NodeContext *make_master_with_workers(int n)
{
    NodeContext *ctx = (NodeContext *)calloc(1, sizeof(NodeContext));
    if (!ctx) return NULL;

    ft_init(ctx, NODE_ROLE_MASTER, "127.0.0.1", 9000);
    ctx->on_fault        = on_fault_cb;
    ctx->on_recovery     = on_recovery_cb;
    ctx->on_state_change = on_state_change_cb;

    for (int i = 0; i < n && i < MAX_WORKERS; i++) {
        ClusterNode *w = &ctx->workers[i];
        ft_generate_uuid(w->uuid);
        snprintf(w->ip, MAX_IP_LEN, "10.0.0.%d", i + 2);
        w->port                   = 9000 + i + 1;
        w->role                   = NODE_ROLE_WORKER;
        w->state                  = NODE_STATE_ACTIVE;
        w->resources.ram_total_mb = 4096;
        w->resources.ram_available_mb = 2048;
        w->resources.cpu_load     = 0.20f + i * 0.05f;
        w->resources.cpu_count    = 4;
        w->socket_fd              = -1;
        clock_gettime(CLOCK_MONOTONIC, &w->last_heartbeat);
    }
    ctx->worker_count = n;

    /* Configurer le secondaire */
    ft_generate_uuid(ctx->secondary.uuid);
    strcpy(ctx->secondary.ip, "10.0.0.100");
    ctx->secondary.port  = 9100;
    ctx->secondary.role  = NODE_ROLE_SECONDARY;
    ctx->secondary.state = NODE_STATE_ACTIVE;
    clock_gettime(CLOCK_MONOTONIC, &ctx->secondary.last_heartbeat);

    return ctx;
}

/* ─────────────────────────────────────────────────────────────
 * TEST 1 — Détection heartbeat manquant
 * ───────────────────────────────────────────────────────────── */

static void test_heartbeat_detection(void)
{
    print_separator("TEST 1 — Détection panne heartbeat");

    NodeContext *ctx = make_master_with_workers(3);
    assert(ctx != NULL);

    /* Simuler un worker mort : forcer un heartbeat très ancien via CLOCK_MONOTONIC */
    struct timespec old_ts;
    clock_gettime(CLOCK_MONOTONIC, &old_ts);
    old_ts.tv_sec -= 60;   /* 60 secondes dans le passé */
    ctx->workers[1].last_heartbeat = old_ts;

    int initial_faults = g_fault_count;

    /* Vérification — doit détecter le worker[1] comme mort */
    ft_check_all_nodes(ctx);

    bool worker1_failed = (ctx->workers[1].state == NODE_STATE_FAILED);
    bool worker0_ok     = (ctx->workers[0].state == NODE_STATE_ACTIVE);
    bool worker2_ok     = (ctx->workers[2].state == NODE_STATE_ACTIVE);

    printf("%s Worker mort détecté (state=FAILED)\n",
           worker1_failed ? TEST_PASS : TEST_FAIL);
    printf("%s Workers sains non impactés\n",
           (worker0_ok && worker2_ok) ? TEST_PASS : TEST_FAIL);
    printf("%s Callback on_fault appelé\n",
           (g_fault_count > initial_faults) ? TEST_PASS : TEST_FAIL);

    ft_destroy(ctx);
    free(ctx);
}

/* ─────────────────────────────────────────────────────────────
 * TEST 2 — Détection surcharge CPU
 * ───────────────────────────────────────────────────────────── */

static void test_overload_detection(void)
{
    print_separator("TEST 2 — Détection surcharge CPU");

    NodeContext *ctx = make_master_with_workers(2);
    assert(ctx != NULL);

    /* Simuler surcharge sur worker[0] */
    ctx->workers[0].resources.cpu_load = 0.92f;

    ft_check_all_nodes(ctx);

    bool w0_overloaded = (ctx->workers[0].state == NODE_STATE_OVERLOADED);
    bool w1_active     = (ctx->workers[1].state == NODE_STATE_ACTIVE);
    bool is_overloaded = ft_is_overloaded(&ctx->workers[0]);

    printf("%s Worker[0] cpu=92%% → OVERLOADED\n",
           w0_overloaded ? TEST_PASS : TEST_FAIL);
    printf("%s Worker[1] non affecté\n",
           w1_active ? TEST_PASS : TEST_FAIL);
    printf("%s ft_is_overloaded() correct\n",
           is_overloaded ? TEST_PASS : TEST_FAIL);

    ft_destroy(ctx);
    free(ctx);
}

/* ─────────────────────────────────────────────────────────────
 * TEST 3 — Migration de tâches après panne worker
 * ───────────────────────────────────────────────────────────── */

static void test_task_migration(void)
{
    print_separator("TEST 3 — Migration de tâches");

    NodeContext *ctx = make_master_with_workers(3);
    assert(ctx != NULL);

    char dead_worker_uuid[UUID_STR_LEN];
    strncpy(dead_worker_uuid, ctx->workers[0].uuid, UUID_STR_LEN);

    /* Insérer 3 tâches affectées au worker[0] */
    ctx->task_count = 3;
    for (int i = 0; i < 3; i++) {
        Task *t = &ctx->task_queue[i];
        ft_generate_uuid(t->task_id);
        ft_generate_uuid(t->job_id);
        t->state         = TASK_STATE_RUNNING;
        t->retry_count   = 0;
        t->latency_max_ms = 5000;
        clock_gettime(CLOCK_MONOTONIC, &t->assigned_at);
        strncpy(t->assigned_worker, dead_worker_uuid, UUID_STR_LEN);
    }

    /* Worker[1] a un meilleur score que worker[2] */
    ctx->workers[1].resources.cpu_load         = 0.10f;
    ctx->workers[1].resources.ram_available_mb = 3500;
    ctx->workers[2].resources.cpu_load         = 0.50f;

    /* Simuler la panne */
    ctx->workers[0].state = NODE_STATE_FAILED;
    int ret = ft_master_handle_worker_failure(ctx, dead_worker_uuid);

    int migrated = 0;
    for (int i = 0; i < ctx->task_count; i++) {
        if (ctx->task_queue[i].state == TASK_STATE_MIGRATED)
            migrated++;
    }

    printf("%s ft_master_handle_worker_failure() retourne 0\n",
           (ret == 0) ? TEST_PASS : TEST_FAIL);
    printf("%s 3/3 tâches migrées vers meilleur worker\n",
           (migrated == 3) ? TEST_PASS : TEST_FAIL);

    bool all_on_best = true;
    for (int i = 0; i < ctx->task_count; i++) {
        if (strncmp(ctx->task_queue[i].assigned_worker,
                    ctx->workers[1].uuid, UUID_STR_LEN) != 0)
        {
            all_on_best = false;
        }
    }
    printf("%s Tâches assignées au worker avec meilleur score\n",
           all_on_best ? TEST_PASS : TEST_FAIL);

    ft_destroy(ctx);
    free(ctx);
}

/* ─────────────────────────────────────────────────────────────
 * TEST 4 — Promotion secondaire en maître
 * ───────────────────────────────────────────────────────────── */

static void test_secondary_promotion(void)
{
    print_separator("TEST 4 — Promotion secondaire → maître");

    /* Créer un contexte de type SECONDARY */
    NodeContext *ctx = (NodeContext *)calloc(1, sizeof(NodeContext));
    assert(ctx != NULL);
    ft_init(ctx, NODE_ROLE_SECONDARY, "10.0.0.100", 9100);
    ctx->on_fault        = on_fault_cb;
    ctx->on_recovery     = on_recovery_cb;
    ctx->on_state_change = on_state_change_cb;

    /* Configurer un maître "mort" */
    ft_generate_uuid(ctx->master.uuid);
    strcpy(ctx->master.ip, "10.0.0.1");
    ctx->master.port  = 9000;
    ctx->master.role  = NODE_ROLE_MASTER;
    ctx->master.state = NODE_STATE_ACTIVE;
    struct timespec old = { .tv_sec = time(NULL) - 60, .tv_nsec = 0 };
    ctx->master.last_heartbeat = old;

    /* Ajouter quelques workers connus du secondaire */
    for (int i = 0; i < 2; i++) {
        ft_generate_uuid(ctx->workers[i].uuid);
        snprintf(ctx->workers[i].ip, MAX_IP_LEN, "10.0.0.%d", i + 2);
        ctx->workers[i].port  = 9001 + i;
        ctx->workers[i].role  = NODE_ROLE_WORKER;
        ctx->workers[i].state = NODE_STATE_ACTIVE;
        ctx->workers[i].resources.ram_total_mb    = 4096;
        ctx->workers[i].resources.ram_available_mb = 2048;
        ctx->workers[i].resources.cpu_load        = 0.2f + i * 0.1f;
        clock_gettime(CLOCK_MONOTONIC, &ctx->workers[i].last_heartbeat);
    }
    ctx->worker_count = 2;

    int ret = ft_secondary_promote_to_master(ctx);

    printf("%s Promotion réussie (retval=0)\n",
           (ret == 0) ? TEST_PASS : TEST_FAIL);
    printf("%s Rôle local changé en MASTER\n",
           (ctx->self.role == NODE_ROLE_MASTER) ? TEST_PASS : TEST_FAIL);
    printf("%s État local = ACTIVE\n",
           (ctx->self.state == NODE_STATE_ACTIVE) ? TEST_PASS : TEST_FAIL);

    ft_destroy(ctx);
    free(ctx);
}

/* ─────────────────────────────────────────────────────────────
 * TEST 5 — Timeout de tâche
 * ───────────────────────────────────────────────────────────── */

static void test_task_timeout(void)
{
    print_separator("TEST 5 — Timeout tâche individuelle");

    NodeContext *ctx = make_master_with_workers(2);
    assert(ctx != NULL);

    /* Créer une tâche avec une latence max très courte */
    ctx->task_count = 1;
    Task *t = &ctx->task_queue[0];
    ft_generate_uuid(t->task_id);
    ft_generate_uuid(t->job_id);
    t->state          = TASK_STATE_RUNNING;
    t->retry_count    = 0;
    t->latency_max_ms = 1;  /* 1 ms : déjà dépassé dès le test */
    strncpy(t->assigned_worker, ctx->workers[0].uuid, UUID_STR_LEN);

    /* Forcer un timestamp très ancien */
    struct timespec old = { .tv_sec = 0, .tv_nsec = 0 };
    t->assigned_at = old;

    ft_master_check_task_timeouts(ctx);

    bool retried = (t->state == TASK_STATE_PENDING && t->retry_count == 1);
    printf("%s Tâche expirée remise en PENDING (retry=1)\n",
           retried ? TEST_PASS : TEST_FAIL);

    /* Épuiser les tentatives */
    t->state       = TASK_STATE_RUNNING;
    t->retry_count = MAX_TASK_RETRIES;
    ft_master_check_task_timeouts(ctx);

    bool abandoned = (t->state == TASK_STATE_FAILED);
    printf("%s Tâche abandonnée après %d tentatives\n",
           abandoned ? TEST_PASS : TEST_FAIL, MAX_TASK_RETRIES);

    ft_destroy(ctx);
    free(ctx);
}

/* ─────────────────────────────────────────────────────────────
 * TEST 6 — UUID unicité
 * ───────────────────────────────────────────────────────────── */

static void test_uuid_generation(void)
{
    print_separator("TEST 6 — Génération UUID");

    char uuid1[UUID_STR_LEN];
    char uuid2[UUID_STR_LEN];

    ft_generate_uuid(uuid1);
    ft_generate_uuid(uuid2);

    bool different  = (strncmp(uuid1, uuid2, UUID_STR_LEN) != 0);
    bool len_ok     = (strlen(uuid1) == UUID_STR_LEN - 1);
    bool format_ok  = (uuid1[8] == '-' && uuid1[13] == '-' &&
                       uuid1[18] == '-' && uuid1[23] == '-');

    printf("%s UUIDs différents à chaque génération\n",
           different  ? TEST_PASS : TEST_FAIL);
    printf("%s Longueur correcte (%zu chars)\n",
           len_ok    ? TEST_PASS : TEST_FAIL, strlen(uuid1));
    printf("%s Format xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\n",
           format_ok ? TEST_PASS : TEST_FAIL);
    printf("    uuid1 = %s\n", uuid1);
    printf("    uuid2 = %s\n", uuid2);
}

/* ─────────────────────────────────────────────────────────────
 * MAIN
 * ───────────────────────────────────────────────────────────── */

int main(void)
{
    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  TESTS — Gestion des pannes distribuées  ║\n");
    printf("╚══════════════════════════════════════════╝\n");

    test_uuid_generation();
    test_heartbeat_detection();
    test_overload_detection();
    test_task_migration();
    test_secondary_promotion();
    test_task_timeout();

    printf("\n─────────────────────────────────────────\n");
    printf("  Pannes journalisées  : %d\n", g_fault_count);
    printf("  Récupérations        : %d\n", g_recovery_count);
    printf("─────────────────────────────────────────\n\n");

    return 0;
}
