#include <stdio.h>

typedef void *(*fn)(void *);

void *my_dummy_fxn(void *arg) {
    printf("Hello from generated program!\n");
    return NULL;
}

fn matcher(char *name) {
    return my_dummy_fxn;
}

int main() { return 0; }
