#include <stdint.h>
#include "kiwi_syscall.h"

static void write_str(const char* s) {
    uint64_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    sys_write(1, s, len);
}

int main(void) {
    char buf[64];
    int64_t n = 0;

    write_str("readtest: waiting for stdin\n");
    n = sys_read(0, buf, sizeof(buf));
    if (n < 0) {
        write_str("readtest: FAIL sys_read\n");
        return 1;
    }

    write_str("readtest: got ");
    if (n > 0) {
        sys_write(1, buf, (uint64_t)n);
        if (buf[n - 1] != '\n') {
            write_str("\n");
        }
    } else {
        write_str("EOF\n");
    }

    return 0;
}
