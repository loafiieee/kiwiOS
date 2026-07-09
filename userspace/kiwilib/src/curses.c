#include <stdarg.h>
#include <stddef.h>
#include "curses.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/ioctl.h"
#include "termios.h"
#include "unistd.h"

static WINDOW g_stdscr;
static SCREEN g_screen;
static struct termios g_saved_termios;
static int g_have_saved_termios;
static int g_is_endwin = 1;
static int g_unget = ERR;
static int g_escdelay = 1000;
static unsigned long g_mousemask;

WINDOW* stdscr = &g_stdscr;
WINDOW* curscr = &g_stdscr;
int LINES = 24;
int COLS = 80;
int COLORS = 8;
int COLOR_PAIRS = 64;

static void curses_write(const char* s) {
    if (s) {
        (void)write(STDOUT_FILENO, s, strlen(s));
    }
}

static void curses_move_abs(int y, int x) {
    char seq[32];
    if (y < 0) {
        y = 0;
    }
    if (x < 0) {
        x = 0;
    }
    (void)snprintf(seq, sizeof(seq), "\x1b[%d;%dH", y + 1, x + 1);
    curses_write(seq);
}

static void curses_refresh_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        LINES = ws.ws_row;
        COLS = ws.ws_col;
    }
    g_stdscr.begy = 0;
    g_stdscr.begx = 0;
    g_stdscr.rows = LINES;
    g_stdscr.cols = COLS;
}

static void curses_apply_raw(int enable) {
    struct termios tio;

    if (enable) {
        if (!g_have_saved_termios && tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
            g_have_saved_termios = 1;
        }
        if (g_have_saved_termios) {
            tio = g_saved_termios;
            cfmakeraw(&tio);
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }
    } else if (g_have_saved_termios) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    }
}

WINDOW* initscr(void) {
    curses_refresh_size();
    g_stdscr.cury = 0;
    g_stdscr.curx = 0;
    g_stdscr.delay_ms = -1;
    g_stdscr.attrs = A_NORMAL;
    curses_apply_raw(1);
    curses_write("\x1b[?25l");
    g_is_endwin = 0;
    return stdscr;
}

int endwin(void) {
    curses_write("\x1b[0m\x1b[?25h");
    curses_apply_raw(0);
    g_is_endwin = 1;
    return OK;
}

int isendwin(void) {
    return g_is_endwin;
}

int def_shell_mode(void) {
    return g_have_saved_termios ? OK : ERR;
}

int reset_shell_mode(void) {
    curses_apply_raw(0);
    return OK;
}

int wrefresh(WINDOW* win) {
    (void)win;
    return OK;
}

int refresh(void) {
    return wrefresh(stdscr);
}

int wnoutrefresh(WINDOW* win) {
    return wrefresh(win);
}

int doupdate(void) {
    return OK;
}

int werase(WINDOW* win) {
    if (!win) {
        return ERR;
    }
    curses_write("\x1b[2J");
    (void)wmove(win, 0, 0);
    return OK;
}

int erase(void) {
    return werase(stdscr);
}

int wclear(WINDOW* win) {
    return werase(win);
}

int clear(void) {
    return wclear(stdscr);
}

int wclrtobot(WINDOW* win) {
    if (!win) {
        return ERR;
    }
    curses_write("\x1b[J");
    return OK;
}

int clrtobot(void) {
    return wclrtobot(stdscr);
}

int wclrtoeol(WINDOW* win) {
    (void)win;
    curses_write("\x1b[K");
    return OK;
}

int clrtoeol(void) {
    return wclrtoeol(stdscr);
}

int wmove(WINDOW* win, int y, int x) {
    if (!win || y < 0 || x < 0) {
        return ERR;
    }
    win->cury = y;
    win->curx = x;
    curses_move_abs(win->begy + y, win->begx + x);
    return OK;
}

int move(int y, int x) {
    return wmove(stdscr, y, x);
}

static void curses_apply_attrs(attr_t attrs) {
    curses_write("\x1b[0m");
    if ((attrs & A_BOLD) != 0) {
        curses_write("\x1b[1m");
    }
    if ((attrs & A_REVERSE) != 0 || (attrs & A_STANDOUT) != 0) {
        curses_write("\x1b[7m");
    }
    if ((attrs & A_UNDERLINE) != 0) {
        curses_write("\x1b[4m");
    }
}

