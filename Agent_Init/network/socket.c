#include "socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>

/*
 * Cree une connexion TCP sortante vers l'adresse IP et le port donnes.
 * Retourne une structure connection active, ou NULL en cas d'erreur.
 */
connection *create_connection(char *Ip, int port)
{
    connection *conn = malloc(sizeof(connection));
    if (!conn) return NULL;

    conn->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->sockfd < 0) {
        perror("socket");
        free(conn);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, Ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(conn->sockfd);
        free(conn);
        return NULL;
    }

    if (connect(conn->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(conn->sockfd);
        free(conn);
        return NULL;
    }

    strncpy(conn->ip, Ip, sizeof(conn->ip));
    conn->ip[sizeof(conn->ip) - 1] = '\0';

    conn->port = port;
    conn->state = CONN_ACTIVE;

    return conn;
}



/*
 * Cree un socket serveur TCP, l'attache a Ip:port, puis le place en ecoute.
 * Si Ip vaut NULL, le listener est attache par defaut a 127.0.0.1.
 */
connection *create_listener(char *Ip, int port, int backlog)
{
    connection *conn = malloc(sizeof(connection));
    if (!conn) return NULL;

    conn->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->sockfd < 0) {
        perror("socket");
        free(conn);
        return NULL;
    }

    /* This listener stays open for the agent's entire lifetime. Without
       FD_CLOEXEC, every program the master launches via system() (see
       master_thread.c) inherits this fd and can independently accept()
       connections meant for the master, silently stealing worker responses
       and making execute_fxn hang forever waiting for them. */
    fcntl(conn->sockfd, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    setsockopt(conn->sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // 👉 LOCAL DEFAULT
   const char *bind_ip = (Ip == NULL) ? "0.0.0.0" : Ip;

    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(conn->sockfd);
        free(conn);
        return NULL;
    }

    if (bind(conn->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(conn->sockfd);
        free(conn);
        return NULL;
    }

    if (listen(conn->sockfd, backlog) < 0) {
        perror("listen");
        close(conn->sockfd);
        free(conn);
        return NULL;
    }

    strncpy(conn->ip, bind_ip, sizeof(conn->ip));
    conn->ip[sizeof(conn->ip) - 1] = '\0';

    conn->port = port;
    conn->state = CONN_ACTIVE;
    printf("listening for connections on port %d",port);

    return conn;
}



/*
 * Envoie un message_t complet sur une connexion TCP deja etablie.
 * Le nombre d'octets transmis correspond a l'en-tete message_t plus data[].
 */
int send_message(connection *connection, message_t *message)
{
    if (!connection || connection->sockfd < 0 || !message)
        return -1;

    size_t total_size = sizeof(message_t) + message->size;

    ssize_t sent = write(connection->sockfd, message, total_size);

    if (sent != (ssize_t)total_size) {
        perror("write");
        return -1;
    }

    return 0;
}

/*
 * Envoie un message de broadcast UDP sur le port donne.
 *
 * iface_name == NULL (or empty) broadcasts on every up, broadcast-capable
 * interface. Otherwise, only the named interface is used — this is what
 * lets the agent honor the "interface=" setting from parallax.conf instead
 * of blasting the HELLO on every interface on the box (docker/br-* bridges
 * included).
 */
int send_broadcast_message_iface(int port, message_t *message, const char *iface_name)
{
    if (!message) return -1;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket broadcast");
        return -1;
    }

    int broadcastEnable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable)) < 0) {
        perror("setsockopt SO_BROADCAST");
        close(sockfd);
        return -1;
    }

    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == -1) {
        perror("getifaddrs");
        close(sockfd);
        return -1;
    }

    size_t total_size = sizeof(message_t) + message->size;
    int success = 0;
    int want_specific = iface_name && iface_name[0] != '\0';

    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_BROADCAST)) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (want_specific && strcmp(ifa->ifa_name, iface_name) != 0) continue;

        struct sockaddr_in broadcastAddr;
        memcpy(&broadcastAddr, ifa->ifa_broadaddr, sizeof(struct sockaddr_in));
        broadcastAddr.sin_port = htons(port);

        ssize_t sent = sendto(sockfd, message, total_size, 0, (struct sockaddr *)&broadcastAddr, sizeof(broadcastAddr));
        if (sent == (ssize_t)total_size) {
            success = 1;
        } else {
            perror("sendto broadcast specific interface");
        }
    }

    freeifaddrs(ifap);
    close(sockfd);

    if (!success) {
        fprintf(stderr, want_specific
                ? "Failed to broadcast on interface '%s'\n"
                : "Failed to broadcast on any active interface\n",
                iface_name);
        return -1;
    }

    return 0;
}

int send_broadcast_message(int port, message_t *message)
{
    return send_broadcast_message_iface(port, message, NULL);
}
