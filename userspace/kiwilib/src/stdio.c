#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "ctype.h"
#include "errno.h"
#include "fcntl.h"
#include "kiwi_syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

struct FILE {
    int fd;
    int eof;
    int error;
    int has_ungot;
    unsigned char ungot;
    char remove_on_close[FILENAME_MAX];
};

static FILE g_stdin = { STDIN_FILENO, 0, 0, 0, 0, { 0 } };
static FILE g_stdout = { STDOUT_FILENO, 0, 0, 0, 0, { 0 } };
static FILE g_stderr = { STDERR_FILENO, 0, 0, 0, 0, { 0 } };

FILE* stdin = &g_stdin;
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;

typedef struct {
    char* buf;
    size_t size;
    size_t pos;
    int fd;
    int error;
} out_sink_t;

static void sink_putc(out_sink_t* sink, char c) {
    if (!sink) {
        return;
    }

    if (sink->buf && sink->size > 0 && sink->pos + 1u < sink->size) {
        sink->buf[sink->pos] = c;
    } else if (!sink->buf && sink->fd >= 0) {
        if (sys_write(sink->fd, &c, 1) != 1) {
            sink->error = 1;
        }
    }

    sink->pos++;
}

static int is_builtin_stream(FILE* stream) {
    return stream == &g_stdin || stream == &g_stdout || stream == &g_stderr;
}

static int parse_fopen_mode(const char* mode) {
    int flags;

    if (!mode || !mode[0]) {
        return -1;
    }

    if (mode[0] == 'r') {
        flags = O_RDONLY;
    } else if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else {
        return -1;
    }

    for (const char* p = mode + 1; *p; p++) {
        if (*p == '+') {
            flags &= ~(O_RDONLY | O_WRONLY);
            flags |= O_RDWR;
        }
    }

    return flags;
}

static void stream_init(FILE* stream, int fd) {
    stream->fd = fd;
    stream->eof = 0;
    stream->error = 0;
    stream->has_ungot = 0;
    stream->ungot = 0;
    stream->remove_on_close[0] = '\0';
}

static FILE* stream_alloc(int fd) {
    FILE* stream = (FILE*)malloc(sizeof(FILE));

    if (!stream) {
        errno = ENOMEM;
        return NULL;
    }

    stream_init(stream, fd);
    return stream;
}

static void sink_write(out_sink_t* sink, const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        sink_putc(sink, s[i]);
    }
}

static void sink_finish(out_sink_t* sink) {
    if (!sink || !sink->buf || sink->size == 0) {
        return;
    }
    if (sink->pos < sink->size) {
        sink->buf[sink->pos] = '\0';
    } else {
        sink->buf[sink->size - 1u] = '\0';
    }
}

static size_t utoa_base(uint64_t value, unsigned base, int upper, char* out, size_t out_size) {
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char* digits = upper ? upper_digits : lower_digits;
    char tmp[65];
    size_t n = 0;

    if (out_size == 0 || base < 2 || base > 16) {
        return 0;
    }

    if (value == 0) {
        out[0] = '0';
        return 1;
    }

    while (value != 0 && n < sizeof(tmp)) {
        tmp[n++] = digits[value % base];
        value /= base;
    }

    if (n > out_size) {
        n = out_size;
    }

    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }
    return n;
}

static void format_padding(out_sink_t* sink, int count, char ch) {
    while (count-- > 0) {
        sink_putc(sink, ch);
    }
}

static void format_text(out_sink_t* sink,
                        const char* text,
                        size_t len,
                        int width,
                        int left_align,
                        char pad) {
    int padding = width > (int)len ? width - (int)len : 0;

    if (!left_align) {
        format_padding(sink, padding, pad);
    }
    sink_write(sink, text, len);
    if (left_align) {
        format_padding(sink, padding, ' ');
    }
}

