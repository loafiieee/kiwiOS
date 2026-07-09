#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

#define MV_PATH_MAX 256u
#define MV_BUF 512u

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int streq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

static int path_is_dir(const char* p) {
    kiwi_stat_t st;
    return sys_stat(p, &st) == 0 && st.type == KIWI_VNODE_DIR;
}

static const char* path_basename(const char* path) {
    const char* last = path;

    if (!path || !*path) {
        return NULL;
    }
    for (; *path; path++) {
        if (*path == '/' && path[1] != '\0') {
            last = path + 1;
        }
    }
    return last;
}

static int resolve_target(const char* src, const char* dst, char* dst_buf, size_t dst_size) {
    const char* base;
    size_t dl, bl;

    if (!path_is_dir(dst)) {
        dl = strlen(dst);
        if (dl + 1 > dst_size) {
            return -1;
        }
        memcpy(dst_buf, dst, dl + 1);
        return 0;
    }

    base = path_basename(src);
    if (!base || !*base) {
        return -1;
    }
    dl = strlen(dst);
    bl = strlen(base);
    if (streq(dst, "/")) {
        if (1 + bl + 1 > dst_size) {
            return -1;
        }
        dst_buf[0] = '/';
        memcpy(dst_buf + 1, base, bl + 1);
        return 0;
    }
    if (dl + 1 + bl + 1 > dst_size) {
        return -1;
    }
    memcpy(dst_buf, dst, dl);
    dst_buf[dl] = '/';
    memcpy(dst_buf + dl + 1, base, bl + 1);
    return 0;
}

static int copy_file(const char* src, const char* dst) {
    char buf[MV_BUF];
    int64_t s, d;

    if (streq(src, dst)) {
        return -1;
    }
    s = sys_open(src, KIWI_O_RDONLY);
    if (s < 0) {
        return -1;
    }
    d = sys_open(dst, KIWI_O_WRONLY | KIWI_O_CREAT | KIWI_O_TRUNC);
    if (d < 0) {
        (void)sys_close((int)s);
        return -1;
    }
    for (;;) {
        int64_t n = sys_read((int)s, buf, sizeof(buf));
        if (n < 0) {
            (void)sys_close((int)s);
            (void)sys_close((int)d);
            (void)sys_unlink(dst);
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (sys_write((int)d, buf, (uint64_t)n) != n) {
            (void)sys_close((int)s);
            (void)sys_close((int)d);
            (void)sys_unlink(dst);
            return -1;
        }
    }
    (void)sys_close((int)s);
    (void)sys_close((int)d);
    return 0;
}

int main(int argc, char** argv) {
    kiwi_stat_t st;
    char target[MV_PATH_MAX];

    if (argc != 3) {
        out("usage: mv <src> <dst>\n");
        return 1;
    }
    if (sys_stat(argv[1], &st) != 0 || st.type != KIWI_VNODE_FILE) {
        out("mv: only supports regular files\n");
        return 1;
    }
    if (resolve_target(argv[1], argv[2], target, sizeof(target)) != 0) {
        out("mv: failed\n");
        return 1;
    }
    /* Copy then remove the source, matching the old shell built-in. */
    if (copy_file(argv[1], target) != 0 || sys_unlink(argv[1]) != 0) {
        out("mv: failed\n");
        return 1;
    }
    return 0;
}
