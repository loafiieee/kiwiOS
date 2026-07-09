#include "stdio.h"
#include "string.h"
#include "time.h"

static void usage(void) {
    fputs("usage: date [-u] [+FORMAT]\n", stderr);
}

int main(int argc, char** argv) {
    const char* format = "%a %b %e %T UTC %Y";
    time_t now;
    struct tm* tm;
    char buf[128];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            continue;
        }
        if (argv[i][0] == '+') {
            format = argv[i] + 1;
            continue;
        }
        usage();
        return 1;
    }

    now = time(NULL);
    if (now == (time_t)-1) {
        fputs("date: time unavailable\n", stderr);
        return 1;
    }

    tm = localtime(&now);
    if (!tm || strftime(buf, sizeof(buf), format, tm) == 0) {
        fputs("date: format failed\n", stderr);
        return 1;
    }

    puts(buf);
    return 0;
}