static int kvformat(out_sink_t* sink, const char* fmt, va_list ap) {
    while (*fmt) {
        int left_align = 0;
        int force_sign = 0;
        int space_sign = 0;
        int alternate = 0;
        char pad = ' ';
        int width = 0;
        int precision = -1;
        int length = 0;
        char spec;
        char numbuf[68];
        char prefix[3];
        size_t num_len = 0;
        size_t prefix_len = 0;

        if (*fmt != '%') {
            sink_putc(sink, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '%') {
            sink_putc(sink, *fmt++);
            continue;
        }

        for (;;) {
            if (*fmt == '-') {
                left_align = 1;
                fmt++;
                continue;
            }
            if (*fmt == '+') {
                force_sign = 1;
                fmt++;
                continue;
            }
            if (*fmt == ' ') {
                space_sign = 1;
                fmt++;
                continue;
            }
            if (*fmt == '#') {
                alternate = 1;
                fmt++;
                continue;
            }
            if (*fmt == '0') {
                pad = '0';
                fmt++;
                continue;
            }
            break;
        }

        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left_align = 1;
                width = -width;
            }
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        if (*fmt == '.') {
            precision = 0;
            fmt++;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                if (precision < 0) {
                    precision = -1;
                }
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        if (*fmt == 'l') {
            length = 1;
            fmt++;
            if (*fmt == 'l') {
                length = 2;
                fmt++;
            }
        } else if (*fmt == 'h') {
            length = -1;
            fmt++;
            if (*fmt == 'h') {
                length = -2;
                fmt++;
            }
        } else if (*fmt == 'z') {
            length = 3;
            fmt++;
        } else if (*fmt == 'j' || *fmt == 't') {
            length = 2;
            fmt++;
        }

        spec = *fmt ? *fmt++ : '\0';
        prefix[0] = '\0';

        if (spec == 's') {
            const char* s = va_arg(ap, const char*);
            size_t len;
            if (!s) {
                s = "(null)";
            }
            len = strlen(s);
            if (precision >= 0 && len > (size_t)precision) {
                len = (size_t)precision;
            }
            format_text(sink, s, len, width, left_align, ' ');
        } else if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            format_text(sink, &c, 1, width, left_align, ' ');
        } else if (spec == 'd' || spec == 'i') {
            int64_t value;
            uint64_t mag;
            if (length == 2) {
                value = va_arg(ap, long long);
            } else if (length == 1) {
                value = va_arg(ap, long);
            } else if (length == 3) {
                value = (int64_t)va_arg(ap, size_t);
            } else {
                value = va_arg(ap, int);
            }
            if (value < 0) {
                prefix[0] = '-';
                prefix[1] = '\0';
                prefix_len = 1;
                mag = (uint64_t)(-(value + 1)) + 1u;
            } else {
                if (force_sign) {
                    prefix[0] = '+';
                    prefix[1] = '\0';
                    prefix_len = 1;
                } else if (space_sign) {
                    prefix[0] = ' ';
                    prefix[1] = '\0';
                    prefix_len = 1;
                }
                mag = (uint64_t)value;
            }
            num_len = utoa_base(mag, 10, 0, numbuf + prefix_len, sizeof(numbuf) - prefix_len);
            memcpy(numbuf, prefix, prefix_len);
            format_text(sink, numbuf, prefix_len + num_len, width, left_align, pad);
        } else if (spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X') {
            uint64_t value;
            unsigned base = spec == 'u' ? 10u : (spec == 'o' ? 8u : 16u);
            if (length == 2) {
                value = va_arg(ap, unsigned long long);
            } else if (length == 1) {
                value = va_arg(ap, unsigned long);
            } else if (length == 3) {
                value = va_arg(ap, size_t);
            } else {
                value = va_arg(ap, unsigned int);
            }
            if (alternate && value != 0 && spec == 'o') {
                prefix[0] = '0';
                prefix[1] = '\0';
                prefix_len = 1;
            } else if (alternate && value != 0 && (spec == 'x' || spec == 'X')) {
                prefix[0] = '0';
                prefix[1] = spec;
                prefix[2] = '\0';
                prefix_len = 2;
            }
            num_len = utoa_base(value, base, spec == 'X', numbuf + prefix_len, sizeof(numbuf) - prefix_len);
            memcpy(numbuf, prefix, prefix_len);
            format_text(sink, numbuf, prefix_len + num_len, width, left_align, pad);
        } else if (spec == 'p') {
            uint64_t value = (uint64_t)(uintptr_t)va_arg(ap, void*);
            numbuf[0] = '0';
            numbuf[1] = 'x';
            num_len = utoa_base(value, 16, 0, numbuf + 2, sizeof(numbuf) - 2u);
            format_text(sink, numbuf, num_len + 2u, width, left_align, pad);
        } else {
            sink_putc(sink, '%');
            if (spec) {
                sink_putc(sink, spec);
            }
        }
    }

    return sink->error ? -1 : (int)sink->pos;
}

