/*
 * em_parser.h — Interface du Parser de l'Execution Master
 *
 * Responsabilité UNIQUE : décomposer un job (A × B) en un tableau
 * de tâches élémentaires prêtes à être distribuées par l'orchestrateur.
 *
 * Le Parser ne sait pas où les tâches seront exécutées, ni par combien
 * de workers, ni ce qui se passe en cas d'échec. Il produit uniquement
 * le "bag of tasks".
 *
 * Algorithme de décomposition (base 10, chunks de taille fixe) :
 *
 *   A = 987654321, chunk_digits = 3
 *
 *   chunk[0] = 321  position = 0   →  321 × B × 10^0
 *   chunk[1] = 654  position = 3   →  654 × B × 10^3
 *   chunk[2] = 987  position = 6   →  987 × B × 10^6
 *
 *   Vérification : 321 + 654×10^3 + 987×10^6 = 987 654 321 = A  ✓
 *
 * La propriété garantie par le parser :
 *   Σ (task[i].chunk_value × 10^task[i].position) == job->operand_A
 */

#ifndef EM_PARSER_H
#define EM_PARSER_H

#include "em_types.h"

/*
 * em_parser_decompose() — Décompose un job en tâches.
 *
 * Paramètres :
 *   job      : descripteur du job (operand_A, operand_B, chunk_digits).
 *   tasks    : tableau de sortie, doit avoir EM_MAX_TASKS_PER_JOB entrées.
 *   n_tasks  : (sortie) nombre de tâches produites.
 *
 * Retour :
 *    0  : succès.
 *   -1  : paramètres invalides (pointeurs NULL, chunk_digits <= 0).
 *   -2  : trop de chunks (A trop grand pour chunk_digits trop petit).
 *
 * Garanties :
 *   - Chaque task.task_id est unique dans [0..n_tasks-1].
 *   - Chaque task.job_id == job->job_id.
 *   - task.state == EM_TASK_PENDING après l'appel.
 *   - La somme positionnelle des chunks reconstitue job->operand_A.
 */
int em_parser_decompose(const em_job_t *job,
                         em_task_t      *tasks,
                         size_t         *n_tasks);

/*
 * em_parser_verify() — Vérifie qu'un ensemble de tâches reconstitue A.
 *
 * Utile en debug et pour les tests unitaires.
 * Retourne true si Σ (task[i].chunk_value × 10^task[i].position) == expected_A.
 */
bool em_parser_verify(const em_task_t *tasks, size_t n_tasks, long expected_A);

#endif /* EM_PARSER_H */
