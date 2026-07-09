#ifndef KIWILIB_STRING_H
#define KIWILIB_STRING_H

#include <stddef.h>

void* memcpy(void* dst, const void* src, size_t n);
void* mempcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);
void* memrchr(const void* s, int c, size_t n);
void* rawmemchr(const void* s, int c);
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t maxlen);
size_t strlcpy(char* dst, const char* src, size_t size);
size_t strlcat(char* dst, const char* src, size_t size);
char* strcpy(char* dst, const char* src);
char* stpcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* stpncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strncat(char* dst, const char* src, size_t n);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
int strcoll(const char* a, const char* b);
size_t strxfrm(char* dst, const char* src, size_t n);
int strverscmp(const char* a, const char* b);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
char* strcasestr(const char* haystack, const char* needle);
char* strchr(const char* s, int c);
char* strchrnul(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strpbrk(const char* s, const char* accept);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strtok(char* s, const char* delim);
char* strtok_r(char* s, const char* delim, char** saveptr);
char* strsep(char** stringp, const char* delim);
char* strdup(const char* s);
char* strndup(const char* s, size_t n);
char* strerror(int errnum);
int strerror_r(int errnum, char* buf, size_t buflen);
char* strsignal(int signum);

#endif // KIWILIB_STRING_H
