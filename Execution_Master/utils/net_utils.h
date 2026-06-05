/* ==========================================================================
 * net_utils.h  —  Lightweight network utility helpers
 *
 * Provides load_network_interface() and get_local_ip() without depending on
 * the full agent init stack (monitoring, state_receiver, etc.).
 * ========================================================================== */

#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <stddef.h>

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
void load_network_interface(char *iface, size_t max_len);

/*
 * get_local_ip
 *
 * Description: Runs "ip addr show <iface>" and parses the first IPv4 address.
 *              Falls back to "0.0.0.0" if parsing fails.
 *
 * Input:  ip         — caller-supplied buffer to receive the IP string.
 *         max_len    — size of ip in bytes.
 *         iface_name — network interface to query (e.g. "eth0").
 *
 * Output: none (ip is always populated on return).
 */
void get_local_ip(char *ip, size_t max_len, const char *iface_name);

#endif /* NET_UTILS_H */
