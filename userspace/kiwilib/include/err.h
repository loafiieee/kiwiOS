#ifndef KIWILIB_ERR_H
#define KIWILIB_ERR_H

#include <stdarg.h>

void warn(const char* fmt, ...);
void vwarn(const char* fmt, va_list ap);
void warnx(const char* fmt, ...);
void vwarnx(const char* fmt, va_list ap);
void err(int eval, const char* fmt, ...) __attribute__((noreturn));
void verr(int eval, const char* fmt, va_list ap) __attribute__((noreturn));
void errx(int eval, const char* fmt, ...) __attribute__((noreturn));
void verrx(int eval, const char* fmt, va_list ap) __attribute__((noreturn));

#endif // KIWILIB_ERR_H
