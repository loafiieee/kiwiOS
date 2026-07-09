#include <stdarg.h>
#include "stdio.h"
#include "string.h"
#include "term.h"
#include "unistd.h"

static char g_tparm_buf[64];
char PC;
char* UP = "\x1b[A";
char* BC = "\x1b[D";
short ospeed;

static int term_name_is(const char* capname, const char* a, const char* b) {
    return capname && (strcmp(capname, a) == 0 || (b && strcmp(capname, b) == 0));
}

int setupterm(char* term, int fd, int* errret) {
    (void)term;
    (void)fd;
    if (errret) {
        *errret = 1;
    }
    return 0;
}

char* tigetstr(const char* capname) {
    if (!capname) {
        return (char*)-1;
    }
    if (term_name_is(capname, "clear", "cl")) return "\x1b[2J";
    if (term_name_is(capname, "cup", "cm")) return "\x1b[%i%p1%d;%p2%dH";
    if (strcmp(capname, "hpa") == 0) return "\x1b[%i%p1%dG";
    if (strcmp(capname, "vpa") == 0) return "\x1b[%i%p1%dd";
    if (term_name_is(capname, "home", "ho")) return "\x1b[H";
    if (term_name_is(capname, "el", "ce")) return "\x1b[K";
    if (term_name_is(capname, "ed", "cd")) return "\x1b[J";
    if (strcmp(capname, "el1") == 0) return "\x1b[1K";
    if (term_name_is(capname, "cuu1", "up")) return "\x1b[A";
    if (term_name_is(capname, "cud1", "do")) return "\x1b[B";
    if (term_name_is(capname, "cuf1", "nd")) return "\x1b[C";
    if (term_name_is(capname, "cub1", "le")) return "\x1b[D";
    if (strcmp(capname, "cuu") == 0) return "\x1b[%p1%dA";
    if (strcmp(capname, "cud") == 0) return "\x1b[%p1%dB";
    if (strcmp(capname, "cuf") == 0) return "\x1b[%p1%dC";
    if (strcmp(capname, "cub") == 0) return "\x1b[%p1%dD";
    if (term_name_is(capname, "smkx", "ks")) return "\x1b[?1h\x1b=";
    if (term_name_is(capname, "rmkx", "ke")) return "\x1b[?1l\x1b>";
    if (term_name_is(capname, "smcup", "ti")) return "\x1b[?1049h";
    if (term_name_is(capname, "rmcup", "te")) return "\x1b[?1049l";
    if (term_name_is(capname, "civis", "vi")) return "\x1b[?25l";
    if (term_name_is(capname, "cnorm", "ve")) return "\x1b[?25h";
    if (strcmp(capname, "cvvis") == 0) return "\x1b[?25h";
    if (term_name_is(capname, "bold", "md")) return "\x1b[1m";
    if (strcmp(capname, "dim") == 0) return "\x1b[2m";
    if (term_name_is(capname, "smul", "us")) return "\x1b[4m";
    if (term_name_is(capname, "rmul", "ue")) return "\x1b[24m";
    if (term_name_is(capname, "rev", "mr")) return "\x1b[7m";
    if (term_name_is(capname, "smso", "so")) return "\x1b[7m";
    if (term_name_is(capname, "rmso", "se")) return "\x1b[27m";
    if (strcmp(capname, "blink") == 0) return "\x1b[5m";
    if (term_name_is(capname, "sgr0", "me")) return "\x1b[0m";
    if (strcmp(capname, "sgr") == 0) return "\x1b[%?%p1%t1;%;%?%p2%t4;%;%?%p3%t7;%;m";
    if (strcmp(capname, "setaf") == 0 || strcmp(capname, "setf") == 0) return "\x1b[3%p1%dm";
    if (strcmp(capname, "setab") == 0 || strcmp(capname, "setb") == 0) return "\x1b[4%p1%dm";
    if (strcmp(capname, "op") == 0) return "\x1b[39;49m";
    if (strcmp(capname, "sc") == 0) return "\x1b" "7";
    if (strcmp(capname, "rc") == 0) return "\x1b" "8";
    if (strcmp(capname, "csr") == 0) return "\x1b[%i%p1%d;%p2%dr";
    if (strcmp(capname, "ind") == 0) return "\n";
    if (strcmp(capname, "ri") == 0) return "\x1bM";
    if (strcmp(capname, "ht") == 0) return "\t";
    if (term_name_is(capname, "il1", "al")) return "\x1b[L";
    if (term_name_is(capname, "dl1", "dl")) return "\x1b[M";
    if (term_name_is(capname, "ich1", "ic")) return "\x1b[@";
    if (term_name_is(capname, "dch1", "dc")) return "\x1b[P";
    if (strcmp(capname, "il") == 0) return "\x1b[%p1%dL";
    if (strcmp(capname, "dl") == 0) return "\x1b[%p1%dM";
    if (strcmp(capname, "ich") == 0) return "\x1b[%p1%d@";
    if (strcmp(capname, "dch") == 0) return "\x1b[%p1%dP";
    if (term_name_is(capname, "kcuu1", "ku")) return "\x1b[A";
    if (term_name_is(capname, "kcud1", "kd")) return "\x1b[B";
    if (term_name_is(capname, "kcuf1", "kr")) return "\x1b[C";
    if (term_name_is(capname, "kcub1", "kl")) return "\x1b[D";
    if (term_name_is(capname, "khome", "kh")) return "\x1b[H";
    if (term_name_is(capname, "kend", "@7")) return "\x1b[F";
    if (term_name_is(capname, "kdch1", "kD")) return "\x1b[3~";
    if (term_name_is(capname, "kich1", "kI")) return "\x1b[2~";
    if (term_name_is(capname, "knp", "kN")) return "\x1b[6~";
    if (term_name_is(capname, "kpp", "kP")) return "\x1b[5~";
    if (strcmp(capname, "kent") == 0) return "\r";
    if (strcmp(capname, "kbs") == 0) return "\x7f";
    return (char*)-1;
}

