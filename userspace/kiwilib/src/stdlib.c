#include <stdint.h>
#include "ctype.h"
#include "errno.h"
#include "fcntl.h"
#include "inttypes.h"
#include "kiwi_syscall.h"
#include "limits.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "unistd.h"

typedef struct heap_block {
    size_t size;
    uint8_t free;
    uint8_t reserved[7];
    struct heap_block* next;
} heap_block_t;

#define MALLOC_ALIGN 16u
#define MALLOC_MIN_SPLIT 32u

static heap_block_t* g_heap_head;
char** environ;
static void (*g_atexit_handlers[16])(void);
static size_t g_atexit_count;
static unsigned long g_temp_counter;
static char g_progname[64] = "kiwi";

typedef struct {
    char* entry;
    int owned;
} env_entry_t;

static env_entry_t g_extra_env[32];

static size_t align_size(size_t size) {
    return (size + (MALLOC_ALIGN - 1u)) & ~(MALLOC_ALIGN - 1u);
}

static heap_block_t* ptr_to_block(void* ptr) {
    return ((heap_block_t*)ptr) - 1;
}

static void split_block(heap_block_t* block, size_t size) {
    heap_block_t* next = NULL;

    if (!block || block->size < size + sizeof(heap_block_t) + MALLOC_MIN_SPLIT) {
        return;
    }

    next = (heap_block_t*)((uint8_t*)(block + 1) + size);
    next->size = block->size - size - sizeof(heap_block_t);
    next->free = 1;
    next->next = block->next;

    block->size = size;
    block->next = next;
}

static void coalesce_blocks(void) {
    heap_block_t* cur = g_heap_head;

    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(heap_block_t) + cur->next->size;
            cur->next = cur->next->next;
            continue;
        }
        cur = cur->next;
    }
}

static heap_block_t* find_free_block(size_t size) {
    heap_block_t* cur = g_heap_head;

    while (cur) {
        if (cur->free && cur->size >= size) {
            return cur;
        }
        cur = cur->next;
    }

    return NULL;
}

static heap_block_t* append_block(size_t size) {
    heap_block_t* block = NULL;
    heap_block_t* cur = NULL;
    void* mem = sbrk((intptr_t)(sizeof(heap_block_t) + size));

    if (mem == (void*)-1) {
        errno = ENOMEM;
        return NULL;
    }

    block = (heap_block_t*)mem;
    block->size = size;
    block->free = 0;
    block->next = NULL;

    if (!g_heap_head) {
        g_heap_head = block;
        return block;
    }

    cur = g_heap_head;
    while (cur->next) {
        cur = cur->next;
    }
    cur->next = block;
    return block;
}

void* malloc(size_t size) {
    heap_block_t* block = NULL;

    if (size == 0) {
        return NULL;
    }

    size = align_size(size);
    block = find_free_block(size);
    if (block) {
        block->free = 0;
        split_block(block, size);
        return block + 1;
    }

    block = append_block(size);
    return block ? (void*)(block + 1) : NULL;
}

void free(void* ptr) {
    heap_block_t* block = NULL;

    if (!ptr) {
        return;
    }

    block = ptr_to_block(ptr);
    block->free = 1;
    coalesce_blocks();
}

void* calloc(size_t count, size_t size) {
    void* ptr = NULL;

    if (count != 0 && size > ((size_t)-1 / count)) {
        errno = ENOMEM;
        return NULL;
    }

    ptr = malloc(count * size);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    heap_block_t* block = NULL;
    void* next = NULL;

    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    size = align_size(size);
    block = ptr_to_block(ptr);
    if (block->size >= size) {
        split_block(block, size);
        return ptr;
    }

    next = malloc(size);
    if (!next) {
        return NULL;
    }

    memcpy(next, ptr, block->size);
    free(ptr);
    return next;
}

void* reallocarray(void* ptr, size_t count, size_t size) {
    if (count != 0 && size > ((size_t)-1 / count)) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(ptr, count * size);
}

int atoi(const char* s) {
    return (int)strtol(s, NULL, 10);
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return -1;
}

unsigned long long strtoull(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    const char* start = NULL;
    unsigned long long value = 0;
    int neg = 0;

    if (!s) {
        if (endptr) {
            *endptr = NULL;
        }
        return 0;
    }

    while (isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '+' || *s == '-') {
        neg = *s == '-';
        s++;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
        s++;
    } else if (base == 0) {
        base = 10;
    }

    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = (char*)nptr;
        }
        return 0;
    }

    start = s;
    while (*s) {
        int digit = digit_value((unsigned char)*s);
        if (digit < 0 || digit >= base) {
            break;
        }
        value = value * (unsigned)base + (unsigned)digit;
        s++;
    }

    if (endptr) {
        *endptr = (char*)(s == start ? nptr : s);
    }

    return neg ? (unsigned long long)(-(long long)value) : value;
}

