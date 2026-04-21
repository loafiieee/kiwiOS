#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

int putchar(int ch) {
    char c = (char)ch;
    if (sys_write(1, &c, 1) != 1) {
        return -1;
    }
    return (unsigned char)c;
}

int puts(const char* s) {
    size_t len = 0;

    if (!s) {
        return -1;
    }

    len = strlen(s);
    if (len != 0 && sys_write(1, s, (uint64_t)len) != (int64_t)len) {
        return -1;
    }

    if (sys_write(1, "\n", 1) != 1) {
        return -1;
    }

    return (int)(len + 1);
}
