#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

int main(void) {
    char buf[256];

    if (sys_getcwd(buf, sizeof(buf)) < 0) {
        out("pwd: failed\n");
        return 1;
    }
    out(buf);
    out("\n");
    return 0;
}