long long strtoll(const char* nptr, char** endptr, int base) {
    return (long long)strtoull(nptr, endptr, base);
}

unsigned long strtoul(const char* nptr, char** endptr, int base) {
    return (unsigned long)strtoull(nptr, endptr, base);
}

long strtol(const char* nptr, char** endptr, int base) {
    return (long)strtoll(nptr, endptr, base);
}

intmax_t strtoimax(const char* nptr, char** endptr, int base) {
    return (intmax_t)strtoll(nptr, endptr, base);
}

uintmax_t strtoumax(const char* nptr, char** endptr, int base) {
    return (uintmax_t)strtoull(nptr, endptr, base);
}

double strtod(const char* nptr, char** endptr) {
    const char* s = nptr;
    double value = 0.0;
    double scale = 0.1;
    int neg = 0;
    int saw_digit = 0;

    if (!s) {
        if (endptr) {
            *endptr = NULL;
        }
        return 0.0;
    }

    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '+' || *s == '-') {
        neg = *s == '-';
        s++;
    }
    while (isdigit((unsigned char)*s)) {
        saw_digit = 1;
        value = value * 10.0 + (double)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            saw_digit = 1;
            value += (double)(*s - '0') * scale;
            scale *= 0.1;
            s++;
        }
    }

    if (endptr) {
        *endptr = (char*)(saw_digit ? s : nptr);
    }
    return neg ? -value : value;
}

int abs(int value) {
    return value < 0 ? -value : value;
}

long labs(long value) {
    return value < 0 ? -value : value;
}

long long llabs(long long value) {
    return value < 0 ? -value : value;
}

intmax_t imaxabs(intmax_t value) {
    return value < 0 ? -value : value;
}

div_t div(int numer, int denom) {
    div_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom) {
    imaxdiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

static void swap_bytes(uint8_t* a, uint8_t* b, size_t size) {
    while (size-- > 0) {
        uint8_t tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    uint8_t* bytes = (uint8_t*)base;

    if (!base || size == 0 || !compar || nmemb < 2) {
        return;
    }

    for (size_t i = 0; i + 1u < nmemb; i++) {
        for (size_t j = i + 1u; j < nmemb; j++) {
            uint8_t* a = bytes + i * size;
            uint8_t* b = bytes + j * size;
            if (compar(a, b) > 0) {
                swap_bytes(a, b, size);
            }
        }
    }
}

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    const uint8_t* bytes = (const uint8_t*)base;
    size_t lo = 0;
    size_t hi = nmemb;

    if (!key || !base || size == 0 || !compar) {
        return NULL;
    }

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        const void* elem = bytes + mid * size;
        int cmp = compar(key, elem);
        if (cmp == 0) {
            return (void*)elem;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }

    return NULL;
}

static int str_eq(const char* a, const char* b) {
    while (a && b && *a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return a && b && *a == '\0' && *b == '\0';
}

static int env_name_len(const char* s) {
    int len = 0;

    if (!s || !*s || *s == '=') {
        return -1;
    }
    while (s[len] && s[len] != '=') {
        len++;
    }
    return len;
}

static int env_entry_matches(const char* entry, const char* name, size_t name_len) {
    return entry && strncmp(entry, name, name_len) == 0 && entry[name_len] == '=';
}

static int env_find_extra(const char* name, size_t name_len) {
    for (size_t i = 0; i < sizeof(g_extra_env) / sizeof(g_extra_env[0]); i++) {
        if (env_entry_matches(g_extra_env[i].entry, name, name_len)) {
            return (int)i;
        }
    }
    return -1;
}

static int env_find_free_extra(void) {
    for (size_t i = 0; i < sizeof(g_extra_env) / sizeof(g_extra_env[0]); i++) {
        if (!g_extra_env[i].entry) {
            return (int)i;
        }
    }
    return -1;
}

char* getenv(const char* name) {
    size_t name_len;

    if (!name || !*name) {
        return NULL;
    }

    name_len = strlen(name);
    for (size_t i = 0; i < sizeof(g_extra_env) / sizeof(g_extra_env[0]); i++) {
        if (env_entry_matches(g_extra_env[i].entry, name, name_len)) {
            return g_extra_env[i].entry + name_len + 1u;
        }
    }
    if (environ) {
        for (char** env = environ; *env; env++) {
            if (strncmp(*env, name, name_len) == 0 && (*env)[name_len] == '=') {
                return *env + name_len + 1u;
            }
        }
    }

    if (str_eq(name, "TERM")) {
        return "xterm";
    }
    if (str_eq(name, "HOME")) {
        return "/home";
    }
    if (str_eq(name, "PATH")) {
        return "/bin:/";
    }
    if (str_eq(name, "SHELL")) {
        return "/bin/sh";
    }
    if (str_eq(name, "USER")) {
        return "root";
    }
    return NULL;
}

char* secure_getenv(const char* name) {
    return getenv(name);
}

int setenv(const char* name, const char* value, int overwrite) {
    size_t name_len;
    size_t value_len;
    char* entry;
    int slot;

    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }

    name_len = strlen(name);
    value_len = value ? strlen(value) : 0;
    slot = env_find_extra(name, name_len);
    if (slot >= 0 && !overwrite) {
        return 0;
    }
    if (slot < 0) {
        slot = env_find_free_extra();
        if (slot < 0) {
            errno = ENOMEM;
            return -1;
        }
    }

    entry = (char*)malloc(name_len + 1u + value_len + 1u);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    if (value_len != 0) {
        memcpy(entry + name_len + 1u, value, value_len);
    }
    entry[name_len + 1u + value_len] = '\0';

    if (g_extra_env[slot].owned) {
        free(g_extra_env[slot].entry);
    }
    g_extra_env[slot].entry = entry;
    g_extra_env[slot].owned = 1;
    return 0;
}

int unsetenv(const char* name) {
    size_t name_len;
    int slot;

    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }

    name_len = strlen(name);
    slot = env_find_extra(name, name_len);
    if (slot >= 0) {
        if (g_extra_env[slot].owned) {
            free(g_extra_env[slot].entry);
        }
        g_extra_env[slot].entry = NULL;
        g_extra_env[slot].owned = 0;
    }
    return 0;
}

