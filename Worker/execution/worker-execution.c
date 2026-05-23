#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#define PORT 8080
#define master_address "127.0.0.1"


int main(){

    int worker_fd= socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in master_addr;
    master_addr.sin_family = AF_INET;
    master_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, master_address, &master_addr.sin_addr);
    connect(worker_fd, (struct sockaddr *)&master_addr, sizeof(master_addr));

    // donner envoyer par le master
    struct Task {
    char name_function[32]; // Un tableau  pour le nom
    int data_count;        // Un tableau  pour les données
    };



    //reception l'en-tête dans le tcp parle worker
    struct Task task;
     recv(worker_fd, &task, sizeof(task), 0);

    // allocation de la mémoire pour les données
    double *data= malloc(task.data_count*sizeof(double));

    //reception des données
    recv(worker_fd, data, task.data_count*sizeof(double), 0);


}
