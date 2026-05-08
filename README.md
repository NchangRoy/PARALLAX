# PARALLAX Orchestrator

Module d'orchestration du framework de calcul distribué PARALLAX.
Auteur : NZIELEU Nathan (22P587).

## Architecture

5 modules en C99/C11, single-threaded event-driven :

```
parallax_types.h    -> Types fondamentaux (worker, task, result)
worker_table        -> Vue du cluster (états des workers)
task_pool           -> Bag-of-tasks par job
job_table           -> Registre des jobs et leurs pools
scheduler           -> Deficit Round Robin pondéré (Shreedhar & Varghese 1996)
orchestrator        -> Event loop : reçoit événements, produit actions
```

## Compilation

```bash
make all        # compile tous les binaires
make test       # lance les 65 tests
make memcheck   # tests sous valgrind
make demo       # construit la démo
./demo          # exécute une simulation visuelle
```

## Garanties

- 65 tests automatisés, 0 échec
- 0 fuite mémoire (valgrind)
- 0 warning avec `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`
- Compatible C11 strict

## Sémantique distribuée

- **At-least-once** : une tâche peut être exécutée plusieurs fois en cas de
  panne. Les tâches doivent être idempotentes.
- **Déduplication des résultats** : résultats tardifs de zombies rejetés
  silencieusement.
- **Retries bornés** : `max_retries` (défaut 3) par tâche.
- **Tolérance aux pannes** : un worker qui meurt voit ses tâches redistribuées.

## Intégration

Voir `docs/RFC-001-protocol.md` pour le contrat d'interface avec :
- Ngonga (réseau)
- Bala/Ulrich (Execution Master)
- Roy (Parser)
- Farelle/Kouam (Controller / Panne Detection)
- Tchapda (Worker Execution)

## Référence académique

Algorithme de scheduling : Shreedhar, M., & Varghese, G. (1996).
*Efficient Fair Queueing using Deficit Round Robin*.
ACM SIGCOMM Computer Communication Review.