int putenv(char* string) {
    int name_len = env_name_len(string);
    int slot;

    if (name_len <= 0 || string[name_len] != '=') {
        errno = EINVAL;
        return -1;
    }

    slot = env_find_extra(string, (size_t)name_len);
    if (slot < 0) {
        slot = env_find_free_extra();
        if (slot < 0) {
            errno = ENOMEM;
            return -1;
        }
    }
    if (g_extra_env[slot].owned) {
        free(g_extra_env[slot].entry);
    }
    g_extra_env[slot].entry = string;
    g_extra_env[slot].owned = 0;
    return 0;
}

int clearenv(void) {
    for (size_t i = 0; i < sizeof(g_extra_env) / sizeof(g_extra_env[0]); i++) {
        if (g_extra_env[i].owned) {
            free(g_extra_env[i].entry);
        }
        g_extra_env[i].entry = NULL;
        g_extra_env[i].owned = 0;
    }
    environ = NULL;
    return 0;
}

int getsubopt(char** optionp, char* const* tokens, char** valuep) {
    char* option = NULL;
    char* end = NULL;
    char* value = NULL;
    size_t name_len = 0;

    if (!optionp || !*optionp || !tokens || !valuep) {
        errno = EINVAL;
        return -1;
    }

    option = *optionp;
    end = strchr(option, ',');
    if (end) {
        *end = '\0';
        *optionp = end + 1;
    } else {
        *optionp = option + strlen(option);
    }

    value = strchr(option, '=');
    if (value) {
        *value++ = '\0';
    }
    *valuep = value;
    name_len = strlen(option);

    for (int i = 0; tokens[i]; i++) {
        if (strlen(tokens[i]) == name_len && strncmp(option, tokens[i], name_len) == 0) {
            return i;
        }
    }

    *valuep = option;
    return -1;
}

const char* getprogname(void) {
    return g_progname;
}

void setprogname(const char* name) {
    const char* base = name;
    size_t len = 0;

    if (!name || !*name) {
        return;
    }

    for (const char* p = name; *p; p++) {
        if (*p == '/' && p[1]) {
            base = p + 1;
        }
    }

    while (base[len] && len + 1u < sizeof(g_progname)) {
        g_progname[len] = base[len];
        len++;
    }
    g_progname[len] = '\0';
}

static void path_pop(char* path) {
    size_t len = strlen(path);
    while (len > 0 && path[len - 1u] != '/') {
        len--;
    }
    if (len <= 1u) {
        path[0] = '/';
        path[1] = '\0';
    } else {
        path[len - 1u] = '\0';
    }
}

static int path_append(char* out, size_t out_size, const char* comp, size_t comp_len) {
    size_t len;

    if (comp_len == 0 || (comp_len == 1u && comp[0] == '.')) {
        return 1;
    }
    if (comp_len == 2u && comp[0] == '.' && comp[1] == '.') {
        path_pop(out);
        return 1;
    }

    len = strlen(out);
    if (len == 1u && out[0] == '/') {
        if (1u + comp_len + 1u > out_size) {
            return 0;
        }
        memcpy(out + 1u, comp, comp_len);
        out[1u + comp_len] = '\0';
        return 1;
    }

    if (len + 1u + comp_len + 1u > out_size) {
        return 0;
    }
    out[len] = '/';
    memcpy(out + len + 1u, comp, comp_len);
    out[len + 1u + comp_len] = '\0';
    return 1;
}

