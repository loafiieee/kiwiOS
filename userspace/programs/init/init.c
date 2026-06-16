#include <stdint.h>
#include <stdio.h>
#include "kiwi_syscall.h"

int main(void) {
    static const char* const base_dirs[] = {
        "/bin",
        "/dev",
        "/mnt",
        "/home",
        "/tmp",
    };
    static const char* const shell_candidates[] = {
        "/bin/sh",
        "/bin/shell",
        "/shell",
    };

    for (uint32_t i = 0; i < (sizeof(base_dirs) / sizeof(base_dirs[0])); i++) {
        kiwi_stat_t st;

        if (sys_mkdir(base_dirs[i], 0755u) == 0) {
            continue;
        }

        if (sys_stat(base_dirs[i], &st) == 0 && st.type == KIWI_VNODE_DIR) {
            continue;
        }

        puts("init: failed to create base directory");
        puts(base_dirs[i]);
    }

    for (uint32_t i = 0; i < (sizeof(shell_candidates) / sizeof(shell_candidates[0])); i++) {
        kiwi_stat_t st;

        if (sys_stat(shell_candidates[i], &st) == 0 && st.type == KIWI_VNODE_FILE) {
            sys_exec(shell_candidates[i]);
        }
    }

    puts("init: failed to start shell");
    return 1;
}
