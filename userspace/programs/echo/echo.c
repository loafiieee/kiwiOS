#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        out(argv[i]);
        if (i + 1 < argc) {
            out(" ");
        }
    }
    out("\n");
    return 0;
}
