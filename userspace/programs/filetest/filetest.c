#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kiwi_syscall.h"

static int fail(const char* msg, int code) {
    puts(msg);
    return code;
}

int main(void) {
    static const char expected_magic[] = "KXE";
    static const char heap_msg[] = "filetest: heap write ok\n";
    kiwi_stat_t st;
    char buf[64];
    char hello_path[256];
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

    if (kiwi_resolve_program_path("hello", hello_path, sizeof(hello_path)) != 0) {
        return fail("filetest: FAIL resolve hello", 2);
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(hello_path, &st) != 0) {
        return fail("filetest: FAIL sys_stat", 3);
    }
    if (st.type != KIWI_VNODE_FILE) {
        return fail("filetest: FAIL stat type", 4);
    }
    if (st.size < 4u) {
        return fail("filetest: FAIL stat size", 5);
    }

    fd = sys_open(hello_path, KIWI_O_RDONLY);
    if (fd < 0) {
        return fail("filetest: FAIL sys_open", 6);
    }

    memset(buf, 0, sizeof(buf));
    n = sys_read((int)fd, buf, 4);
    if (n != 4) {
        sys_close((int)fd);
        return fail("filetest: FAIL sys_read", 7);
    }
    if (memcmp(buf, expected_magic, 3u) != 0 || buf[3] != '\0') {
        sys_close((int)fd);
        return fail("filetest: FAIL file contents", 8);
    }

    if (sys_seek((int)fd, 0, KIWI_SEEK_SET) != 0) {
        sys_close((int)fd);
        return fail("filetest: FAIL sys_seek", 9);
    }

    memset(buf, 0, sizeof(buf));
    n = sys_read((int)fd, buf, 3);
    if (n != 3 || memcmp(buf, expected_magic, 3u) != 0) {
        sys_close((int)fd);
        return fail("filetest: FAIL seek/readback", 10);
    }

    if (sys_close((int)fd) != 0) {
        return fail("filetest: FAIL sys_close", 11);
    }

    cur_brk = sys_brk(0);
    if (cur_brk <= 0) {
        return fail("filetest: FAIL sys_brk query", 12);
    }

    new_brk = sys_brk((uint64_t)cur_brk + 4096u);
    if (new_brk != cur_brk + 4096) {
        return fail("filetest: FAIL sys_brk grow", 13);
    }

    heap = (char*)(uintptr_t)cur_brk;
    memcpy(heap, heap_msg, sizeof(heap_msg) - 1u);
    if (sys_write(1, heap, sizeof(heap_msg) - 1u) != (int64_t)(sizeof(heap_msg) - 1u)) {
        return fail("filetest: FAIL heap sys_write", 14);
    }

    puts("filetest: PASS getpid/stat/open/read/seek/close/brk");
    puts("filetest: exec'ing hello");
    sys_exec(hello_path);
    return fail("filetest: FAIL sys_exec returned", 15);
}
