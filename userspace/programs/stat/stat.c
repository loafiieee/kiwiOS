#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static void out_u64(uint64_t v) {
    char buf[24];
    size_t i = sizeof(buf);

    buf[--i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    }
    while (v != 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    out(&buf[i]);
}

int main(int argc, char** argv) {
    kiwi_stat_t st;

    if (argc != 2) {
        out("usage: stat <path>\n");
        return 1;
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(argv[1], &st) != 0) {
        out("stat: cannot stat path\n");
        return 1;
    }

    out("ino=");
    out_u64(st.ino);
    out(" type=");
    if (st.type == KIWI_VNODE_DIR) {
        out("dir");
    } else if (st.type == KIWI_VNODE_FILE) {
        out("file");
    } else {
        out("unknown");
    }
    out(" size=");
    out_u64(st.size);
    out(" links=");
    out_u64(st.link_count);
    out("\n");
    return 0;
}