int fputc(int ch, FILE* stream) {
    char c = (char)ch;
    if (!stream || sys_write(stream->fd, &c, 1) != 1) {
        if (stream) {
            stream->error = 1;
        }
        return EOF;
    }
    return (unsigned char)c;
}

int putc(int ch, FILE* stream) {
    return fputc(ch, stream);
}

int putchar(int ch) {
    return fputc(ch, stdout);
}

int putc_unlocked(int ch, FILE* stream) {
    return putc(ch, stream);
}

int putchar_unlocked(int ch) {
    return putchar(ch);
}

int fputs(const char* s, FILE* stream) {
    size_t len = 0;

    if (!s || !stream) {
        return EOF;
    }

    len = strlen(s);
    if (len != 0 && sys_write(stream->fd, s, (uint64_t)len) != (int64_t)len) {
        stream->error = 1;
        return EOF;
    }

    return 0;
}

int fputs_unlocked(const char* s, FILE* stream) {
    return fputs(s, stream);
}

int puts(const char* s) {
    if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF) {
        return EOF;
    }
    return (int)(strlen(s) + 1u);
}

int fflush(FILE* stream) {
    (void)stream;
    return 0;
}

int fflush_unlocked(FILE* stream) {
    return fflush(stream);
}

FILE* fopen(const char* path, const char* mode) {
    int flags = parse_fopen_mode(mode);
    int fd;

    if (!path || flags < 0) {
        errno = EINVAL;
        return NULL;
    }

    fd = open(path, flags, 0666);
    if (fd < 0) {
        return NULL;
    }

    FILE* stream = stream_alloc(fd);
    if (!stream) {
        close(fd);
        return NULL;
    }

    return stream;
}

FILE* fdopen(int fd, const char* mode) {
    if (fd < 0 || parse_fopen_mode(mode) < 0) {
        errno = EINVAL;
        return NULL;
    }

    return stream_alloc(fd);
}

FILE* freopen(const char* path, const char* mode, FILE* stream) {
    int flags = parse_fopen_mode(mode);
    int fd;

    if (!path || !stream || flags < 0) {
        errno = EINVAL;
        return NULL;
    }

    fd = open(path, flags, 0666);
    if (fd < 0) {
        return NULL;
    }

    (void)fflush(stream);
    (void)close(stream->fd);
    stream_init(stream, fd);
    return stream;
}

FILE* tmpfile(void) {
    char path[FILENAME_MAX];
    int fd = -1;
    FILE* stream = NULL;

    strcpy(path, "/tmp/tmpfile.XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) {
        strcpy(path, "tmpfile.XXXXXX");
        fd = mkstemp(path);
    }
    if (fd < 0) {
        return NULL;
    }

    stream = stream_alloc(fd);
    if (!stream) {
        close(fd);
        (void)unlink(path);
        return NULL;
    }
    strlcpy(stream->remove_on_close, path, sizeof(stream->remove_on_close));
    return stream;
}

