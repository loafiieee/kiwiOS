#ifndef KIWILIB_WCHAR_H
#define KIWILIB_WCHAR_H

#include <stddef.h>
#include "stdio.h"

#ifndef __cplusplus
#ifndef _WCHAR_T
#define _WCHAR_T
typedef __WCHAR_TYPE__ wchar_t;
#endif
#endif

typedef unsigned int wint_t;
typedef struct {
    unsigned int opaque;
} mbstate_t;

#define WEOF ((wint_t)-1)

size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps);
size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps);
int mbsinit(const mbstate_t* ps);
int mbtowc(wchar_t* pwc, const char* s, size_t n);
int wctomb(char* s, wchar_t wc);
size_t mbsrtowcs(wchar_t* dst, const char** src, size_t len, mbstate_t* ps);
size_t wcsrtombs(char* dst, const wchar_t** src, size_t len, mbstate_t* ps);
size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps);
size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps);
size_t wcslen(const wchar_t* s);
size_t wcsnlen(const wchar_t* s, size_t maxlen);
int wcscmp(const wchar_t* a, const wchar_t* b);
int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n);
int wcscoll(const wchar_t* a, const wchar_t* b);
size_t wcsxfrm(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wcscpy(wchar_t* dst, const wchar_t* src);
wchar_t* wcsncpy(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wcscat(wchar_t* dst, const wchar_t* src);
wchar_t* wcsncat(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wcsdup(const wchar_t* s);
wchar_t* wcschr(const wchar_t* s, wchar_t c);
wchar_t* wcsrchr(const wchar_t* s, wchar_t c);
wchar_t* wcsstr(const wchar_t* haystack, const wchar_t* needle);
wchar_t* wcspbrk(const wchar_t* s, const wchar_t* accept);
size_t wcsspn(const wchar_t* s, const wchar_t* accept);
size_t wcscspn(const wchar_t* s, const wchar_t* reject);
wchar_t* wmemcpy(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wmemmove(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wmemset(wchar_t* s, wchar_t c, size_t n);
int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n);
int wcwidth(wchar_t wc);

#endif // KIWILIB_WCHAR_H
