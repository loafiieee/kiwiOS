#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int mkdir_one(const char* path) {
    kiwi_stat_t st;

    if (sys_stat(path, &st) == 0) {
        out("mkdir: ");
        out(path);
        out(st.type == KIWI_VNODE_DIR ? ": already exists\n"
                                      : ": exists and is not a directory\n");
        return 1;
    }

    if (sys_mkdir(path, 0755u) < 0) {
        out("mkdir: ");
        out(path);
        out(": create failed (filesystem may be read-only or the parent is missing)\n");
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int rc = 0;

    if (argc < 2) {
        out("usage: mkdir <path>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (mkdir_one(argv[i]) != 0) {
            rc = 1;
        }
    }
    return rc;
}
