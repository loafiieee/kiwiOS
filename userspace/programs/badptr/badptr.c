#include <stdint.h>
#include <stdio.h>
#include "kiwi_syscall.h"

int main(void) {
    int64_t rc = 0;

    puts("badptr: testing syscall pointer validation");

    rc = sys_write(1, (const void*)0x0ull, 16);
    if (rc != -1) {
        puts("badptr: FAIL null pointer was accepted");
        return 1;
    }

    rc = sys_write(1, (const void*)0x0000000000600000ull, 16);
    if (rc != -1) {
        puts("badptr: FAIL unmapped user pointer was accepted");
        return 2;
    }

    rc = sys_write(1, (const void*)0xFFFFFFFF80000000ull, 16);
    if (rc != -1) {
        puts("badptr: FAIL kernel pointer was accepted");
        return 3;
    }

    puts("badptr: PASS invalid pointers rejected");
    return 0;
}
