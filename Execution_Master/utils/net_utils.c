/* ==========================================================================
 * net_utils.c  —  Lightweight network utility helpers
 *
 * Extracted from Agent_Init/init.c so that the Execution_Master binaries can
 * link load_network_interface() and get_local_ip() without pulling in the
 * entire agent initialisation stack (monitoring, state_receiver, etc.).
 * ========================================================================== */

#include "net_utils.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

/* Path to the agent config file — same constant used in init.c */
#ifndef CONF_FILE
#define CONF_FILE "parallax/parallax.conf"
#endif

/*
 * load_network_interface
 *
 * Description: Reads the "interface=" field from the agent config file and
 *              copies it into iface.  Falls back to "eth0" if the file is
 *              missing or the field is not found.
 *
 * Input:  iface   — caller-supplied buffer to receive the interface name.
 *         max_len — size of iface in bytes.
 *
 * Output: none (iface is always populated on return).
 */
void load_network_interface(char *iface, size_t max_len)
{
    FILE *f = fopen(CONF_FILE, "r");
    if (!f) {
        /* Try alternative paths to locate config file */
        f = fopen("parallax/parallax.conf", "r");
        if (!f) {
            f = fopen("../parallax/parallax.conf", "r");
            if (!f) {
                f = fopen("parallax/agent.conf", "r");
                if (!f) {
                    f = fopen("../parallax/agent.conf", "r");
                }
            }
        }
    }

    if (!f) {
        strncpy(iface, "eth0", max_len - 1);
        iface[max_len - 1] = '\0';
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "interface=", 10) == 0) {
            strncpy(iface, line + 10, max_len - 1);
            iface[max_len - 1] = '\0';
            iface[strcspn(iface, "\r\n")] = '\0'; /* strip both CR and LF */

            /* Validate: only allow [a-zA-Z0-9_-] */
            for (int i = 0; iface[i]; i++) {
                char c = iface[i];
                if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
                    !(c >= '0' && c <= '9') && c != '_' && c != '-') {
                    strncpy(iface, "eth0", max_len - 1);
                    iface[max_len - 1] = '\0';
                    break;
                }
            }

            fclose(f);
            return;
        }
    }

    fclose(f);
    strncpy(iface, "eth0", max_len - 1);
    iface[max_len - 1] = '\0';
}

/*
 * get_local_ip
 *
 * Description: Uses getifaddrs to find the IPv4 address of the configured interface.
 *              If the requested interface fails or doesn't have an IPv4 address,
 *              it automatically falls back to finding the first active, UP,
 *              non-loopback IPv4 interface.
 *
 * Input:  ip         — caller-supplied buffer to receive the IP string.
 *         max_len    — size of ip in bytes.
 *         iface_name — network interface to query (e.g. "eth0").
 *
 * Output: none (ip is always populated on return).
 */
void get_local_ip(char *ip, size_t max_len, const char *iface_name)
{
    ip[0] = '\0';

    struct ifaddrs *ifap = NULL, *ifa = NULL;
    if (getifaddrs(&ifap) == -1) {
        perror("get_local_ip: getifaddrs");
        strncpy(ip, "0.0.0.0", max_len - 1);
        ip[max_len - 1] = '\0';
        return;
    }

    /* First pass: search for the exact matching interface name */
    if (iface_name && strlen(iface_name) > 0) {
        for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (strcmp(ifa->ifa_name, iface_name) == 0) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                const char *addr = inet_ntop(AF_INET, &sa->sin_addr, ip, max_len);
                if (addr) {
                    break;
                }
            }
        }
    }

    /* Second pass fallback: find the first active, UP, non-loopback IPv4 interface */
    if (strlen(ip) == 0) {
        for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;

            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            const char *addr = inet_ntop(AF_INET, &sa->sin_addr, ip, max_len);
            if (addr) {
                break;
            }
        }
    }

    /* Third pass fallback: loopback */
    if (strlen(ip) == 0) {
        for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;

            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            const char *addr = inet_ntop(AF_INET, &sa->sin_addr, ip, max_len);
            if (addr) {
                break;
            }
        }
    }

    freeifaddrs(ifap);

    if (strlen(ip) == 0) {
        strncpy(ip, "0.0.0.0", max_len - 1);
        ip[max_len - 1] = '\0';
    }
}
