/*
 * em_ipc.c — Implémentation IPC de l'Execution Master
 */

#include "em_ipc.h"

#include <sys/msg.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* ── État interne ─────────────────────────────────────────────────────────── */

static int g_submit_qid = -1;   /* file EM → Orchestrateur */
static int g_result_qid = -1;   /* file Orchestrateur → EM */

/* ── Cycle de vie ─────────────────────────────────────────────────────────── */

int em_ipc_init(void)
{
    g_submit_qid = msgget(EM_IPC_KEY_SUBMIT, IPC_CREAT | 0666);
    if (g_submit_qid == -1) {
        perror("[EM_IPC] msgget(SUBMIT) failed");
        return -1;
    }

    g_result_qid = msgget(EM_IPC_KEY_RESULT, IPC_CREAT | 0666);
    if (g_result_qid == -1) {
        perror("[EM_IPC] msgget(RESULT) failed");
        return -1;
    }

    printf("[EM_IPC] Files ouvertes : submit_qid=%d result_qid=%d\n",
           g_submit_qid, g_result_qid);
    return 0;
}

void em_ipc_destroy(void)
{
    /* L'EM ne supprime pas les files — l'Orchestrateur en est propriétaire. */
    g_submit_qid = -1;
    g_result_qid = -1;
    printf("[EM_IPC] Handles fermés.\n");
}

/* ── Soumission de job ────────────────────────────────────────────────────── */

int em_ipc_submit_job(const em_job_t  *job,
                       const em_task_t *tasks,
                       size_t           n_tasks)
{
    if (!job || !tasks || n_tasks == 0 || g_submit_qid == -1)
        return -1;
    if (n_tasks > EM_MAX_TASKS_PER_JOB)
        return -1;

    em_submit_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.mtype   = EM_MSG_JOB_SUBMIT;
    msg.job_id  = job->job_id;
    msg.n_tasks = n_tasks;
    memcpy(msg.client_id, job->client_id, EM_CLIENT_ID_LEN - 1);
    msg.client_id[EM_CLIENT_ID_LEN - 1] = '\0';
    memcpy(msg.tasks, tasks, n_tasks * sizeof(em_task_t));

    /*
     * On envoie uniquement la partie utile du message pour rester
     * bien en dessous de MSGMAX (8192 octets sur Linux).
     */
    size_t payload_size = sizeof(em_submit_msg_t)
                          - (EM_MAX_TASKS_PER_JOB - n_tasks) * sizeof(em_task_t);

    if (msgsnd(g_submit_qid, &msg, payload_size - sizeof(long), 0) == -1) {
        perror("[EM_IPC] msgsnd(SUBMIT) failed");
        return -1;
    }

    printf("[EM_IPC] Job %lu soumis (%zu tâches).\n",
           (unsigned long)job->job_id, n_tasks);
    return 0;
}

/* ── Attente du résultat ─────────────────────────────────────────────────── */

int em_ipc_wait_result(uint64_t        job_id,
                        em_result_msg_t *result_msg,
                        int             timeout_s)
{
    if (!result_msg || g_result_qid == -1)
        return -1;

    time_t deadline = (timeout_s > 0) ? time(NULL) + timeout_s : 0;

    while (1) {
        /*
         * IPC_NOWAIT si timeout activé : permet de vérifier l'expiration
         * entre chaque tentative. Bloquant si timeout == 0.
         */
        int flags = (timeout_s > 0) ? IPC_NOWAIT : 0;
        ssize_t ret = msgrcv(g_result_qid,
                              result_msg,
                              sizeof(*result_msg) - sizeof(long),
                              EM_MSG_JOB_RESULT,
                              flags);

        if (ret > 0) {
            if (result_msg->job_id == job_id) {
                printf("[EM_IPC] Résultat reçu pour job %lu.\n",
                       (unsigned long)job_id);
                return 0;
            }
            /*
             * Message pour un autre job : remis en queue.
             * Dans un système multi-jobs, un dispatcher dédié serait nécessaire.
             */
            msgsnd(g_result_qid, result_msg,
                   sizeof(*result_msg) - sizeof(long), 0);
        }

        if (timeout_s > 0) {
            if (time(NULL) >= deadline) {
                fprintf(stderr, "[EM_IPC] Timeout pour job %lu.\n",
                        (unsigned long)job_id);
                return -2;
            }
            sleep(1);
        }
    }
}
