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
    const char* path = ".";
    kiwi_stat_t st;
    kiwi_dirent_t ent;
    uint64_t index = 0;

    if (argc > 2) {
        outln("usage: ls [path]");
        return 1;
    }
    if (argc == 2) {
        path = argv[1];
    }

    if (sys_stat(path, &st) != 0) {
        outln("ls: cannot access path");
        return 1;
    }
    if (st.type == KIWI_VNODE_FILE) {
        outln(path);
        return 0;
    }
    if (st.type != KIWI_VNODE_DIR) {
        outln("ls: not a file or directory");
        return 1;
    }

    for (;;) {
        int64_t rc;
        memset(&ent, 0, sizeof(ent));
        rc = sys_readdir(path, index, &ent);
        if (rc < 0) {
            outln("ls: read failed");
            return 1;
        }
        if (rc == 0) {
            break;
        }
        outln(ent.name);
        index++;
    }
    return 0;
}
