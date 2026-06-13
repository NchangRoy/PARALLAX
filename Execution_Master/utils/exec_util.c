#include <stdio.h>
#include <stdlib.h>

char *read_my_source(void)
{
    FILE *f = fopen(__FILE__, "r");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    fread(buf, 1, size, f);
    buf[size] = '\0';

    fclose(f);
    return buf;
}