int waddch(WINDOW* win, chtype ch) {
    char c = (char)(ch & A_CHARTEXT);

    if (!win) {
        return ERR;
    }

    curses_apply_attrs(win->attrs | (ch & ~A_CHARTEXT));
    if (write(STDOUT_FILENO, &c, 1) != 1) {
        return ERR;
    }
    curses_apply_attrs(win->attrs);
    if (c == '\n') {
        win->cury++;
        win->curx = 0;
    } else {
        win->curx++;
    }
    return OK;
}

int addch(chtype ch) {
    return waddch(stdscr, ch);
}

int mvwaddch(WINDOW* win, int y, int x, chtype ch) {
    if (wmove(win, y, x) != OK) {
        return ERR;
    }
    return waddch(win, ch);
}

int mvaddch(int y, int x, chtype ch) {
    return mvwaddch(stdscr, y, x, ch);
}

int waddnstr(WINDOW* win, const char* str, int n) {
    int len = 0;

    if (!win || !str) {
        return ERR;
    }
    len = (n < 0) ? (int)strlen(str) : n;
    curses_apply_attrs(win->attrs);
    if (len > 0 && write(STDOUT_FILENO, str, (size_t)len) != len) {
        return ERR;
    }
    curses_apply_attrs(A_NORMAL);
    win->curx += len;
    return OK;
}

int waddstr(WINDOW* win, const char* str) {
    return waddnstr(win, str, -1);
}

int addnstr(const char* str, int n) {
    return waddnstr(stdscr, str, n);
}

int addstr(const char* str) {
    return waddstr(stdscr, str);
}

int waddbytes(WINDOW* win, const char* str, int n) {
    return waddnstr(win, str, n);
}

int addbytes(const char* str, int n) {
    return waddbytes(stdscr, str, n);
}

int mvwaddnstr(WINDOW* win, int y, int x, const char* str, int n) {
    if (wmove(win, y, x) != OK) {
        return ERR;
    }
    return waddnstr(win, str, n);
}

int mvaddnstr(int y, int x, const char* str, int n) {
    return mvwaddnstr(stdscr, y, x, str, n);
}

int mvwaddstr(WINDOW* win, int y, int x, const char* str) {
    return mvwaddnstr(win, y, x, str, -1);
}

int mvaddstr(int y, int x, const char* str) {
    return mvwaddstr(stdscr, y, x, str);
}

int vwprintw(WINDOW* win, const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        return ERR;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    return waddnstr(win, buf, n);
}

int wprintw(WINDOW* win, const char* fmt, ...) {
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vwprintw(win, fmt, ap);
    va_end(ap);
    return ret;
}

int printw(const char* fmt, ...) {
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vwprintw(stdscr, fmt, ap);
    va_end(ap);
    return ret;
}

int mvwprintw(WINDOW* win, int y, int x, const char* fmt, ...) {
    va_list ap;
    int ret;
    if (wmove(win, y, x) != OK) {
        return ERR;
    }
    va_start(ap, fmt);
    ret = vwprintw(win, fmt, ap);
    va_end(ap);
    return ret;
}

int mvprintw(int y, int x, const char* fmt, ...) {
    va_list ap;
    int ret;
    if (move(y, x) != OK) {
        return ERR;
    }
    va_start(ap, fmt);
    ret = vwprintw(stdscr, fmt, ap);
    va_end(ap);
    return ret;
}

static int curses_read_byte(WINDOW* win) {
    unsigned char c;

    if (win && win->delay_ms == 0) {
        int available = 0;
        if (ioctl(STDIN_FILENO, FIONREAD, &available) != 0 || available <= 0) {
            return ERR;
        }
    }

    return read(STDIN_FILENO, &c, 1) == 1 ? (int)c : ERR;
}

