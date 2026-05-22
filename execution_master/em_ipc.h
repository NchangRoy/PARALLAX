/*
 * em_ipc.h — Interface IPC entre l'Execution Master et l'Orchestrateur
 *
 * Ce module abstrait le canal de communication entre l'Execution Master
 * et l'Orchestrateur. Il est le SEUL point de contact entre les deux
 * composants : l'EM ne connaît ni l'implémentation interne de
 * l'orchestrateur, ni les workers.
 *
 * Protocole (System V Message Queues) :
 *
 *   ┌─────────────────┐   EM_QUEUE_JOB_SUBMIT   ┌──────────────────┐
 *   │ Execution Master│ ──────────────────────→  │   Orchestrateur  │
 *   │                 │ ←──────────────────────  │                  │
 *   └─────────────────┘   EM_QUEUE_JOB_RESULT    └──────────────────┘
 *
 * Clés IPC :
 *   EM_IPC_KEY_SUBMIT  (0x504C5801) : EM → Orchestrateur (job + tâches)
 *   EM_IPC_KEY_RESULT  (0x504C5802) : Orchestrateur → EM (résultats)
 *
 * Point d'intégration :
 *   Quand le Network Thread (branche network-thread) sera intégré, ce
 *   module sera remplacé par des appels au Network Thread. L'interface
 *   em_ipc_* restera identique pour l'Execution Master.
 */

#ifndef EM_IPC_H
#define EM_IPC_H

#include "em_types.h"

/* ── Clés des files de messages System V ─────────────────────────────────── */

#define EM_IPC_KEY_SUBMIT  0x504C5801   /* EM → Orchestrateur */
#define EM_IPC_KEY_RESULT  0x504C5802   /* Orchestrateur → EM */

/* ── Types de messages ────────────────────────────────────────────────────── */

typedef enum {
    EM_MSG_JOB_SUBMIT = 1,   /* Execution Master soumet un job            */
    EM_MSG_JOB_RESULT = 2    /* Orchestrateur retourne les résultats       */
} em_msg_type_t;

/* ── Structure du message de soumission (EM → Orchestrateur) ─────────────── */

typedef struct {
    long          mtype;                           /* toujours EM_MSG_JOB_SUBMIT */
    uint64_t      job_id;
    char          client_id[EM_CLIENT_ID_LEN];
    size_t        n_tasks;
    em_task_t     tasks[EM_MAX_TASKS_PER_JOB];
} em_submit_msg_t;

/* ── Structure du message de résultat (Orchestrateur → EM) ───────────────── */

typedef struct {
    long              mtype;                       /* toujours EM_MSG_JOB_RESULT */
    uint64_t          job_id;
    bool              success;
    size_t            n_results;
    em_task_result_t  results[EM_MAX_TASKS_PER_JOB];
} em_result_msg_t;

/* ── Cycle de vie ─────────────────────────────────────────────────────────── */

/*
 * em_ipc_init() — Ouvre (ou crée) les deux files de messages.
 * Retour : 0 si OK, -1 si erreur système (errno positionné).
 */
int  em_ipc_init(void);

/*
 * em_ipc_destroy() — Ferme les handles IPC.
 * Ne supprime PAS les files : l'Orchestrateur en est propriétaire.
 */
void em_ipc_destroy(void);

/* ── Communication ────────────────────────────────────────────────────────── */

/*
 * em_ipc_submit_job() — Envoie un job à l'orchestrateur.
 * Retour : 0 si OK, -1 si erreur IPC.
 */
int em_ipc_submit_job(const em_job_t  *job,
                       const em_task_t *tasks,
                       size_t           n_tasks);

/*
 * em_ipc_wait_result() — Attend le résultat d'un job (bloquant).
 *
 * Retour :
 *    0  : résultat reçu.
 *   -1  : erreur IPC.
 *   -2  : timeout expiré (timeout_s > 0).
 */
int em_ipc_wait_result(uint64_t        job_id,
                        em_result_msg_t *result_msg,
                        int             timeout_s);

#endif /* EM_IPC_H */
