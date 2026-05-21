/*
 * master_servant.h
 * 
 * Interface du module Master Servant.
 * Définit les fonctions permettant au système de servir les requêtes du Master Node.
 * Le Master Servant tourne dans un thread séparé et écoute les connexions entrantes.
 */

#ifndef MASTER_SERVANT_H
#define MASTER_SERVANT_H

#include "local_state.h"

/* Port TCP sur lequel le Master Servant écoute les connexions entrantes */
#define MASTER_SERVANT_PORT 9090

/*
 * master_servant_thread()
 * Point d'entrée du thread Master Servant.
 * 
 * Paramètre :
 *   arg : Pointeur vers une structure LocalStateStorage contenant l'état des nœuds
 * 
 * Retour :
 *   NULL (standard pour les threads POSIX)
 * 
 * Fonction :
 *   - Initialise le serveur socket TCP
 *   - Écoute les connexions entrantes sur le port MASTER_SERVANT_PORT
 *   - Pour chaque connexion, appelle handle_request pour traiter la requête
 */
void *master_servant_thread(void *arg);

/*
 * handle_request()
 * Traite une requête entrante du Master Node.
 * 
 * Paramètres :
 *   client_fd : Descripteur de fichier socket pour la connexion client
 *   state : Pointeur vers le stockage local des états des nœuds
 * 
 * Fonction :
 *   - Lit le type de requête envoyé par le client (1 byte)
 *   - Appelle la fonction appropriée selon le type de requête
 *   - Envoie les données demandées au client
 */
void handle_request(int client_fd, LocalStateStorage *state);

#endif