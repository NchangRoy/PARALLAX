#include <stdio.h>
#include <string.h>
#include "fonctions.h" 

void *add(void *arg) {
    printf("Instance du Worker : Exécution de la fonction ADD \n");
    return arg; 
}

void *square(void *arg) {
    printf("Instance du Worker : Exécution de la fonction SQUARE \n");
    return arg;
}

fn matcher(char *nom_fonction) {
    if (strcmp(nom_fonction, "add") == 0) {
        return add; 
    } 
    else if (strcmp(nom_fonction, "square") == 0) {
        return square; 
    }
    return NULL; 
}