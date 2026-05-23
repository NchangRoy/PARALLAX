/*
 * em_combiner.h — Interface du Combiner de l'Execution Master
 *
 * Responsabilité UNIQUE : reconstruire le résultat final à partir
 * des résultats partiels retournés par l'orchestrateur.
 *
 * Formule de recombinaison :
 *
 *   final_result = Σ (result[i].partial_result × 10^result[i].position)
 *
 * Propriété garantie :
 *   Si toutes les tâches ont réussi et que le parser a correctement
 *   décomposé A, alors : final_result == A × B.
 *
 * Le Combiner ne sait pas comment les tâches ont été exécutées,
 * ni combien de retries ont eu lieu. Il reçoit uniquement les résultats
 * déjà validés par l'orchestrateur.
 */

#ifndef EM_COMBINER_H
#define EM_COMBINER_H

#include "em_types.h"

/*
 * em_combiner_reconstruct() — Reconstruit le résultat final.
 *
 * Paramètres :
 *   results    : tableau de résultats de tâches (succès et échecs mélangés).
 *   n_results  : nombre d'éléments dans results[].
 *   job_result : (sortie) résultat final du job.
 *
 * Retour :
 *    0  : succès (toutes les tâches ont réussi).
 *   -1  : paramètres invalides.
 *   -2  : au moins une tâche en échec ; job_result->success = false,
 *         job_result->final_result contient la somme partielle des
 *         tâches qui ont réussi (pour diagnostic).
 *
 * Note : le Combiner est appelé APRÈS que l'orchestrateur ait terminé.
 * Il ne prend aucune décision de retry.
 */
int em_combiner_reconstruct(const em_task_result_t *results,
                             size_t                  n_results,
                             em_job_result_t        *job_result);

/*
 * em_combiner_verify() — Vérifie le résultat final contre A×B attendu.
 *
 * Retourne true si job_result->final_result == expected.
 */
bool em_combiner_verify(const em_job_result_t *job_result, long expected);

#endif /* EM_COMBINER_H */
