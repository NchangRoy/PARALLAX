#include <stdio.h>
#include <unistd.h>
int main() {
    printf("\n========================================\n");
    printf("  HELLO FROM THE COMPILED PAYLOAD!\n");
    printf("  PID: %d\n", getpid());
    printf("========================================\n\n");
    return 0;
}
