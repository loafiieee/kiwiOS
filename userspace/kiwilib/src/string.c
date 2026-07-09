#include <stddef.h>
#include <stdlib.h>
#include "ctype.h"
#include "string.h"

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void* mempcpy(void* dst, const void* src, size_t n) {
    memcpy(dst, src, n);
    return (unsigned char*)dst + n;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char needle = (unsigned char)c;

    for (size_t i = 0; i < n; i++) {
        if (p[i] == needle) {
            return (void*)(p + i);
        }
    }
    return NULL;
}

void* memrchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char needle = (unsigned char)c;

    while (n > 0) {
        n--;
        if (p[n] == needle) {
            return (void*)(p + n);
        }
    }
    return NULL;
}

void* rawmemchr(const void* s, int c) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char needle = (unsigned char)c;

    while (*p != needle) {
        p++;
    }
    return (void*)p;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) {
        n++;
    }
    return n;
}

size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t len = strlen(src);
    if (size != 0) {
        size_t copy = len >= size ? size - 1u : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

size_t strlcat(char* dst, const char* src, size_t size) {
    size_t dst_len = strnlen(dst, size);
    size_t src_len = strlen(src);

    if (dst_len == size) {
        return size + src_len;
    }
    if (size > dst_len + 1u) {
        size_t copy = src_len;
        if (copy > size - dst_len - 1u) {
            copy = size - dst_len - 1u;
        }
        memcpy(dst + dst_len, src, copy);
        dst[dst_len + copy] = '\0';
    }
    return dst_len + src_len;
}

char* strcpy(char* dst, const char* src) {
    char* out = dst;
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

char* stpcpy(char* dst, const char* src) {
    while ((*dst = *src) != '\0') {
        dst++;
        src++;
    }
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char* stpncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;

    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    if (i < n) {
        char* end = dst + i;
        for (; i < n; i++) {
            dst[i] = '\0';
        }
        return end;
    }
    return dst + n;
}

char* strcat(char* dst, const char* src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* out = dst;
    dst += strlen(dst);
    while (n > 0 && *src) {
        *dst++ = *src++;
        n--;
    }
    *dst = '\0';
    return out;
}

int strcmp(const char* a, const char* b) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    while (*pa && *pa == *pb) {
        pa++;
        pb++;
    }
    return (int)*pa - (int)*pb;
}

int strncmp(const char* a, const char* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i] || pa[i] == '\0') {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

int strcoll(const char* a, const char* b) {
    return strcmp(a, b);
}

size_t strxfrm(char* dst, const char* src, size_t n) {
    size_t len = strlen(src);

    if (dst && n != 0) {
        size_t copy = len >= n ? n - 1u : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

int strverscmp(const char* a, const char* b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long va = 0;
            unsigned long vb = 0;
            while (*a == '0') a++;
            while (*b == '0') b++;
            while (isdigit((unsigned char)*a)) {
                va = va * 10ul + (unsigned long)(*a - '0');
                a++;
            }
            while (isdigit((unsigned char)*b)) {
                vb = vb * 10ul + (unsigned long)(*b - '0');
                b++;
            }
            if (va != vb) {
                return va < vb ? -1 : 1;
            }
            continue;
        }
        if (*a != *b) {
            return (unsigned char)*a - (unsigned char)*b;
        }
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char* strchr(const char* s, int c) {
    char needle = (char)c;
    while (*s) {
        if (*s == needle) {
            return (char*)s;
        }
        s++;
    }
    return needle == '\0' ? (char*)s : NULL;
}

char* strchrnul(const char* s, int c) {
    char* found = strchr(s, c);
    return found ? found : (char*)s + strlen(s);
}

char* strrchr(const char* s, int c) {
    char needle = (char)c;
    const char* last = NULL;
    do {
        if (*s == needle) {
            last = s;
        }
    } while (*s++);
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return (char*)haystack;
    }

    for (; *haystack; haystack++) {
        if (*haystack == *needle && strncmp(haystack, needle, needle_len) == 0) {
            return (char*)haystack;
        }
    }

    return NULL;
}

char* strpbrk(const char* s, const char* accept) {
    while (*s) {
        for (const char* a = accept; *a; a++) {
            if (*s == *a) {
                return (char*)s;
            }
        }
        s++;
    }
    return NULL;
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;

    while (s[n]) {
        int found = 0;
        for (const char* a = accept; *a; a++) {
            if (s[n] == *a) {
                found = 1;
                break;
            }
        }
        if (!found) {
            break;
        }
        n++;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;

    while (s[n]) {
        for (const char* r = reject; *r; r++) {
            if (s[n] == *r) {
                return n;
            }
        }
        n++;
    }
    return n;
}

char* strtok_r(char* s, const char* delim, char** saveptr) {
    char* token = NULL;

    if (!saveptr || !delim) {
        return NULL;
    }

    if (!s) {
        s = *saveptr;
    }
    if (!s) {
        return NULL;
    }

    s += strspn(s, delim);
    if (*s == '\0') {
        *saveptr = s;
        return NULL;
    }

    token = s;
    s += strcspn(s, delim);
    if (*s != '\0') {
        *s++ = '\0';
    }
    *saveptr = s;
    return token;
}

char* strtok(char* s, const char* delim) {
    static char* save;
    return strtok_r(s, delim, &save);
}

char* strsep(char** stringp, const char* delim) {
    char* start;
    char* p;

    if (!stringp || !*stringp || !delim) {
        return NULL;
    }
    start = *stringp;
    p = strpbrk(start, delim);
    if (p) {
        *p = '\0';
        *stringp = p + 1;
    } else {
        *stringp = NULL;
    }
    return start;
}

char* strdup(const char* s) {
    size_t len = strlen(s) + 1u;
    char* out = (char*)malloc(len);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len);
    return out;
}

char* strndup(const char* s, size_t n) {
    size_t len = strnlen(s, n);
    char* out = (char*)malloc(len + 1u);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

int strcasecmp(const char* a, const char* b) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;

    while (*pa && tolower(*pa) == tolower(*pb)) {
        pa++;
        pb++;
    }
    return tolower(*pa) - tolower(*pb);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;

    for (size_t i = 0; i < n; i++) {
        int ca = tolower(pa[i]);
        int cb = tolower(pb[i]);
        if (ca != cb || ca == '\0') {
            return ca - cb;
        }
    }
    return 0;
}

char* strcasestr(const char* haystack, const char* needle) {
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return (char*)haystack;
    }

    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return (char*)haystack;
        }
    }
    return NULL;
}

