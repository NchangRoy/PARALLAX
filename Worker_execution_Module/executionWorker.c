// Gère la réception des workloads et la création des threads.
// executionWorker.c

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

// --- Structures minimales représentant un workload ---
typedef struct {
    int id;
    char *source_code;
    char *input_data;
    
    int status;          // 0 pour succès, autre pour erreur
    char *output_result; // Le texte généré par l'exécution
} workload_t;

typedef struct {
    char *results_buffer[100]; // Stockage temporaire
    int count;
    pthread_mutex_t lock;      // Le verrou (Le Mutex)
} shared_queue_t;

shared_queue_t network_queue = {.count = 0, .lock = PTHREAD_MUTEX_INITIALIZER};

// --- Prototypes des fonctions internes ---
void* execution_thread(void *arg);
workload_t* get_next_workload(void);   // récupère le prochain workload (depuis le network thread)

// --- Fonction principale du worker : boucle infinie, reçoit les workloads et crée les threads ---
void execution_worker_run(void) {
    while (1) {
        // 1. RECEIVE WORKLOAD (depuis le network thread)
        workload_t *wl = get_next_workload();
        if (wl == NULL) {
            // Pas de workload pour l'instant, on attend un peu (ou on utilise une condition)
            usleep(10000); // 10ms
            continue;
        }

        printf("[Worker] Reçu workload ID=%d\n", wl->id);

        // 2. CREATE EXECUTION THREAD (thread dédié pour ce workload)
        pthread_t thread_id;
        int ret = pthread_create(&thread_id, NULL, execution_thread, (void*)wl);
        if (ret != 0) {
            perror("pthread_create");
            free(wl->source_code);
            free(wl->input_data);
            free(wl);
            continue;
        }

        // Détacher le thread pour ne pas avoir à faire un join (exécution asynchrone)
        pthread_detach(thread_id);
    }
}

// --- Fonction exécutée par chaque thread dédié ---
void* execution_thread(void *arg) {
    workload_t *wl = (workload_t*)arg;

    printf("[Execution Thread] Démarrage du workload %d\n", wl->id);

    // --- SIMULATION DE CAPTURE (Étape 5 & 6) ---
    sleep(1); 
    
    // On simule une sortie d'exécution
    wl->output_result = strdup("Sortie : Calcul terminé avec succès.");
    wl->status = 0; // Succès

    // --- ENVOI DES RÉSULTATS (Étape 7) ---
    send_results_to_network(wl);

    // Libération finale
    free(wl->source_code);
    free(wl->input_data);
    free(wl->output_result); // Ne pas oublier de libérer le résultat !
    free(wl);

    return NULL;
}

// --- SIMULATION de la réception d'un workload depuis le network thread ---
// Dans la réalité, cette fonction lirait une queue partagée avec le network thread.
workload_t* get_next_workload(void) {
    static int counter = 0;
    // Pour l'exemple, on retourne 5 workloads puis NULL répété (arrêt simulé)
    if (counter >= 5) {
        return NULL;
    }

    workload_t *wl = malloc(sizeof(workload_t));
    wl->id = counter++;
    wl->source_code = strdup("int main() { return 0; }");  // code bidon
    wl->input_data = strdup("input example");
    return wl;
}

//--- Fonction pour envoyer les résultats au network thread (ajout dans une file partagée) ---
void send_results_to_network(workload_t *wl) {
    // --- ZONE CRITIQUE ---
    pthread_mutex_lock(&network_queue.lock); 
    
    // On ajoute le résultat dans la file partagée
    if (network_queue.count < 100) {
        network_queue.results_buffer[network_queue.count] = strdup(wl->output_result);
        network_queue.count++;
        printf("[Mutex] ID=%d stocké en sécurité. Total dans la file : %d\n", wl->id, network_queue.count);
    }

    pthread_mutex_unlock(&network_queue.lock); 
    // --- FIN ZONE CRITIQUE ---
}

// --- Exemple d'utilisation (main) ---
int main(void) {
    printf("Démarrage de l'Execution Worker\n");
    execution_worker_run();  // boucle infinie
    return 0;
}