static int normalize_path_user(const char* path, char* out, size_t out_size) {
    char combined[PATH_MAX * 2];
    const char* p = path;

    if (!path || !out || out_size < 2u) {
        errno = EINVAL;
        return 0;
    }

    if (path[0] != '/') {
        if (!getcwd(combined, sizeof(combined))) {
            return 0;
        }
        if (strcmp(combined, "/") != 0) {
            size_t cwd_len = strlen(combined);
            size_t path_len = strlen(path);
            if (cwd_len + 1u + path_len + 1u > sizeof(combined)) {
                errno = ENAMETOOLONG;
                return 0;
            }
            combined[cwd_len] = '/';
            memcpy(combined + cwd_len + 1u, path, path_len + 1u);
        } else {
            size_t path_len = strlen(path);
            if (1u + path_len + 1u > sizeof(combined)) {
                errno = ENAMETOOLONG;
                return 0;
            }
            combined[0] = '/';
            memcpy(combined + 1u, path, path_len + 1u);
        }
        p = combined;
    }

    out[0] = '/';
    out[1] = '\0';
    while (*p) {
        const char* start;
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p && *p != '/') {
            p++;
        }
        if (!path_append(out, out_size, start, (size_t)(p - start))) {
            errno = ENAMETOOLONG;
            return 0;
        }
    }
    return 1;
}

char* realpath(const char* path, char* resolved_path) {
    char* out = resolved_path;

    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (!out) {
        out = (char*)malloc(PATH_MAX);
        if (!out) {
            return NULL;
        }
    }
    if (!normalize_path_user(path, out, PATH_MAX)) {
        if (!resolved_path) {
            free(out);
        }
        return NULL;
    }
    if (access(out, F_OK) != 0) {
        if (!resolved_path) {
            free(out);
        }
        return NULL;
    }
    return out;
}

static int fill_temp_template(char* template) {
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    size_t len;
    size_t x_start;
    unsigned long value;

    if (!template) {
        errno = EINVAL;
        return 0;
    }

    len = strlen(template);
    x_start = len;
    while (x_start > 0 && template[x_start - 1u] == 'X') {
        x_start--;
    }
    if (x_start == len) {
        errno = EINVAL;
        if (len > 0) {
            template[0] = '\0';
        }
        return 0;
    }

    value = (unsigned long)getpid() * 997ul + ++g_temp_counter;
    for (size_t i = len; i > x_start; i--) {
        template[i - 1u] = alphabet[value % 36u];
        value /= 36u;
    }
    return 1;
}

char* mktemp(char* template) {
    return fill_temp_template(template) ? template : NULL;
}

int mkstemp(char* template) {
    char original[PATH_MAX];

    if (!template || strlen(template) >= sizeof(original)) {
        errno = EINVAL;
        return -1;
    }

    strcpy(original, template);
    for (int i = 0; i < 32; i++) {
        strcpy(template, original);
        if (!fill_temp_template(template)) {
            return -1;
        }
        int fd = open(template, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            return fd;
        }
    }

    errno = EEXIST;
    return -1;
}

char* mkdtemp(char* template) {
    char original[PATH_MAX];

    if (!template || strlen(template) >= sizeof(original)) {
        errno = EINVAL;
        return NULL;
    }

    strcpy(original, template);
    for (int i = 0; i < 32; i++) {
        strcpy(template, original);
        if (!fill_temp_template(template)) {
            return NULL;
        }
        if (mkdir(template, 0700) == 0) {
            return template;
        }
    }

    errno = EEXIST;
    return NULL;
}

int system(const char* command) {
    if (!command) {
        return 0;
    }
    errno = ENOSYS;
    return -1;
}

int atexit(void (*function)(void)) {
    if (!function || g_atexit_count >= (sizeof(g_atexit_handlers) / sizeof(g_atexit_handlers[0]))) {
        errno = ENOMEM;
        return -1;
    }
    g_atexit_handlers[g_atexit_count++] = function;
    return 0;
}

void exit(int status) {
    while (g_atexit_count > 0) {
        void (*handler)(void) = g_atexit_handlers[--g_atexit_count];
        if (handler) {
            handler();
        }
    }
    sys_exit(status);
    __builtin_unreachable();
}

void abort(void) {
    sys_exit(134);
    __builtin_unreachable();
}

void assert(int expression) {
    if (!expression) {
        abort();
    }
}