int wgetch(WINDOW* win) {
    int ch;

    (void)win;
    if (g_unget != ERR) {
        ch = g_unget;
        g_unget = ERR;
        return ch;
    }

    ch = curses_read_byte(win);
    if (ch != 0x1b) {
        return ch;
    }

    ch = curses_read_byte(win);
    if (ch != '[') {
        return 0x1b;
    }

    ch = curses_read_byte(win);
    switch (ch) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '3':
            (void)curses_read_byte(win);
            return KEY_DC;
        case '5':
            (void)curses_read_byte(win);
            return KEY_PPAGE;
        case '6':
            (void)curses_read_byte(win);
            return KEY_NPAGE;
        default:
            return 0x1b;
    }
}

int getch(void) {
    return wgetch(stdscr);
}

int mvwgetch(WINDOW* win, int y, int x) {
    if (wmove(win, y, x) != OK) {
        return ERR;
    }
    return wgetch(win);
}

int mvgetch(int y, int x) {
    return mvwgetch(stdscr, y, x);
}

int wgetnstr(WINDOW* win, char* str, int n) {
    int len = 0;

    if (!win || !str || n < 0) {
        return ERR;
    }

    while (len < n) {
        int ch = wgetch(win);
        if (ch == ERR) {
            return ERR;
        }
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if (ch == KEY_BACKSPACE || ch == 0x7f || ch == '\b') {
            if (len > 0) {
                len--;
            }
            continue;
        }
        if (ch >= 0 && ch < 256) {
            str[len++] = (char)ch;
        }
    }
    str[len] = '\0';
    return OK;
}

int wgetstr(WINDOW* win, char* str) {
    return wgetnstr(win, str, 255);
}

int getnstr(char* str, int n) {
    return wgetnstr(stdscr, str, n);
}

int getstr(char* str) {
    return wgetstr(stdscr, str);
}

int mvwgetnstr(WINDOW* win, int y, int x, char* str, int n) {
    if (wmove(win, y, x) != OK) {
        return ERR;
    }
    return wgetnstr(win, str, n);
}

int mvwgetstr(WINDOW* win, int y, int x, char* str) {
    return mvwgetnstr(win, y, x, str, 255);
}

int mvgetnstr(int y, int x, char* str, int n) {
    return mvwgetnstr(stdscr, y, x, str, n);
}

int mvgetstr(int y, int x, char* str) {
    return mvwgetstr(stdscr, y, x, str);
}

chtype winch(WINDOW* win) {
    (void)win;
    return (chtype)' ';
}

chtype inch(void) {
    return winch(stdscr);
}

chtype mvwinch(WINDOW* win, int y, int x) {
    if (wmove(win, y, x) != OK) {
        return (chtype)ERR;
    }
    return winch(win);
}

chtype mvinch(int y, int x) {
    return mvwinch(stdscr, y, x);
}

int wdelch(WINDOW* win) {
    if (!win) {
        return ERR;
    }
    curses_write("\x1b[P");
    return OK;
}

int delch(void) {
    return wdelch(stdscr);
}

int mvwdelch(WINDOW* win, int y, int x) {
    if (wmove(win, y, x) != OK) {
        return ERR;
    }
    return wdelch(win);
}

int mvdelch(int y, int x) {
    return mvwdelch(stdscr, y, x);
}

int winsch(WINDOW* win, chtype ch) {
    if (!win) {
        return ERR;
    }
    curses_write("\x1b[@");
    return waddch(win, ch);
}

int insch(chtype ch) {
    return winsch(stdscr, ch);
}

int mvwinsch(WINDOW* win, int y, int x, chtype ch) {
    if (wmove(win, y, x) != OK) {
        return ERR;
    }
    return winsch(win, ch);
}

int mvinsch(int y, int x, chtype ch) {
    return mvwinsch(stdscr, y, x, ch);
}

int ungetch(int ch) {
    if (g_unget != ERR) {
        return ERR;
    }
    g_unget = ch;
    return OK;
}

int flushinp(void) {
    g_unget = ERR;
    return OK;
}

WINDOW* newwin(int nlines, int ncols, int begin_y, int begin_x) {
    WINDOW* win = (WINDOW*)malloc(sizeof(WINDOW));
    if (!win) {
        return NULL;
    }
    win->begy = begin_y;
    win->begx = begin_x;
    win->rows = nlines > 0 ? nlines : LINES;
    win->cols = ncols > 0 ? ncols : COLS;
    win->cury = 0;
    win->curx = 0;
    win->delay_ms = -1;
    win->attrs = A_NORMAL;
    return win;
}

