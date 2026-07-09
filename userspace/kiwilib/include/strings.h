#ifndef KIWILIB_STRINGS_H
#define KIWILIB_STRINGS_H

#include <stddef.h>

int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
char* strcasestr(const char* haystack, const char* needle);
void bzero(void* s, size_t n);
void explicit_bzero(void* s, size_t n);
void bcopy(const void* src, void* dst, size_t n);
int bcmp(const void* a, const void* b, size_t n);
char* index(const char* s, int c);
char* rindex(const char* s, int c);

#endif // KIWILIB_STRINGS_H
