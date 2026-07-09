#include <stddef.h>
#include "ctype.h"
#include "errno.h"
#include "limits.h"
#include "stdlib.h"
#include "string.h"
#include "wchar.h"
#include "wctype.h"

size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps) {
    (void)ps;
    if (!s) {
        return 0;
    }
    if (n == 0) {
        return (size_t)-2;
    }
    if (*s == '\0') {
        if (pwc) {
            *pwc = 0;
        }
        return 0;
    }
    if ((unsigned char)*s >= 0x80) {
        return (size_t)-1;
    }
    if (pwc) {
        *pwc = (wchar_t)(unsigned char)*s;
    }
    return 1;
}

size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps) {
    (void)ps;
    if (!s) {
        return 1;
    }
    if ((unsigned long)wc > 0x7f) {
        return (size_t)-1;
    }
    *s = (char)wc;
    return 1;
}

int mbsinit(const mbstate_t* ps) {
    return !ps || ps->opaque == 0;
}

int mbtowc(wchar_t* pwc, const char* s, size_t n) {
    size_t ret = mbrtowc(pwc, s, n, NULL);
    return ret > (size_t)INT_MAX ? -1 : (int)ret;
}

int wctomb(char* s, wchar_t wc) {
    size_t ret = wcrtomb(s, wc, NULL);
    return ret == (size_t)-1 ? -1 : (int)ret;
}

size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps) {
    const char* s;
    size_t out = 0;

    (void)ps;
    if (!src || !*src) {
        errno = EINVAL;
        return (size_t)-1;
    }
    s = *src;
    while (nms > 0 && *s) {
        unsigned char ch = (unsigned char)*s;
        if (ch >= 0x80) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (dst) {
            if (out >= len) {
                *src = s;
                return out;
            }
            dst[out] = (wchar_t)ch;
        }
        out++;
        s++;
        nms--;
    }
    if (dst && out < len) {
        dst[out] = 0;
    }
    *src = (nms > 0 && *s == '\0') ? NULL : s;
    return out;
}

size_t mbsrtowcs(wchar_t* dst, const char** src, size_t len, mbstate_t* ps) {
    return mbsnrtowcs(dst, src, (size_t)-1, len, ps);
}

size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps) {
    const wchar_t* s;
    size_t out = 0;

    (void)ps;
    if (!src || !*src) {
        errno = EINVAL;
        return (size_t)-1;
    }
    s = *src;
    while (nwc > 0 && *s) {
        if ((unsigned long)*s > 0x7f) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (dst) {
            if (out >= len) {
                *src = s;
                return out;
            }
            dst[out] = (char)*s;
        }
        out++;
        s++;
        nwc--;
    }
    if (dst && out < len) {
        dst[out] = '\0';
    }
    *src = (nwc > 0 && *s == 0) ? NULL : s;
    return out;
}

size_t wcsrtombs(char* dst, const wchar_t** src, size_t len, mbstate_t* ps) {
    return wcsnrtombs(dst, src, (size_t)-1, len, ps);
}

size_t wcslen(const wchar_t* s) {
    size_t n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

size_t wcsnlen(const wchar_t* s, size_t maxlen) {
    size_t n = 0;
    while (s && n < maxlen && s[n]) {
        n++;
    }
    return n;
}

int wcscmp(const wchar_t* a, const wchar_t* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (*a > *b) - (*a < *b);
}

int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) {
            return (a[i] > b[i]) - (a[i] < b[i]);
        }
    }
    return 0;
}

int wcscoll(const wchar_t* a, const wchar_t* b) {
    return wcscmp(a, b);
}

size_t wcsxfrm(wchar_t* dst, const wchar_t* src, size_t n) {
    size_t len = wcslen(src);

    if (dst && n != 0) {
        size_t copy = len >= n ? n - 1u : len;
        wmemcpy(dst, src, copy);
        dst[copy] = 0;
    }
    return len;
}

wchar_t* wcscpy(wchar_t* dst, const wchar_t* src) {
    wchar_t* out = dst;
    while ((*out++ = *src++) != 0) {
    }
    return dst;
}

