#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

static void out(const char* s) {
    if (s) {
        sys_write(1, s, (uint64_t)strlen(s));
    }
}

static int cat_one(const char* path, char* last_ch, int* saw) {
    char buf[512];
    int64_t fd = sys_open(path, KIWI_O_RDONLY);

    if (fd < 0) {
        out("cat: ");
        out(path);
        out(": cannot open\n");
        return 1;
    }

    for (;;) {
        int64_t n = sys_read((int)fd, buf, sizeof(buf));
        if (n < 0) {
            out("cat: ");
            out(path);
            out(": read error\n");
            (void)sys_close((int)fd);
            return 1;
        }
        if (n == 0) {
            break;
        }
        *saw = 1;
        *last_ch = buf[n - 1];
        (void)sys_write(1, buf, (uint64_t)n);
    }

    (void)sys_close((int)fd);
    return 0;
}

int main(int argc, char** argv) {
    int rc = 0;
    int saw = 0;
    char last_ch = '\0';

    if (argc < 2) {
        out("usage: cat <path>...\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (cat_one(argv[i], &last_ch, &saw) != 0) {
            rc = 1;
        }
    }

    /* Keep the shell prompt on a fresh line, matching the old built-in. */
    if (!saw || last_ch != '\n') {
        out("\n");
    }
    return rc;
}
