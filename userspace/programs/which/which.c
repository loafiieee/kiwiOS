#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static void outln(const char* s) {
    out(s);
    out("\n");
}

int main(int argc, char** argv) {
    int rc = 0;

    if (argc < 2) {
        outln("usage: which <command>...");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char path[256];
        if (kiwi_resolve_program_path(argv[i], path, sizeof(path)) == 0) {
            outln(path);
        } else {
            out(argv[i]);
            outln(": not found");
            rc = 1;
        }
    }
    return rc;
}
