#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static void out_u64(uint64_t v) {
    char buf[24];
    size_t i = sizeof(buf);

    buf[--i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    }
    while (v != 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    out(&buf[i]);
}

int main(int argc, char** argv) {
    int64_t count;

    (void)argv;
    if (argc > 1) {
        out("usage: rescan\n");
        return 1;
    }

    count = sys_dev_rescan();
    if (count < 0) {
        out("rescan: failed\n");
        return 1;
    }
    out("rescan: found ");
    out_u64((uint64_t)count);
    out(" new disk(s)\n");
    return 0;
}
