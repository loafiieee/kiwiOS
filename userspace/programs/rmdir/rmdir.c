#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int rmdir_one(const char* path) {
    kiwi_stat_t st;

    if (sys_stat(path, &st) != 0) {
        out("rmdir: ");
        out(path);
        out(": not found\n");
        return 1;
    }
    if (st.type != KIWI_VNODE_DIR) {
        out("rmdir: ");
        out(path);
        out(": not a directory\n");
        return 1;
    }
    /* The kernel unlink removes empty directories and refuses non-empty ones. */
    if (sys_unlink(path) < 0) {
        out("rmdir: ");
        out(path);
        out(": failed (directory not empty or filesystem read-only)\n");
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int rc = 0;

    if (argc < 2) {
        out("usage: rmdir <path>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (rmdir_one(argv[i]) != 0) {
            rc = 1;
        }
    }
    return rc;
}
