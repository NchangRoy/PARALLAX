/*
 * em_types.h — Types fondamentaux de l'Execution Master
 *
 * Tous les types partagés entre les sous-modules :
 *   em_parser, em_combiner, em_ipc, execution_master.
 *
 * Conventions :
 *   - job_id   : uint64_t unique généré par l'EM au moment de la soumission.
 *   - task_id  : uint32_t, index dans le tableau de tâches (0..n_tasks-1).
 *   - position : exposant de 10 pour la recomposition (ex: position=6 → ×10^6).
 *   - Pas d'allocation dynamique dans les structures de base.
 */

#ifndef EM_TYPES_H
#define EM_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Limites ──────────────────────────────────────────────────────────────── */

#define EM_MAX_TASKS_PER_JOB  64
#define EM_CLIENT_ID_LEN      64
#define EM_ERROR_LEN         128

/* ── État d'une tâche tel que l'EM le voit ────────────────────────────────── */

typedef enum {
    EM_TASK_PENDING   = 0,  /* créée, pas encore soumise à l'orchestrateur  */
    EM_TASK_SUBMITTED = 1,  /* soumise, en attente de résultat               */
    EM_TASK_DONE      = 2,  /* résultat reçu avec succès                     */
    EM_TASK_FAILED    = 3   /* échec définitif (retries épuisés côté orch.)  */
} em_task_state_t;

/* ── État d'un job ────────────────────────────────────────────────────────── */

typedef enum {
    EM_JOB_CREATED    = 0,
    EM_JOB_SUBMITTED  = 1,
    EM_JOB_COMPLETED  = 2,
    EM_JOB_FAILED     = 3
} em_job_state_t;

/* ── Tâche élémentaire (unité de travail distribuée) ─────────────────────── */

/*
 * Une tâche représente le calcul : chunk_value × multiplier.
 * Le résultat partiel devra ensuite être pondéré par 10^position
 * lors de la recombinaison.
 *
 * Exemple pour A=987654321, B=56789, chunk_digits=3 :
 *   task[0] : chunk_value=321, position=0  → 321  × B × 10^0
 *   task[1] : chunk_value=654, position=3  → 654  × B × 10^3
 *   task[2] : chunk_value=987, position=6  → 987  × B × 10^6
 */
typedef struct {
    uint32_t        task_id;
    uint64_t        job_id;
    long            chunk_value;   /* morceau de A extrait par le parser     */
    long            multiplier;    /* B, identique pour toutes les tâches    */
    int             position;      /* exposant de 10 pour la recomposition   */
    em_task_state_t state;
} em_task_t;

/* ── Résultat d'une tâche (retourné par l'orchestrateur) ─────────────────── */

typedef struct {
    uint32_t task_id;
    uint64_t job_id;
    bool     success;
    long     partial_result;             /* chunk_value × multiplier         */
    int      position;                   /* même que em_task_t.position      */
    char     error[EM_ERROR_LEN];        /* vide si succès                   */
} em_task_result_t;

/* ── Descripteur de job ───────────────────────────────────────────────────── */

typedef struct {
    uint64_t       job_id;
    char           client_id[EM_CLIENT_ID_LEN];
    long           operand_A;       /* opérande à décomposer en chunks          */
    long           operand_B;       /* multiplicateur commun à toutes les tâches*/
    int            chunk_digits;    /* nombre de chiffres décimaux par chunk    */
    em_job_state_t state;
} em_job_t;

/* ── Résultat final d'un job ─────────────────────────────────────────────── */

typedef struct {
    uint64_t job_id;
    bool     success;
    long     final_result;   /* Σ (partial_result_i × 10^position_i)         */
    int      n_tasks;
    int      n_succeeded;
    int      n_failed;
} em_job_result_t;

#endif /* EM_TYPES_H */
