/* === Parallax: embedded program source (auto-generated) === */
#include <string.h>
#include "parallax/parallax_param.h"
extern void execute_fxn(ParallaxParam *, int, char *, ParallaxExecutionCtx *, const char *, const char *);
static const char *__parallax_prog_code__ = "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include \"parallax/parallax_param.h\"\n\n\ntypedef void *(*fn)(void *);\n\nfn matcher(char *name) {\n    return NULL;\n}\n\nint main() { return 0; }\n";
static const char *__parallax_prog_name__ = "d361e3cd-956c-4f33-8207-c4dec0a7f1eb";

// __parallax_callback_host__ = "127.0.0.1"
// __parallax_callback_port__ = "5000"
// __parallax_prog_name__ = "d361e3cd-956c-4f33-8207-c4dec0a7f1eb"
#include <stdio.h>
int main(void) {
    printf("Hello from PARALLAX, no distribution needed!\n");
    for (int i = 1; i <= 5; i++) {
        printf("Line %d\n", i);
    }
    return 0;
}


typedef void *(*fn)(void *);

fn matcher(char *name) {
    return NULL;
}