char* tmpnam(char* s) {
    static char buf[L_tmpnam];
    char* out = s ? s : buf;

    strcpy(out, "/tmp/tmpnam.XXXXXX");
    return mktemp(out);
}

int fclose(FILE* stream) {
    int ret;

    if (!stream) {
        errno = EINVAL;
        return EOF;
    }

    ret = fflush(stream);
    if (close(stream->fd) != 0) {
        ret = EOF;
    }
    if (stream->remove_on_close[0] != '\0') {
        (void)unlink(stream->remove_on_close);
        stream->remove_on_close[0] = '\0';
    }

    if (!is_builtin_stream(stream)) {
        free(stream);
    }
    return ret;
}

int remove(const char* path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    if (unlink(path) == 0) {
        return 0;
    }
    return rmdir(path);
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t total;
    size_t done = 0;
    unsigned char* out = (unsigned char*)ptr;

    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }
    if (nmemb > ((size_t)-1 / size)) {
        stream->error = 1;
        errno = EINVAL;
        return 0;
    }

    total = size * nmemb;
    while (done < total) {
        ssize_t n;

        if (stream->has_ungot) {
            out[done++] = stream->ungot;
            stream->has_ungot = 0;
            continue;
        }

        n = read(stream->fd, out + done, total - done);
        if (n < 0) {
            stream->error = 1;
            break;
        }
        if (n == 0) {
            stream->eof = 1;
            break;
        }
        done += (size_t)n;
    }

    return done / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t total;
    size_t done = 0;
    const unsigned char* in = (const unsigned char*)ptr;

    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }
    if (nmemb > ((size_t)-1 / size)) {
        stream->error = 1;
        errno = EINVAL;
        return 0;
    }

    total = size * nmemb;
    while (done < total) {
        ssize_t n = write(stream->fd, in + done, total - done);
        if (n <= 0) {
            stream->error = 1;
            break;
        }
        done += (size_t)n;
    }

    return done / size;
}

int fgetc(FILE* stream) {
    unsigned char c;
    ssize_t n;

    if (!stream) {
        errno = EINVAL;
        return EOF;
    }

    if (stream->has_ungot) {
        stream->has_ungot = 0;
        return stream->ungot;
    }

    n = read(stream->fd, &c, 1);
    if (n == 1) {
        return c;
    }
    if (n == 0) {
        stream->eof = 1;
    } else {
        stream->error = 1;
    }
    return EOF;
}

int getc(FILE* stream) {
    return fgetc(stream);
}

int getchar(void) {
    return fgetc(stdin);
}

int getc_unlocked(FILE* stream) {
    return getc(stream);
}

int getchar_unlocked(void) {
    return getchar();
}

char* fgets(char* s, int size, FILE* stream) {
    int i = 0;

    if (!s || size <= 0 || !stream) {
        errno = EINVAL;
        return NULL;
    }

    while (i + 1 < size) {
        int c = fgetc(stream);
        if (c == EOF) {
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }

    if (i == 0) {
        return NULL;
    }
    s[i] = '\0';
    return s;
}

ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream) {
    size_t cap;
    size_t len = 0;
    char* line;

    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }

    cap = *n;
    line = *lineptr;
    if (!line || cap == 0) {
        cap = 128;
        line = (char*)malloc(cap);
        if (!line) {
            errno = ENOMEM;
            return -1;
        }
    }

    for (;;) {
        int ch = fgetc(stream);
        char* next;

        if (ch == EOF) {
            if (len == 0) {
                if (line != *lineptr) {
                    *lineptr = line;
                    *n = cap;
                }
                return -1;
            }
            break;
        }

        if (len + 2u > cap) {
            cap *= 2u;
            next = (char*)realloc(line, cap);
            if (!next) {
                if (line != *lineptr) {
                    free(line);
                }
                errno = ENOMEM;
                return -1;
            }
            line = next;
        }

        line[len++] = (char)ch;
        if (ch == delim) {
            break;
        }
    }

    line[len] = '\0';
    *lineptr = line;
    *n = cap;
    return (ssize_t)len;
}