int tigetnum(const char* capname) {
    if (!capname) {
        return -2;
    }
    if (strcmp(capname, "cols") == 0) return 80;
    if (strcmp(capname, "lines") == 0) return 24;
    if (strcmp(capname, "colors") == 0) return 8;
    if (strcmp(capname, "pairs") == 0) return 64;
    if (strcmp(capname, "it") == 0) return 8;
    return -2;
}

int tigetflag(const char* capname) {
    if (!capname) {
        return -1;
    }
    if (strcmp(capname, "am") == 0) return 1;
    if (strcmp(capname, "km") == 0) return 1;
    if (strcmp(capname, "mir") == 0) return 1;
    if (strcmp(capname, "xenl") == 0) return 1;
    return 0;
}

int tgetent(char* bp, const char* name) {
    (void)bp;
    (void)name;
    return 1;
}

int tgetflag(const char* id) {
    return tigetflag(id);
}

int tgetnum(const char* id) {
    return tigetnum(id);
}

char* tgetstr(const char* id, char** area) {
    char* cap = NULL;

    if (!id) {
        return NULL;
    }

    cap = tigetstr(id);

    if (cap == (char*)-1) {
        return NULL;
    }
    if (area && *area) {
        char* out = *area;
        size_t len = strlen(cap) + 1u;
        memcpy(out, cap, len);
        *area += len;
        return out;
    }
    return cap;
}

char* tgoto(const char* cap, int col, int row) {
    (void)cap;
    (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%d;%dH", row + 1, col + 1);
    return g_tparm_buf;
}

char* tparm(const char* str, ...) {
    va_list ap;
    long p1 = 0;
    long p2 = 0;

    if (!str) {
        return "";
    }

    if (strstr(str, "%p1") && strstr(str, "%p2")) {
        va_start(ap, str);
        p1 = va_arg(ap, long);
        p2 = va_arg(ap, long);
        va_end(ap);
        if (strstr(str, "r")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ld;%ldr", p1 + 1, p2 + 1);
        } else {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ld;%ldH", p1 + 1, p2 + 1);
        }
        return g_tparm_buf;
    }

    if (strstr(str, "%p1")) {
        va_start(ap, str);
        p1 = va_arg(ap, long);
        va_end(ap);
        if (strstr(str, "[3%p1")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[3%ldm", p1);
        } else if (strstr(str, "[4%p1")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[4%ldm", p1);
        } else if (strstr(str, "G")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldG", p1 + 1);
        } else if (strstr(str, "d")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldd", p1 + 1);
        } else if (strstr(str, "A")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldA", p1);
        } else if (strstr(str, "B")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldB", p1);
        } else if (strstr(str, "C")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldC", p1);
        } else if (strstr(str, "D")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldD", p1);
        } else if (strstr(str, "L")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldL", p1);
        } else if (strstr(str, "M")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldM", p1);
        } else if (strstr(str, "@")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ld@", p1);
        } else if (strstr(str, "P")) {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "\x1b[%ldP", p1);
        } else {
            (void)snprintf(g_tparm_buf, sizeof(g_tparm_buf), "%ld", p1);
        }
        return g_tparm_buf;
    }

    return (char*)str;
}

int putp(const char* str) {
    if (!str) {
        return -1;
    }
    return write(STDOUT_FILENO, str, strlen(str)) < 0 ? -1 : 0;
}

int tputs(const char* str, int affcnt, int (*putc_fn)(int)) {
    (void)affcnt;
    if (!str) {
        return -1;
    }
    while (*str) {
        if (putc_fn) {
            putc_fn((unsigned char)*str);
        } else {
            (void)write(STDOUT_FILENO, str, 1);
        }
        str++;
    }
    return 0;
}
