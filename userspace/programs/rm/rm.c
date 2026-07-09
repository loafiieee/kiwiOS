#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

#define RM_PATH_MAX 256u

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int streq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

static void err(const char* path, const char* reason) {
    out("rm: ");
    if (path && *path) {
        out(path);
        out(": ");
    }
    out(reason);
    out("\n");
}

static int str_contains_char(const char* s, char needle) {
    for (; s && *s; s++) {
        if (*s == needle) {
            return 1;
        }
    }
    return 0;
}

static int path_join_child(const char* parent, const char* child, char* dst, size_t dst_size) {
    size_t pl, cl;

    if (!parent || !child || !*parent || !*child || dst_size < 2) {
        return -1;
    }
    pl = strlen(parent);
    cl = strlen(child);

    /* "." means the current directory: children are just their bare names. */
    if (streq(parent, ".")) {
        if (cl + 1 > dst_size) {
            return -1;
        }
        memcpy(dst, child, cl + 1);
        return 0;
    }
    if (streq(parent, "/")) {
        if (1 + cl + 1 > dst_size) {
            return -1;
        }
        dst[0] = '/';
        memcpy(dst + 1, child, cl + 1);
        return 0;
    }
    if (pl + 1 + cl + 1 > dst_size) {
        return -1;
    }
    memcpy(dst, parent, pl);
    dst[pl] = '/';
    memcpy(dst + pl + 1, child, cl + 1);
    return 0;
}

static int rm_recursive(const char* path, int recursive, int force) {
    kiwi_stat_t st;

    if (!path || !*path) {
        return -1;
    }
    if (streq(path, "/")) {
        if (!force) {
            err(path, "refusing to remove root");
        }
        return -1;
    }
    if (sys_stat(path, &st) != 0) {
        if (!force) {
            err(path, "not found");
        }
        return force ? 0 : -1;
    }

    if (st.type == KIWI_VNODE_DIR) {
        uint64_t index = 0;

        if (!recursive) {
            err(path, "is a directory; use -r");
            return -1;
        }
        for (;;) {
            kiwi_dirent_t ent;
            char child[RM_PATH_MAX];
            int64_t rc;

            memset(&ent, 0, sizeof(ent));
            rc = sys_readdir(path, index, &ent);
            if (rc < 0) {
                err(path, "cannot read directory");
                return -1;
            }
            if (rc == 0) {
                break;
            }
            if (streq(ent.name, ".") || streq(ent.name, "..")) {
                index++;
                continue;
            }
            if (path_join_child(path, ent.name, child, sizeof(child)) != 0) {
                err(path, "child path is too long");
                return -1;
            }
            if (rm_recursive(child, recursive, force) != 0 && !force) {
                return -1;
            }
            index = 0;
        }
    } else if (st.type != KIWI_VNODE_FILE) {
        err(path, "unsupported file type");
        return -1;
    }

    if (sys_unlink(path) < 0) {
        if (!force) {
            err(path, "remove failed; filesystem may be read-only, busy, or unsupported");
        }
        return -1;
    }
    return 0;
}

/* Only a bare star, or a directory path ending in slash-then-star, are
 * supported, matching the old shell built-in. Returns 1 when a glob directory
 * was extracted into out_dir, 0 for a literal path, -2 for an unsupported
 * wildcard pattern, -1 on error. */
static int glob_dir(const char* arg, char* out_dir, size_t out_size) {
    size_t len;

    if (!arg || out_size < 2) {
        return -1;
    }
    if (streq(arg, "*")) {
        out_dir[0] = '.';
        out_dir[1] = '\0';
        return 1;
    }
    len = strlen(arg);
    if (len >= 2u && arg[len - 1u] == '*' && arg[len - 2u] == '/') {
        size_t parent_len = len - 2u;
        if (parent_len == 0u) {
            out_dir[0] = '/';
            out_dir[1] = '\0';
            return 1;
        }
        if (parent_len + 1u > out_size) {
            return -1;
        }
        memcpy(out_dir, arg, parent_len);
        out_dir[parent_len] = '\0';
        return 1;
    }
    return str_contains_char(arg, '*') ? -2 : 0;
}

static int rm_glob_children(const char* dir, int recursive, int force) {
    kiwi_stat_t st;
    uint64_t index = 0;
    int matched = 0;
    int failed = 0;

    if (!dir || sys_stat(dir, &st) != 0 || st.type != KIWI_VNODE_DIR) {
        if (!force) {
            err(dir, "glob parent is not a directory");
        }
        return -1;
    }
    for (;;) {
        kiwi_dirent_t ent;
        char child[RM_PATH_MAX];
        int64_t rc;

        memset(&ent, 0, sizeof(ent));
        rc = sys_readdir(dir, index, &ent);
        if (rc < 0) {
            if (!force) {
                err(dir, "cannot read directory");
            }
            return -1;
        }
        if (rc == 0) {
            break;
        }
        if (streq(ent.name, ".") || streq(ent.name, "..")) {
            index++;
            continue;
        }
        if (path_join_child(dir, ent.name, child, sizeof(child)) != 0) {
            if (!force) {
                err(dir, "child path is too long");
            }
            return -1;
        }
        matched = 1;
        if (rm_recursive(child, recursive, force) != 0) {
            failed = 1;
            if (!force) {
                return -1;
            }
            index++;
            continue;
        }
        index = 0;
    }

    if (!matched && !force) {
        err(dir, "no matches");
        return -1;
    }
    return failed ? -1 : 0;
}

int main(int argc, char** argv) {
    int recursive = 0;
    int force = 0;
    int saw_path = 0;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (arg[0] == '-' && arg[1] != '\0') {
            for (size_t j = 1; arg[j] != '\0'; j++) {
                if (arg[j] == 'r' || arg[j] == 'R') {
                    recursive = 1;
                } else if (arg[j] == 'f') {
                    force = 1;
                } else {
                    out("usage: rm [-r] [-f] <path>...\n");
                    return 1;
                }
            }
            continue;
        }

        saw_path = 1;
        {
            char dir[RM_PATH_MAX];
            int g = glob_dir(arg, dir, sizeof(dir));

            if (g == 1) {
                if (rm_glob_children(dir, recursive, force) != 0) {
                    rc = 1;
                }
                continue;
            }
            if (g == -2) {
                err(arg, "unsupported wildcard pattern");
                rc = 1;
                continue;
            }
            if (g < 0) {
                err(arg, "invalid wildcard path");
                rc = 1;
                continue;
            }
            if (rm_recursive(arg, recursive, force) != 0) {
                rc = 1;
            }
        }
    }

    if (!saw_path && !force) {
        out("usage: rm [-r] [-f] <path>...\n");
        return 1;
    }
    return rc;
}
