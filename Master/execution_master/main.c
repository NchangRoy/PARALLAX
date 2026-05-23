/*
 * main.c — Point d'entrée de l'Execution Master
 *
 * En production, ce main() est remplacé par l'intégration dans le
 * thread "parser" lancé par Agent_Init (branche Agent_Init).
 *
 * En mode STANDALONE (-DSTANDALONE), un simulateur d'orchestrateur
 * tourne dans un thread séparé pour valider la logique de bout en
 * bout sans déploiement du cluster complet.
 *
 * Compilation :
 *   make        → binaire normal (nécessite un vrai orchestrateur)
 *   make sim    → binaire standalone avec simulateur intégré
 *
 * Usage :
 *   ./execution_master [A] [B]
 *   ./execution_master_sim 987654321 56789
 */

#include "execution_master.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Simulateur d'Orchestrateur (mode STANDALONE uniquement) ─────────────── */

#ifdef STANDALONE
#include "em_ipc.h"
#include <sys/msg.h>
#include <pthread.h>
#include <unistd.h>

static void *simulate_orchestrator(void *arg)
{
    (void)arg;

    int submit_qid = msgget(EM_IPC_KEY_SUBMIT, 0666);
    int result_qid = msgget(EM_IPC_KEY_RESULT, 0666);

    if (submit_qid == -1 || result_qid == -1) {
        perror("[SIM_ORCH] Impossible d'accéder aux files IPC");
        return NULL;
    }

    em_submit_msg_t smsg;
    ssize_t ret = msgrcv(submit_qid, &smsg,
                          sizeof(smsg) - sizeof(long),
                          EM_MSG_JOB_SUBMIT, 0);
    if (ret == -1) {
        perror("[SIM_ORCH] msgrcv(SUBMIT) failed");
        return NULL;
    }

    printf("\n[SIM_ORCH] Job %lu reçu (%zu tâches). Calcul en cours...\n\n",
           (unsigned long)smsg.job_id, smsg.n_tasks);

    em_result_msg_t rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.mtype     = EM_MSG_JOB_RESULT;
    rmsg.job_id    = smsg.job_id;
    rmsg.n_results = smsg.n_tasks;
    rmsg.success   = true;

    for (size_t i = 0; i < smsg.n_tasks; i++) {
        em_task_t *t = &smsg.tasks[i];
        rmsg.results[i].task_id        = t->task_id;
        rmsg.results[i].job_id         = t->job_id;
        rmsg.results[i].success        = true;
        rmsg.results[i].partial_result = t->chunk_value * t->multiplier;
        rmsg.results[i].position       = t->position;
        strncpy(rmsg.results[i].error, "ok", EM_ERROR_LEN - 1);

        printf("[SIM_ORCH]   tâche[%u] : %ld × %ld = %ld  (position 10^%d)\n",
               t->task_id, t->chunk_value, t->multiplier,
               rmsg.results[i].partial_result, t->position);
    }

    printf("\n");
    msgsnd(result_qid, &rmsg, sizeof(rmsg) - sizeof(long), 0);
    printf("[SIM_ORCH] Résultats envoyés à l'Execution Master.\n\n");
    return NULL;
}
#endif /* STANDALONE */

/* ── Point d'entrée ──────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    long A = 987654321L;
    long B = 56789L;

    if (argc >= 3) {
        A = atol(argv[1]);
        B = atol(argv[2]);
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║           PARALLAX — Execution Master        ║\n");
    printf("║          Calcul distribué de A × B           ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");
    printf("  A = %ld\n  B = %ld\n  Attendu = %ld\n\n", A, B, A * B);

    em_config_t cfg   = em_default_config();
    cfg.chunk_digits  = 3;
    cfg.job_timeout_s = 10;

    if (em_init(&cfg) != 0) {
        fprintf(stderr, "Échec de l'initialisation de l'Execution Master.\n");
        return EXIT_FAILURE;
    }

#ifdef STANDALONE
    pthread_t sim_thread;
    pthread_create(&sim_thread, NULL, simulate_orchestrator, NULL);
#endif

    em_job_result_t result;
    int rc = em_run_job(A, B, "client-demo", &result);

#ifdef STANDALONE
    pthread_join(sim_thread, NULL);
#endif

    em_print_result(&result, A * B);
    em_shutdown();

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