void bzero(void* s, size_t n) {
    (void)memset(s, 0, n);
}

void explicit_bzero(void* s, size_t n) {
    volatile unsigned char* p = (volatile unsigned char*)s;
    while (n-- > 0) {
        *p++ = 0;
    }
}

void bcopy(const void* src, void* dst, size_t n) {
    (void)memmove(dst, src, n);
}

int bcmp(const void* a, const void* b, size_t n) {
    return memcmp(a, b, n);
}

char* index(const char* s, int c) {
    return strchr(s, c);
}

char* rindex(const char* s, int c) {
    return strrchr(s, c);
}

char* strerror(int errnum) {
    switch (errnum) {
        case 0: return "Success";
        case 1: return "Operation not permitted";
        case 2: return "No such file or directory";
        case 3: return "No such process";
        case 4: return "Interrupted system call";
        case 5: return "I/O error";
        case 7: return "Argument list too long";
        case 8: return "Exec format error";
        case 9: return "Bad file descriptor";
        case 10: return "No child processes";
        case 11: return "Resource temporarily unavailable";
        case 12: return "Out of memory";
        case 13: return "Permission denied";
        case 14: return "Bad address";
        case 16: return "Device or resource busy";
        case 17: return "File exists";
        case 19: return "No such device";
        case 20: return "Not a directory";
        case 21: return "Is a directory";
        case 22: return "Invalid argument";
        case 23: return "File table overflow";
        case 24: return "Too many open files";
        case 25: return "Not a tty";
        case 27: return "File too large";
        case 28: return "No space left on device";
        case 30: return "Read-only filesystem";
        case 32: return "Broken pipe";
        case 33: return "Numerical argument out of domain";
        case 36: return "File name too long";
        case 38: return "Function not implemented";
        case 39: return "Directory not empty";
        case 40: return "Too many symbolic links";
        case 75: return "Value too large for defined data type";
        case 95: return "Operation not supported";
        default: return "Unknown error";
    }
}

int strerror_r(int errnum, char* buf, size_t buflen) {
    const char* msg = strerror(errnum);
    size_t len = strlen(msg);

    if (!buf || buflen == 0) {
        return -1;
    }
    if (len >= buflen) {
        len = buflen - 1u;
    }
    memcpy(buf, msg, len);
    buf[len] = '\0';
    return 0;
}

char* strsignal(int signum) {
    switch (signum) {
        case 1: return "Hangup";
        case 2: return "Interrupt";
        case 3: return "Quit";
        case 6: return "Aborted";
        case 9: return "Killed";
        case 11: return "Segmentation fault";
        case 13: return "Broken pipe";
        case 14: return "Alarm clock";
        case 15: return "Terminated";
        case 17: return "Child exited";
        case 20: return "Stopped";
        case 28: return "Window changed";
        default: return "Unknown signal";
    }
}
