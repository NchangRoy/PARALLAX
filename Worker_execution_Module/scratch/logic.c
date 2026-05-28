#include <stdio.h>
#include <unistd.h>
int worker_entry() {
    printf("Coming from logic\n");
    _exit(0);
}
