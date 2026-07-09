#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

#define MOUNT_PATH_MAX 256u

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int str_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char** argv) {
    const char* source;
    const char* target = "/";
    const char* check_path;
    char dev_path[MOUNT_PATH_MAX];
    kiwi_stat_t st;

    if (argc < 2 || argc > 3) {
        out("usage: mount <device> [path]\n");
        return 1;
    }
    source = argv[1];
    if (argc == 3) {
        target = argv[2];
    }

    /* Existence is checked against the /dev node; the mount itself is given the
     * original source argument (matching the old shell built-in). */
    if (str_starts_with(source, "/dev/")) {
        check_path = source;
    } else {
        size_t sl = strlen(source);
        if (sizeof("/dev/") - 1u + sl + 1u > sizeof(dev_path)) {
            out("mount: device name is too long\n");
            return 1;
        }
        memcpy(dev_path, "/dev/", sizeof("/dev/") - 1u);
        memcpy(dev_path + sizeof("/dev/") - 1u, source, sl + 1u);
        check_path = dev_path;
    }

    if (sys_stat(check_path, &st) != 0) {
        (void)sys_dev_rescan();
        if (sys_stat(check_path, &st) != 0) {
            out("mount: ");
            out(source);
            out(": device not found\n");
            return 1;
        }
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(target, &st) != 0) {
        out("mount: ");
        out(target);
        out(": target directory does not exist\n");
        return 1;
    }
    if (st.type != KIWI_VNODE_DIR) {
        out("mount: ");
        out(target);
        out(": target is not a directory\n");
        return 1;
    }

    if (sys_mount(source, target) != 0) {
        out("mount: mount failed (unsupported filesystem, bad device, or target already mounted)\n");
        return 1;
    }
    return 0;
}