ssize_t getline(char** lineptr, size_t* n, FILE* stream) {
    return getdelim(lineptr, n, '\n', stream);
}

int ungetc(int ch, FILE* stream) {
    if (!stream || ch == EOF || stream->has_ungot) {
        return EOF;
    }

    stream->ungot = (unsigned char)ch;
    stream->has_ungot = 1;
    stream->eof = 0;
    return (unsigned char)ch;
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    if (lseek(stream->fd, (off_t)offset, whence) < 0) {
        stream->error = 1;
        return -1;
    }
    stream->eof = 0;
    stream->has_ungot = 0;
    return 0;
}

long ftell(FILE* stream) {
    off_t pos;

    if (!stream) {
        errno = EINVAL;
        return -1;
    }

    pos = lseek(stream->fd, 0, SEEK_CUR);
    return pos < 0 ? -1 : (long)pos;
}

int fseeko(FILE* stream, long offset, int whence) {
    return fseek(stream, offset, whence);
}

long ftello(FILE* stream) {
    return ftell(stream);
}

int fgetpos(FILE* stream, fpos_t* pos) {
    if (!pos) {
        errno = EINVAL;
        return -1;
    }
    *pos = ftell(stream);
    return *pos < 0 ? -1 : 0;
}

int fsetpos(FILE* stream, const fpos_t* pos) {
    if (!pos) {
        errno = EINVAL;
        return -1;
    }
    return fseek(stream, *pos, SEEK_SET);
}

void rewind(FILE* stream) {
    if (stream) {
        (void)fseek(stream, 0, SEEK_SET);
        clearerr(stream);
    }
}

int feof(FILE* stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE* stream) {
    return stream ? stream->error : 1;
}

void clearerr(FILE* stream) {
    if (stream) {
        stream->eof = 0;
        stream->error = 0;
    }
}

void __fseterr(FILE* stream) {
    if (stream) {
        stream->error = 1;
    }
}

int fileno(FILE* stream) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    return stream->fd;
}

