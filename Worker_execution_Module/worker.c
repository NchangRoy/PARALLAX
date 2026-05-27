#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>  //  Nécessaire pour les threads
#include "fonctions.h"

#include<string.h>
#define PORT 8888
#define MASTER_ADDRESS "127.0.0.1"

// Variable globale pour contrôler l'état du Worker
volatile int running = 1; 

// 1. La fonction exécutée par le thread
void *worker_thread_run(void *arg)
{
    int server_fd, worker_fd;
    struct sockaddr_in addr;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return NULL;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        return NULL;
    }

    printf("Worker listening on port %d...\n", PORT);

    worker_fd = accept(server_fd, NULL, NULL);
    if (worker_fd < 0) {
        perror("accept failed");
        return NULL;
    }

    printf("Client connected!\n");

    while (running) {

    char buffer[256] = {0};

    int bytes_received = recv(
        worker_fd,
        buffer,
        sizeof(buffer) - 1,
        0
    );

    if (bytes_received <= 0) {
        printf("Client déconnecté ou erreur\n");
        break;
    }

    buffer[bytes_received] = '\0';

    printf("Message reçu : %s\n", buffer);

    /*
     * Expected format:
     * add 42
     * square 9
     */

    char function_name[64] = {0};
    int value = 0;

    int parsed = sscanf(buffer, "%63s %d",
                        function_name,
                        &value);

    if (parsed != 2) {
        printf("Format invalide\n");

        const char *err = "Format invalide\n";
        send(worker_fd, err, strlen(err), 0);
        continue;
    }

    fn fonction_calcul = matcher(function_name);

    if (fonction_calcul != NULL) {

        int *result = (int *)fonction_calcul(&value);

        char response[128];

        snprintf(response,
                 sizeof(response),
                 "Résultat = %d\n",
                 *result);

        send(worker_fd,
             response,
             strlen(response),
             0);

    } else {

        const char *err = "Fonction inconnue\n";

        send(worker_fd,
             err,
             strlen(err),
             0);
    }
}

    close(worker_fd);
    close(server_fd);

    return NULL;
}
// 2. La fonction pour arrêter le Worker
void worker_stop() {
    printf("Signal d'arrêt reçu... \n");
    running = 0; // Fait sortir le thread de sa boucle while
}

// 3. Le main sert maintenant juste à lancer et tester le thread
int main() {
    pthread_t thread_id;

    // Lancement du thread du Worker
    if (pthread_create(&thread_id, NULL, worker_thread_run, NULL) != 0) {
        perror("Erreur de création du thread ");
        return 1;
    }

    // Le programme principal attend 10 secondes puis arrête le worker pour le test
    sleep(10);
    worker_stop();

    // On attend que le thread se termine proprement
    pthread_join(thread_id, NULL);

    return 0;
}