#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int touch_one(const char* path) {
    kiwi_stat_t st;
    int64_t fd;

    if (sys_stat(path, &st) == 0) {
        if (st.type != KIWI_VNODE_FILE) {
            out("touch: ");
            out(path);
            out(": not a regular file\n");
            return 1;
        }
        /* Already exists; no mtime update is available yet. */
        return 0;
    }

    fd = sys_open(path, KIWI_O_WRONLY | KIWI_O_CREAT);
    if (fd < 0) {
        out("touch: ");
        out(path);
        out(": cannot create\n");
        return 1;
    }
    (void)sys_close((int)fd);
    return 0;
}

int main(int argc, char** argv) {
    int rc = 0;

    if (argc < 2) {
        out("usage: touch <path>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (touch_one(argv[i]) != 0) {
            rc = 1;
        }
    }
    return rc;
}