int setvbuf(FILE* stream, char* buf, int mode, size_t size) {
    (void)buf;
    (void)mode;
    (void)size;
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void setbuf(FILE* stream, char* buf) {
    (void)setvbuf(stream, buf, 0, BUFSIZ);
}

static int scan_set_match(const char* spec_start, const char* spec_end, int negate, int ch) {
    const char* p = spec_start;
    int matched = 0;

    while (p < spec_end) {
        int first = (unsigned char)*p++;
        if (first == '\\' && p < spec_end) {
            first = (unsigned char)*p++;
        }
        if (p < spec_end - 1 && *p == '-') {
            int last = (unsigned char)p[1];
            if (first <= last) {
                matched |= ch >= first && ch <= last;
            } else {
                matched |= ch >= last && ch <= first;
            }
            p += 2;
        } else {
            matched |= ch == first;
        }
    }

    return negate ? !matched : matched;
}

static void scan_store_signed(va_list ap, int length, long long value) {
    if (length == -2) {
        signed char* out = va_arg(ap, signed char*);
        *out = (signed char)value;
    } else if (length == -1) {
        short* out = va_arg(ap, short*);
        *out = (short)value;
    } else if (length == 1) {
        long* out = va_arg(ap, long*);
        *out = (long)value;
    } else if (length >= 2) {
        long long* out = va_arg(ap, long long*);
        *out = value;
    } else {
        int* out = va_arg(ap, int*);
        *out = (int)value;
    }
}

static void scan_store_unsigned(va_list ap, int length, unsigned long long value) {
    if (length == -2) {
        unsigned char* out = va_arg(ap, unsigned char*);
        *out = (unsigned char)value;
    } else if (length == -1) {
        unsigned short* out = va_arg(ap, unsigned short*);
        *out = (unsigned short)value;
    } else if (length == 1) {
        unsigned long* out = va_arg(ap, unsigned long*);
        *out = (unsigned long)value;
    } else if (length >= 2) {
        unsigned long long* out = va_arg(ap, unsigned long long*);
        *out = value;
    } else {
        unsigned int* out = va_arg(ap, unsigned int*);
        *out = (unsigned int)value;
    }
}

int vsscanf(const char* str, const char* fmt, va_list ap) {
    const char* in = str;
    int assigned = 0;

    if (!str || !fmt) {
        errno = EINVAL;
        return EOF;
    }

    while (*fmt) {
        int suppress = 0;
        int width = 0;
        int length = 0;
        char spec;

        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt)) {
                fmt++;
            }
            while (isspace((unsigned char)*in)) {
                in++;
            }
            continue;
        }

        if (*fmt != '%') {
            if (*in != *fmt) {
                break;
            }
            in++;
            fmt++;
            continue;
        }

        fmt++;
        if (*fmt == '%') {
            if (*in != '%') {
                break;
            }
            in++;
            fmt++;
            continue;
        }

        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        while (isdigit((unsigned char)*fmt)) {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == 'h') {
            length = -1;
            fmt++;
            if (*fmt == 'h') {
                length = -2;
                fmt++;
            }
        } else if (*fmt == 'l') {
            length = 1;
            fmt++;
            if (*fmt == 'l') {
                length = 2;
                fmt++;
            }
        } else if (*fmt == 'j' || *fmt == 'z' || *fmt == 't') {
            length = 2;
            fmt++;
        }

        spec = *fmt ? *fmt++ : '\0';
        if (spec == '\0') {
            break;
        }

        if (spec != 'c' && spec != '[' && spec != 'n') {
            while (isspace((unsigned char)*in)) {
                in++;
            }
        }

        if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X') {
            char tmp[128];
            char* end = NULL;
            size_t len = 0;
            int base = 10;

            if (spec == 'i') {
                base = 0;
            } else if (spec == 'o') {
                base = 8;
            } else if (spec == 'x' || spec == 'X') {
                base = 16;
            }

            while (in[len] && !isspace((unsigned char)in[len]) && len + 1u < sizeof(tmp) &&
                   (width == 0 || (int)len < width)) {
                tmp[len] = in[len];
                len++;
            }
            tmp[len] = '\0';
            if (len == 0) {
                break;
            }

            if (spec == 'd' || spec == 'i') {
                long long value = strtoll(tmp, &end, base);
                if (end == tmp) {
                    break;
                }
                if (!suppress) {
                    scan_store_signed(ap, length, value);
                    assigned++;
                }
            } else {
                unsigned long long value = strtoull(tmp, &end, base);
                if (end == tmp) {
                    break;
                }
                if (!suppress) {
                    scan_store_unsigned(ap, length, value);
                    assigned++;
                }
            }
            in += end - tmp;
        } else if (spec == 's') {
            char* out = suppress ? NULL : va_arg(ap, char*);
            int copied = 0;

            while (*in && !isspace((unsigned char)*in) && (width == 0 || copied < width)) {
                if (out) {
                    out[copied] = *in;
                }
                copied++;
                in++;
            }
            if (copied == 0) {
                break;
            }
            if (out) {
                out[copied] = '\0';
                assigned++;
            }
        } else if (spec == 'c') {
            char* out = suppress ? NULL : va_arg(ap, char*);
            int count = width ? width : 1;

            for (int i = 0; i < count; i++) {
                if (*in == '\0') {
                    return assigned == 0 ? EOF : assigned;
                }
                if (out) {
                    out[i] = *in;
                }
                in++;
            }
            if (out) {
                assigned++;
            }
        } else if (spec == '[') {
            const char* set_start = fmt;
            const char* set_end = NULL;
            int negate = 0;
            char* out = suppress ? NULL : va_arg(ap, char*);
            int copied = 0;

            if (*set_start == '^') {
                negate = 1;
                set_start++;
            }
            set_end = set_start;
            if (*set_end == ']') {
                set_end++;
            }
            while (*set_end && *set_end != ']') {
                set_end++;
            }
            if (*set_end != ']') {
                break;
            }
            fmt = set_end + 1;

            while (*in && scan_set_match(set_start, set_end, negate, (unsigned char)*in) &&
                   (width == 0 || copied < width)) {
                if (out) {
                    out[copied] = *in;
                }
                copied++;
                in++;
            }
            if (copied == 0) {
                break;
            }
            if (out) {
                out[copied] = '\0';
                assigned++;
            }
        } else if (spec == 'n') {
            if (!suppress) {
                scan_store_signed(ap, length, (long long)(in - str));
            }
        } else {
            break;
        }
    }

    return assigned;
}

