#include <stdio.h>

static void write_str(const char* s) {
    while (s && *s) {
        putchar(*s++);
    }
}

static void write_dec(int value) {
    char buf[16];
    int i = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    if (value < 0) {
        putchar('-');
        value = -value;
    }

    while (value != 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        putchar(buf[--i]);
    }
}

int main(int argc, char** argv) {
    puts("argtest: starting");
    write_str("argtest: argc=");
    write_dec(argc);
    putchar('\n');

    for (int i = 0; i < argc; i++) {
        write_str("argtest: argv[");
        write_dec(i);
        write_str("]=");
        write_str(argv[i] ? argv[i] : "(null)");
        putchar('\n');
    }

    if (argc >= 3) {
        puts("argtest: PASS argv");
        return 0;
    }

    puts("argtest: usage: argtest one two");
    return 1;
}