wchar_t* wcsncpy(wchar_t* dst, const wchar_t* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

wchar_t* wcscat(wchar_t* dst, const wchar_t* src) {
    wcscpy(dst + wcslen(dst), src);
    return dst;
}

wchar_t* wcsncat(wchar_t* dst, const wchar_t* src, size_t n) {
    wchar_t* out = dst + wcslen(dst);
    size_t i = 0;
    while (i < n && src[i]) {
        out[i] = src[i];
        i++;
    }
    out[i] = 0;
    return dst;
}

wchar_t* wcsdup(const wchar_t* s) {
    size_t len;
    wchar_t* out;

    if (!s) {
        return NULL;
    }
    len = wcslen(s);
    out = (wchar_t*)malloc((len + 1u) * sizeof(wchar_t));
    if (!out) {
        return NULL;
    }
    wmemcpy(out, s, len + 1u);
    return out;
}

wchar_t* wcschr(const wchar_t* s, wchar_t c) {
    while (*s) {
        if (*s == c) {
            return (wchar_t*)s;
        }
        s++;
    }
    return c == 0 ? (wchar_t*)s : NULL;
}

wchar_t* wcsrchr(const wchar_t* s, wchar_t c) {
    const wchar_t* last = NULL;
    do {
        if (*s == c) {
            last = s;
        }
    } while (*s++);
    return (wchar_t*)last;
}

wchar_t* wcsstr(const wchar_t* haystack, const wchar_t* needle) {
    size_t needle_len;

    if (!haystack || !needle) {
        return NULL;
    }
    needle_len = wcslen(needle);
    if (needle_len == 0) {
        return (wchar_t*)haystack;
    }
    while (*haystack) {
        if (wcsncmp(haystack, needle, needle_len) == 0) {
            return (wchar_t*)haystack;
        }
        haystack++;
    }
    return NULL;
}

wchar_t* wcspbrk(const wchar_t* s, const wchar_t* accept) {
    while (s && *s) {
        if (wcschr(accept, *s)) {
            return (wchar_t*)s;
        }
        s++;
    }
    return NULL;
}

size_t wcsspn(const wchar_t* s, const wchar_t* accept) {
    size_t n = 0;
    while (s && s[n] && wcschr(accept, s[n])) {
        n++;
    }
    return n;
}

size_t wcscspn(const wchar_t* s, const wchar_t* reject) {
    size_t n = 0;
    while (s && s[n] && !wcschr(reject, s[n])) {
        n++;
    }
    return n;
}

wchar_t* wmemcpy(wchar_t* dst, const wchar_t* src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    return dst;
}

wchar_t* wmemmove(wchar_t* dst, const wchar_t* src, size_t n) {
    if (dst < src) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    } else if (dst > src) {
        while (n > 0) {
            n--;
            dst[n] = src[n];
        }
    }
    return dst;
}

wchar_t* wmemset(wchar_t* s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        s[i] = c;
    }
    return s;
}

int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (a[i] > b[i]) - (a[i] < b[i]);
        }
    }
    return 0;
}

wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c) {
            return (wchar_t*)&s[i];
        }
    }
    return NULL;
}

int wcwidth(wchar_t wc) {
    if (wc == 0) {
        return 0;
    }
    if ((unsigned long)wc < 32 || wc == 127) {
        return -1;
    }
    return 1;
}

int iswspace(wint_t wc) {
    return wc <= 0x7fu && isspace((int)wc);
}

int iswprint(wint_t wc) {
    return wc <= 0x7fu && isprint((int)wc);
}

int iswcntrl(wint_t wc) {
    return wc <= 0x7fu && iscntrl((int)wc);
}

int iswalpha(wint_t wc) {
    return wc <= 0x7fu && isalpha((int)wc);
}

int iswdigit(wint_t wc) {
    return wc <= 0x7fu && isdigit((int)wc);
}

int iswalnum(wint_t wc) {
    return wc <= 0x7fu && isalnum((int)wc);
}

int iswblank(wint_t wc) {
    return wc <= 0x7fu && isblank((int)wc);
}

int iswlower(wint_t wc) {
    return wc <= 0x7fu && islower((int)wc);
}

int iswupper(wint_t wc) {
    return wc <= 0x7fu && isupper((int)wc);
}

int iswxdigit(wint_t wc) {
    return wc <= 0x7fu && isxdigit((int)wc);
}

int iswgraph(wint_t wc) {
    return wc <= 0x7fu && isgraph((int)wc);
}

int iswpunct(wint_t wc) {
    return wc <= 0x7fu && ispunct((int)wc);
}

wctype_t wctype(const char* property) {
    static const char* names[] = {
        "alnum", "alpha", "blank", "cntrl", "digit", "graph",
        "lower", "print", "punct", "space", "upper", "xdigit",
    };

    if (!property) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(property, names[i]) == 0) {
            return (wctype_t)(i + 1u);
        }
    }
    return 0;
}

int iswctype(wint_t wc, wctype_t desc) {
    switch (desc) {
        case 1: return iswalnum(wc);
        case 2: return iswalpha(wc);
        case 3: return iswblank(wc);
        case 4: return iswcntrl(wc);
        case 5: return iswdigit(wc);
        case 6: return iswgraph(wc);
        case 7: return iswlower(wc);
        case 8: return iswprint(wc);
        case 9: return iswpunct(wc);
        case 10: return iswspace(wc);
        case 11: return iswupper(wc);
        case 12: return iswxdigit(wc);
        default: return 0;
    }
}

wctrans_t wctrans(const char* property) {
    if (!property) {
        return 0;
    }
    if (strcmp(property, "tolower") == 0) {
        return 1;
    }
    if (strcmp(property, "toupper") == 0) {
        return 2;
    }
    return 0;
}

wint_t towctrans(wint_t wc, wctrans_t desc) {
    if (desc == 1) {
        return towlower(wc);
    }
    if (desc == 2) {
        return towupper(wc);
    }
    return wc;
}

wint_t towlower(wint_t wc) {
    return wc <= 0x7fu ? (wint_t)tolower((int)wc) : wc;
}

wint_t towupper(wint_t wc) {
    return wc <= 0x7fu ? (wint_t)toupper((int)wc) : wc;
}
