#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kiwi_syscall.h"

static int fail(const char* msg, int code) {
    puts(msg);
    return code;
}

int main(void) {
    static const char expected_file[] = "Hello from KiFS v0.1 on KiwiOS!\n";
    static const char heap_msg[] = "filetest: heap write ok\n";
    kiwi_stat_t st;
    char buf[64];
    int64_t pid = 0;
    int64_t fd = -1;
    int64_t n = 0;
    int64_t cur_brk = 0;
    int64_t new_brk = 0;
    char* heap = 0;

    puts("filetest: starting");

    pid = sys_getpid();
    if (pid == -1) {
        return fail("filetest: FAIL sys_getpid", 1);
    }
    if (pid == 0) {
        return fail("filetest: FAIL sys_getpid returned 0", 1);
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat("/hello.txt", &st) != 0) {
        return fail("filetest: FAIL sys_stat", 2);
    }
    if (st.type != KIWI_VNODE_FILE) {
        return fail("filetest: FAIL stat type", 3);
    }
    if (st.size != (uint64_t)strlen(expected_file)) {
        return fail("filetest: FAIL stat size", 4);
    }

    fd = sys_open("/hello.txt", KIWI_O_RDONLY);
    if (fd < 0) {
        return fail("filetest: FAIL sys_open", 5);
    }

    memset(buf, 0, sizeof(buf));
    n = sys_read((int)fd, buf, sizeof(buf) - 1u);
    if (n != (int64_t)strlen(expected_file)) {
        sys_close((int)fd);
        return fail("filetest: FAIL sys_read", 6);
    }
    if (memcmp(buf, expected_file, (size_t)n) != 0) {
        sys_close((int)fd);
        return fail("filetest: FAIL file contents", 7);
    }

    if (sys_seek((int)fd, 0, KIWI_SEEK_SET) != 0) {
        sys_close((int)fd);
        return fail("filetest: FAIL sys_seek", 8);
    }

    memset(buf, 0, sizeof(buf));
    n = sys_read((int)fd, buf, 5);
    if (n != 5 || memcmp(buf, "Hello", 5) != 0) {
        sys_close((int)fd);
        return fail("filetest: FAIL seek/readback", 9);
    }

    if (sys_close((int)fd) != 0) {
        return fail("filetest: FAIL sys_close", 10);
    }

    cur_brk = sys_brk(0);
    if (cur_brk <= 0) {
        return fail("filetest: FAIL sys_brk query", 11);
    }

    new_brk = sys_brk((uint64_t)cur_brk + 4096u);
    if (new_brk != cur_brk + 4096) {
        return fail("filetest: FAIL sys_brk grow", 12);
    }

    heap = (char*)(uintptr_t)cur_brk;
    memcpy(heap, heap_msg, sizeof(heap_msg) - 1u);
    if (sys_write(1, heap, sizeof(heap_msg) - 1u) != (int64_t)(sizeof(heap_msg) - 1u)) {
        return fail("filetest: FAIL heap sys_write", 13);
    }

    puts("filetest: PASS getpid/stat/open/read/seek/close/brk");
    puts("filetest: exec'ing /hello");
    sys_exec("/hello");
    return fail("filetest: FAIL sys_exec returned", 14);
}