WINDOW* subwin(WINDOW* orig, int nlines, int ncols, int begin_y, int begin_x) {
    (void)orig;
    return newwin(nlines, ncols, begin_y, begin_x);
}

WINDOW* derwin(WINDOW* orig, int nlines, int ncols, int begin_y, int begin_x) {
    if (!orig) {
        return NULL;
    }
    return newwin(nlines, ncols, orig->begy + begin_y, orig->begx + begin_x);
}

int mvwin(WINDOW* win, int y, int x) {
    if (!win || y < 0 || x < 0) {
        return ERR;
    }
    win->begy = y;
    win->begx = x;
    return OK;
}

int delwin(WINDOW* win) {
    if (!win || win == stdscr) {
        return ERR;
    }
    free(win);
    return OK;
}

SCREEN* newterm(const char* type, FILE* outfd, FILE* infd) {
    (void)type;
    (void)outfd;
    (void)infd;
    return initscr() ? &g_screen : NULL;
}

SCREEN* set_term(SCREEN* screen) {
    return screen ? screen : &g_screen;
}

void delscreen(SCREEN* screen) {
    (void)screen;
}

int keypad(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int meta(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int nodelay(WINDOW* win, int bf) { if (win) win->delay_ms = bf ? 0 : -1; return OK; }
int wtimeout(WINDOW* win, int delay) { if (win) win->delay_ms = delay; return OK; }
void timeout(int delay) { (void)wtimeout(stdscr, delay); }
int halfdelay(int tenths) { (void)tenths; curses_apply_raw(1); return OK; }
int cbreak(void) { curses_apply_raw(1); return OK; }
int nocbreak(void) { return OK; }
int raw(void) { curses_apply_raw(1); return OK; }
int noraw(void) { curses_apply_raw(0); return OK; }
int echo(void) { return OK; }
int noecho(void) { return OK; }
int noqiflush(void) { return OK; }
void qiflush(void) {}
int nonl(void) { return OK; }
int nl(void) { return OK; }
int intrflush(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int scrollok(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int wscrl(WINDOW* win, int n) { (void)win; (void)n; return OK; }
int scrl(int n) { return wscrl(stdscr, n); }
int scroll(WINDOW* win) { return wscrl(win, 1); }
int idlok(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int idcok(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int leaveok(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int immedok(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int syncok(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }
int typeahead(int fd) { (void)fd; return OK; }
int curs_set(int visibility) { curses_write(visibility ? "\x1b[?25h" : "\x1b[?25l"); return 1; }
int baudrate(void) { return 115200; }
attr_t termattrs(void) { return A_BOLD | A_REVERSE | A_UNDERLINE; }
int napms(int ms) { if (ms > 0) (void)usleep((unsigned int)ms * 1000u); return OK; }
int beep(void) { curses_write("\a"); return OK; }
int flash(void) { return beep(); }
int start_color(void) { return OK; }
int has_colors(void) { return TRUE; }
int can_change_color(void) { return FALSE; }
int use_default_colors(void) { return OK; }
int init_color(short color, short r, short g, short b) {
    (void)r;
    (void)g;
    (void)b;
    return (color >= 0 && color < COLORS) ? OK : ERR;
}
int color_content(short color, short* r, short* g, short* b) {
    static const short table[8][3] = {
        { 0, 0, 0 },
        { 1000, 0, 0 },
        { 0, 1000, 0 },
        { 1000, 1000, 0 },
        { 0, 0, 1000 },
        { 1000, 0, 1000 },
        { 0, 1000, 1000 },
        { 1000, 1000, 1000 },
    };
    if (color < 0 || color >= COLORS) {
        return ERR;
    }
    if (r) {
        *r = table[color][0];
    }
    if (g) {
        *g = table[color][1];
    }
    if (b) {
        *b = table[color][2];
    }
    return OK;
}
int init_pair(short pair, short f, short b) { (void)pair; (void)f; (void)b; return OK; }
int pair_content(short pair, short* f, short* b) {
    if (pair < 0) {
        return ERR;
    }
    if (f) {
        *f = COLOR_WHITE;
    }
    if (b) {
        *b = COLOR_BLACK;
    }
    return OK;
}
int wattron(WINDOW* win, int attrs) { if (!win) return ERR; win->attrs |= (attr_t)attrs; return OK; }
int wattroff(WINDOW* win, int attrs) { if (!win) return ERR; win->attrs &= ~(attr_t)attrs; return OK; }
int wattrset(WINDOW* win, int attrs) { if (!win) return ERR; win->attrs = (attr_t)attrs; return OK; }
int wstandout(WINDOW* win) { return wattron(win, A_STANDOUT); }
int wstandend(WINDOW* win) { return wattroff(win, A_STANDOUT); }
int wattr_get(WINDOW* win, attr_t* attrs, short* pair, void* opts) {
    (void)opts;
    if (!win) {
        return ERR;
    }
    if (attrs) {
        *attrs = win->attrs;
    }
    if (pair) {
        *pair = 0;
    }
    return OK;
}
int wattr_set(WINDOW* win, attr_t attrs, short pair, void* opts) {
    (void)pair;
    (void)opts;
    if (!win) {
        return ERR;
    }
    win->attrs = attrs;
    return OK;
}
int attron(int attrs) { return wattron(stdscr, attrs); }
int attroff(int attrs) { return wattroff(stdscr, attrs); }
int attrset(int attrs) { return wattrset(stdscr, attrs); }
int standout(void) { return wstandout(stdscr); }
int standend(void) { return wstandend(stdscr); }
attr_t getattrs(WINDOW* win) { return win ? win->attrs : A_NORMAL; }
int def_prog_mode(void) { return OK; }
int reset_prog_mode(void) { curses_apply_raw(1); return OK; }

int wborder(WINDOW* win,
            chtype ls,
            chtype rs,
            chtype ts,
            chtype bs,
            chtype tl,
            chtype tr,
            chtype bl,
            chtype br) {
    char l = (char)(ls ? (ls & A_CHARTEXT) : '|');
    char r = (char)(rs ? (rs & A_CHARTEXT) : '|');
    char t = (char)(ts ? (ts & A_CHARTEXT) : '-');
    char b = (char)(bs ? (bs & A_CHARTEXT) : '-');
    char ctl = (char)(tl ? (tl & A_CHARTEXT) : '+');
    char ctr = (char)(tr ? (tr & A_CHARTEXT) : '+');
    char cbl = (char)(bl ? (bl & A_CHARTEXT) : '+');
    char cbr = (char)(br ? (br & A_CHARTEXT) : '+');

    if (!win) {
        return ERR;
    }

    for (int x = 0; x < win->cols; x++) {
        (void)wmove(win, 0, x);
        (void)waddch(win, (chtype)t);
        (void)wmove(win, win->rows - 1, x);
        (void)waddch(win, (chtype)b);
    }
    for (int y = 0; y < win->rows; y++) {
        (void)wmove(win, y, 0);
        (void)waddch(win, (chtype)l);
        (void)wmove(win, y, win->cols - 1);
        (void)waddch(win, (chtype)r);
    }
    (void)mvwaddch(win, 0, 0, (chtype)ctl);
    (void)mvwaddch(win, 0, win->cols - 1, (chtype)ctr);
    (void)mvwaddch(win, win->rows - 1, 0, (chtype)cbl);
    (void)mvwaddch(win, win->rows - 1, win->cols - 1, (chtype)cbr);
    return OK;
}

int border(chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br) {
    return wborder(stdscr, ls, rs, ts, bs, tl, tr, bl, br);
}

int box(WINDOW* win, chtype verch, chtype horch) {
    return wborder(win, verch, verch, horch, horch, 0, 0, 0, 0);
}

int touchwin(WINDOW* win) { (void)win; return OK; }
int untouchwin(WINDOW* win) { (void)win; return OK; }
int wtouchln(WINDOW* win, int y, int n, int changed) { (void)win; (void)y; (void)n; (void)changed; return OK; }
int touchline(WINDOW* win, int start, int count) { return wtouchln(win, start, count, 1); }
int is_wintouched(WINDOW* win) { (void)win; return FALSE; }
int is_linetouched(WINDOW* win, int line) { (void)win; (void)line; return FALSE; }
int redrawwin(WINDOW* win) { return touchwin(win); }
int redrawln(int beg_line, int num_lines) { return wredrawln(stdscr, beg_line, num_lines); }
int wredrawln(WINDOW* win, int beg_line, int num_lines) { (void)beg_line; (void)num_lines; return touchwin(win); }
int clearok(WINDOW* win, int bf) { (void)win; (void)bf; return OK; }

int is_term_resized(int lines, int columns) {
    return lines > 0 && columns > 0 && (lines != LINES || columns != COLS);
}

int resizeterm(int lines, int columns) {
    if (lines <= 0 || columns <= 0) {
        return ERR;
    }
    LINES = lines;
    COLS = columns;
    g_stdscr.rows = lines;
    g_stdscr.cols = columns;
    return OK;
}

int resize_term(int lines, int columns) {
    return resizeterm(lines, columns);
}

void use_env(int bf) {
    (void)bf;
}

void use_tioctl(int bf) {
    (void)bf;
}

int set_escdelay(int size) {
    if (size < 0) {
        return ERR;
    }
    g_escdelay = size;
    return OK;
}

int get_escdelay(void) {
    return g_escdelay;
}

char* keyname(int ch) {
    static char buf[24];

    if (ch > KEY_F0 && ch <= KEY_F(64)) {
        (void)snprintf(buf, sizeof(buf), "KEY_F(%d)", ch - KEY_F0);
        return buf;
    }

    switch (ch) {
        case KEY_UP: return "KEY_UP";
        case KEY_DOWN: return "KEY_DOWN";
        case KEY_LEFT: return "KEY_LEFT";
        case KEY_RIGHT: return "KEY_RIGHT";
        case KEY_HOME: return "KEY_HOME";
        case KEY_END: return "KEY_END";
        case KEY_BACKSPACE: return "KEY_BACKSPACE";
        case KEY_DC: return "KEY_DC";
        case KEY_NPAGE: return "KEY_NPAGE";
        case KEY_PPAGE: return "KEY_PPAGE";
        case KEY_RESIZE: return "KEY_RESIZE";
        case KEY_MOUSE: return "KEY_MOUSE";
        case '\n': return "^J";
        case '\t': return "^I";
        case 0x1b: return "^[";
        default:
            if (ch >= 0 && ch < 32) {
                (void)snprintf(buf, sizeof(buf), "^%c", ch + '@');
            } else if (ch >= 32 && ch < 127) {
                buf[0] = (char)ch;
                buf[1] = '\0';
            } else {
                (void)snprintf(buf, sizeof(buf), "%d", ch);
            }
            return buf;
    }
}

int key_defined(const char* definition) {
    (void)definition;
    return 0;
}

int define_key(const char* definition, int keycode) {
    (void)definition;
    (void)keycode;
    return OK;
}

int has_key(int ch) {
    if (ch > KEY_F0 && ch <= KEY_F(64)) {
        return TRUE;
    }

    switch (ch) {
        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_HOME:
        case KEY_END:
        case KEY_BACKSPACE:
        case KEY_DC:
        case KEY_NPAGE:
        case KEY_PPAGE:
        case KEY_RESIZE:
            return TRUE;
        default:
            return FALSE;
    }
}

unsigned long mousemask(unsigned long newmask, unsigned long* oldmask) {
    if (oldmask) {
        *oldmask = g_mousemask;
    }
    g_mousemask = newmask;
    return 0;
}

int getmouse(MEVENT* event) {
    (void)event;
    return ERR;
}

int ungetmouse(MEVENT* event) {
    (void)event;
    return ERR;
}

int getcurx(WINDOW* win) { return win ? win->curx : ERR; }
int getcury(WINDOW* win) { return win ? win->cury : ERR; }
int getbegx(WINDOW* win) { return win ? win->begx : ERR; }
int getbegy(WINDOW* win) { return win ? win->begy : ERR; }
int getmaxx(WINDOW* win) { return win ? win->cols : ERR; }
int getmaxy(WINDOW* win) { return win ? win->rows : ERR; }
