#include <stddef.h>
#include <stdint.h>
#include "glob.h"
#include "kiwi_syscall.h"
#include "string.h"

#define CP_PATH_MAX 256u
#define CP_BUF 512u

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int streq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

static void err2(const char* prefix, const char* path, const char* suffix) {
    out("cp: ");
    if (prefix) out(prefix);
    if (path) out(path);
    if (suffix) out(suffix);
    out("\n");
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

/* When dst is an existing directory, the copy lands at dst/<basename(src)>;
 * otherwise dst is used verbatim as the destination file. */
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

typedef enum {
    CP_OK = 0,
    CP_ERR_SAME,
    CP_ERR_OPEN_SRC,
    CP_ERR_OPEN_DST,
    CP_ERR_READ,
    CP_ERR_WRITE,
    CP_ERR_VERIFY,
} cp_status_t;

static cp_status_t copy_file(const char* src, const char* dst, uint64_t expected_size) {
    char buf[CP_BUF];
    int64_t s, d;
    kiwi_stat_t dst_st;

    if (streq(src, dst)) {
        return CP_ERR_SAME;
    }
    s = sys_open(src, KIWI_O_RDONLY);
    if (s < 0) {
        return CP_ERR_OPEN_SRC;
    }
    d = sys_open(dst, KIWI_O_WRONLY | KIWI_O_CREAT | KIWI_O_TRUNC);
    if (d < 0) {
        (void)sys_close((int)s);
        return CP_ERR_OPEN_DST;
    }
    for (;;) {
        int64_t n = sys_read((int)s, buf, sizeof(buf));
        if (n < 0) {
            (void)sys_close((int)s);
            (void)sys_close((int)d);
            (void)sys_unlink(dst);
            return CP_ERR_READ;
        }
        if (n == 0) {
            break;
        }
        if (sys_write((int)d, buf, (uint64_t)n) != n) {
            (void)sys_close((int)s);
            (void)sys_close((int)d);
            (void)sys_unlink(dst);
            return CP_ERR_WRITE;
        }
    }
    (void)sys_close((int)s);
    (void)sys_close((int)d);

    memset(&dst_st, 0, sizeof(dst_st));
    if (sys_stat(dst, &dst_st) != 0 || dst_st.type != KIWI_VNODE_FILE ||
        dst_st.size != expected_size) {
        (void)sys_unlink(dst);
        return CP_ERR_VERIFY;
    }

    return CP_OK;
}

static int has_glob_magic(const char* s) {
    for (; s && *s; s++) {
        if (*s == '*' || *s == '?' || *s == '[') {
            return 1;
        }
    }
    return 0;
}

static size_t count_sources(int argc, char** argv) {
    size_t count = 0;

    for (int i = 1; i < argc - 1; i++) {
        if (has_glob_magic(argv[i])) {
            glob_t g;
            memset(&g, 0, sizeof(g));
            if (glob(argv[i], 0, NULL, &g) == 0) {
                count += g.gl_pathc;
                globfree(&g);
                continue;
            }
            globfree(&g);
        }
        count++;
    }

    return count;
}

static int copy_one(const char* src, const char* dst, int dst_is_dir) {
    kiwi_stat_t st;
    char target[CP_PATH_MAX];
    cp_status_t rc;

    if (sys_stat(src, &st) != 0) {
        err2("source not found: ", src, NULL);
        return 1;
    }
    if (st.type != KIWI_VNODE_FILE) {
        err2("not a regular file: ", src, NULL);
        return 1;
    }
    if (!dst_is_dir && path_is_dir(dst)) {
        dst_is_dir = 1;
    }
    if (resolve_target(src, dst, target, sizeof(target)) != 0) {
        err2("target path too long for: ", src, NULL);
        return 1;
    }

    rc = copy_file(src, target, st.size);
    switch (rc) {
        case CP_OK:
            return 0;
        case CP_ERR_SAME:
            err2("source and target are the same: ", src, NULL);
            return 1;
        case CP_ERR_OPEN_SRC:
            err2("open source failed: ", src, NULL);
            return 1;
        case CP_ERR_OPEN_DST:
            err2("open target failed: ", target, " (missing directory, read-only FS, or unsupported create)");
            return 1;
        case CP_ERR_READ:
            err2("read failed: ", src, NULL);
            return 1;
        case CP_ERR_WRITE:
            err2("write failed: ", target, " (disk full, read-only FS, or unsupported write)");
            return 1;
        case CP_ERR_VERIFY:
            err2("verify failed: ", target, " (target size did not match source)");
            return 1;
    }

    return 1;
}

static int copy_arg(const char* src_arg, const char* dst, int dst_is_dir) {
    int failed = 0;

    if (has_glob_magic(src_arg)) {
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(src_arg, 0, NULL, &g) != 0) {
            globfree(&g);
            err2("source not found: ", src_arg, NULL);
            return 1;
        }
        for (size_t i = 0; i < g.gl_pathc; i++) {
            if (copy_one(g.gl_pathv[i], dst, dst_is_dir) != 0) {
                failed = 1;
            }
        }
        globfree(&g);
        return failed;
    }

    return copy_one(src_arg, dst, dst_is_dir);
}

int main(int argc, char** argv) {
    const char* dst;
    int dst_is_dir;
    int failed = 0;

    if (argc < 3) {
        out("usage: cp <src>... <dst>\n");
        return 1;
    }

    dst = argv[argc - 1];
    dst_is_dir = path_is_dir(dst);
    if (count_sources(argc, argv) > 1u && !dst_is_dir) {
        err2("target is not a directory: ", dst, NULL);
        return 1;
    }

    for (int i = 1; i < argc - 1; i++) {
        if (copy_arg(argv[i], dst, dst_is_dir) != 0) {
            failed = 1;
        }
    }

    return failed ? 1 : 0;
}
