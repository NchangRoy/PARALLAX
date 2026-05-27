#include <stdio.h>
#include <signal.h>
#include "Agent_Init/init.h"

static void handle_signal(int sig) {
    (void)sig;
    stop_agent();
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    initialize_agent();
    return 0;
}