#ifndef KIWILIB_STDDEF_H
#define KIWILIB_STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef __cplusplus
#ifndef _WCHAR_T
#define _WCHAR_T
typedef __WCHAR_TYPE__ wchar_t;
#endif
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

typedef struct {
    long long __ll;
    long double __ld;
} max_align_t;

#endif // KIWILIB_STDDEF_H