int sscanf(const char* str, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsscanf(str, fmt, ap);
    va_end(ap);
    return ret;
}

int vfscanf(FILE* stream, const char* fmt, va_list ap) {
    char buf[1024];
    size_t len = 0;

    if (!stream || !fmt) {
        errno = EINVAL;
        return EOF;
    }

    while (len + 1u < sizeof(buf)) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            break;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    if (len == 0 && feof(stream)) {
        return EOF;
    }
    return vsscanf(buf, fmt, ap);
}

int fscanf(FILE* stream, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vfscanf(stream, fmt, ap);
    va_end(ap);
    return ret;
}

int vscanf(const char* fmt, va_list ap) {
    return vfscanf(stdin, fmt, ap);
}

int scanf(const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vscanf(fmt, ap);
    va_end(ap);
    return ret;
}

int vfprintf(FILE* stream, const char* fmt, va_list ap) {
    out_sink_t sink;

    if (!stream || !fmt) {
        return EOF;
    }

    sink.buf = NULL;
    sink.size = 0;
    sink.pos = 0;
    sink.fd = stream->fd;
    sink.error = 0;
    return kvformat(&sink, fmt, ap);
}

int vdprintf(int fd, const char* fmt, va_list ap) {
    FILE stream;

    stream_init(&stream, fd);
    return vfprintf(&stream, fmt, ap);
}

int dprintf(int fd, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vdprintf(fd, fmt, ap);
    va_end(ap);
    return ret;
}

int fprintf(FILE* stream, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vfprintf(stream, fmt, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char* fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    out_sink_t sink;
    int ret;

    if (!fmt || (!buf && size != 0)) {
        return EOF;
    }

    sink.buf = buf;
    sink.size = size;
    sink.pos = 0;
    sink.fd = -1;
    sink.error = 0;
    ret = kvformat(&sink, fmt, ap);
    sink_finish(&sink);
    return ret;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int vsprintf(char* buf, const char* fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsprintf(buf, fmt, ap);
    va_end(ap);
    return ret;
}

int vasprintf(char** strp, const char* fmt, va_list ap) {
    va_list copy;
    int len;
    char* buf;

    if (!strp || !fmt) {
        errno = EINVAL;
        return -1;
    }

    va_copy(copy, ap);
    len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0) {
        return -1;
    }

    buf = (char*)malloc((size_t)len + 1u);
    if (!buf) {
        errno = ENOMEM;
        return -1;
    }

    if (vsnprintf(buf, (size_t)len + 1u, fmt, ap) < 0) {
        free(buf);
        return -1;
    }
    *strp = buf;
    return len;
}

int asprintf(char** strp, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vasprintf(strp, fmt, ap);
    va_end(ap);
    return ret;
}

void perror(const char* s) {
    if (s && *s) {
        fprintf(stderr, "%s: errno %d\n", s, errno);
    } else {
        fprintf(stderr, "errno %d\n", errno);
    }
}
