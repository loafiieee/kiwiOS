#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <term.h>
#include <termios.h>
#include <unistd.h>

static void write_str(const char* s) {
    while (s && *s) {
        putchar(*s++);
    }
}

static void write_dec(unsigned int value) {
    char buf[16];
    int i = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value != 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (i > 0) {
        putchar(buf[--i]);
    }
}

int main(void) {
    struct winsize ws;
    struct termios tio;
    char term_area[128];
    char* term_area_ptr = term_area;
    char* cap = NULL;
    int term_err = 0;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        puts("termtest: FAIL TIOCGWINSZ");
        return 1;
    }

    if (setupterm(NULL, STDOUT_FILENO, &term_err) != 0 || term_err != 1 ||
        tigetnum("colors") < 8 || tigetnum("it") != 8 ||
        tigetflag("am") != 1 || tigetflag("km") != 1) {
        puts("termtest: FAIL terminfo basics");
        return 4;
    }

    cap = tigetstr("cup");
    if (!cap || cap == (char*)-1 ||
        strcmp(tparm(cap, (long)4, (long)9), "\x1b[5;10H") != 0 ||
        strcmp(tparm(tigetstr("setaf"), (long)1), "\x1b[31m") != 0 ||
        strcmp(tigetstr("kcuu1"), "\x1b[A") != 0 ||
        strcmp(tigetstr("kdch1"), "\x1b[3~") != 0) {
        puts("termtest: FAIL terminfo strings");
        return 5;
    }

    cap = tgetstr("cm", &term_area_ptr);
    if (tgetent(NULL, "xterm") != 1 || !cap ||
        strcmp(tgoto(cap, 9, 4), "\x1b[5;10H") != 0 ||
        term_area_ptr <= term_area) {
        puts("termtest: FAIL termcap compatibility");
        return 6;
    }

    if (tcgetattr(STDIN_FILENO, &tio) != 0) {
        puts("termtest: FAIL tcgetattr");
        return 2;
    }
    if (ioctl(STDIN_FILENO, TIOCGETA, &tio) != 0 ||
        cfsetspeed(&tio, B115200) != 0 ||
        cfsetispeed(&tio, (speed_t)12345) == 0) {
        puts("termtest: FAIL termios compatibility");
        return 7;
    }

    cfmakeraw(&tio);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) != 0) {
        puts("termtest: FAIL tcsetattr");
        return 3;
    }

    write_str("\x1b[2J\x1b[H");
    write_str("termtest: terminal size rows=");
    write_dec(ws.ws_row);
    write_str(" cols=");
    write_dec(ws.ws_col);
    putchar('\n');
    write_str("\x1b[31mred\x1b[0m normal \x1b[32mgreen\x1b[0m\n");
    write_str("\x1b[5;10Hcursor move ok\n");
    write_str("\x1b[7;1Htermtest: PASS terminal primitives\n");
    return 0;
}
