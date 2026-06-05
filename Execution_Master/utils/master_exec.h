#ifndef MASTER_EXEC_H
#define MASTER_EXEC_H

#include <stddef.h>

void execute_fxn(void * data ,size_t total_size , char * fxn_name,int node_count);
void load_network_interface(char *iface, size_t max_len);
void get_local_ip(char *ip, size_t max_len, const char *iface_name);

#endif