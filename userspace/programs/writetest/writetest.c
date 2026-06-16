#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kiwi_syscall.h"

static int fail(const char* msg, int code) {
    puts(msg);
    return code;
}

int main(void) {
    static const char dir_path[] = "/tmp";
    static const char file_path[] = "/tmp/writetest.txt";
    static const char file_body[] = "KiFS write path OK\n";
    kiwi_stat_t st;
    char buf[64];
    int64_t fd = -1;
    int64_t n = 0;

    puts("writetest: starting");

    memset(&st, 0, sizeof(st));
    if (sys_mkdir(dir_path, 0755u) < 0) {
        if (sys_stat(dir_path, &st) != 0 || st.type != KIWI_VNODE_DIR) {
            return fail("writetest: FAIL sys_mkdir", 1);
        }
    }

    (void)sys_unlink(file_path);

    fd = sys_open(file_path, KIWI_O_WRONLY | KIWI_O_CREAT | KIWI_O_TRUNC);
    if (fd < 0) {
        return fail("writetest: FAIL create/open", 2);
    }

    n = sys_write((int)fd, file_body, sizeof(file_body) - 1u);
    if (n != (int64_t)(sizeof(file_body) - 1u)) {
        sys_close((int)fd);
        return fail("writetest: FAIL sys_write", 3);
    }

    if (sys_close((int)fd) != 0) {
        return fail("writetest: FAIL sys_close", 4);
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(file_path, &st) != 0 || st.size != (uint64_t)(sizeof(file_body) - 1u)) {
        return fail("writetest: FAIL sys_stat after write", 5);
    }

    fd = sys_open(file_path, KIWI_O_RDONLY);
    if (fd < 0) {
        return fail("writetest: FAIL reopen", 6);
    }

    memset(buf, 0, sizeof(buf));
    n = sys_read((int)fd, buf, sizeof(buf) - 1u);
    if (n != (int64_t)(sizeof(file_body) - 1u) ||
        memcmp(buf, file_body, sizeof(file_body) - 1u) != 0) {
        sys_close((int)fd);
        return fail("writetest: FAIL readback", 7);
    }

    if (sys_close((int)fd) != 0) {
        return fail("writetest: FAIL close after read", 8);
    }

    if (sys_unlink(file_path) != 0) {
        return fail("writetest: FAIL sys_unlink", 9);
    }

    if (sys_stat(file_path, &st) == 0) {
        return fail("writetest: FAIL unlink verification", 10);
    }

    puts("writetest: PASS mkdir/open/write/read/unlink");
    return 0;
}
