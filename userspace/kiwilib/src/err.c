#include <stdarg.h>
#include "err.h"
#include "errno.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

static void print_prefix(void) {
    const char* name = getenv("PROGRAM");
    if (name && *name) {
        fprintf(stderr, "%s: ", name);
    }
}

void vwarnx(const char* fmt, va_list ap) {
    print_prefix();
    if (fmt) {
        vfprintf(stderr, fmt, ap);
    }
    fputc('\n', stderr);
}

void warnx(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

void vwarn(const char* fmt, va_list ap) {
    int saved_errno = errno;

    print_prefix();
    if (fmt) {
        vfprintf(stderr, fmt, ap);
        fputs(": ", stderr);
    }
    fputs(strerror(saved_errno), stderr);
    fputc('\n', stderr);
}

void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
}

void verrx(int eval, const char* fmt, va_list ap) {
    vwarnx(fmt, ap);
    exit(eval);
}

void errx(int eval, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verrx(eval, fmt, ap);
}

void verr(int eval, const char* fmt, va_list ap) {
    vwarn(fmt, ap);
    exit(eval);
}

void err(int eval, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verr(eval, fmt, ap);
}
