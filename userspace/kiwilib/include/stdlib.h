#ifndef KIWILIB_STDLIB_H
#define KIWILIB_STDLIB_H

#include <stddef.h>

typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long quot;
    long long rem;
} lldiv_t;

void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void* reallocarray(void* ptr, size_t count, size_t size);
void free(void* ptr);

int atoi(const char* s);
long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);
long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);
int abs(int value);
long labs(long value);
long long llabs(long long value);
div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);
void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
char* getenv(const char* name);
char* secure_getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int putenv(char* string);
int clearenv(void);
int getsubopt(char** optionp, char* const* tokens, char** valuep);
const char* getprogname(void);
void setprogname(const char* name);
char* realpath(const char* path, char* resolved_path);
char* mktemp(char* template);
int mkstemp(char* template);
char* mkdtemp(char* template);
int system(const char* command);
int atexit(void (*function)(void));
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

extern char** environ;

#endif // KIWILIB_STDLIB_H
