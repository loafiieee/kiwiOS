#ifndef KIWILIB_CURSES_H
#define KIWILIB_CURSES_H

#include <stdarg.h>
#include <stdio.h>

#define OK 0
#define ERR (-1)
#define TRUE 1
#define FALSE 0

#define KEY_CODE_YES 0400
#define KEY_DOWN 0402
#define KEY_UP 0403
#define KEY_LEFT 0404
#define KEY_RIGHT 0405
#define KEY_HOME 0406
#define KEY_BACKSPACE 0407
#define KEY_F0 0410
#define KEY_F(n) (KEY_F0 + (n))
#define KEY_DL 0510
#define KEY_IL 0511
#define KEY_DC 0512
#define KEY_IC 0513
#define KEY_EIC 0514
#define KEY_CLEAR 0515
#define KEY_EOS 0516
#define KEY_EOL 0517
#define KEY_SF 0520
#define KEY_SR 0521
#define KEY_NPAGE 0522
#define KEY_PPAGE 0523
#define KEY_STAB 0524
#define KEY_CTAB 0525
#define KEY_CATAB 0526
#define KEY_ENTER 0527
#define KEY_A1 0534
#define KEY_A3 0535
#define KEY_B2 0536
#define KEY_C1 0537
#define KEY_C3 0540
#define KEY_BTAB 0541
#define KEY_BEG 0542
#define KEY_CANCEL 0543
#define KEY_END 0550
#define KEY_SBEG 0572
#define KEY_SCANCEL 0573
#define KEY_SDC 0577
#define KEY_RESIZE 0632
#define KEY_SUSPEND 0627
#define KEY_EXIT 0551
#define KEY_SIC 0610
#define KEY_SLEFT 0611
#define KEY_SRIGHT 0622
#define KEY_SSUSPEND 0625
#define KEY_MOUSE 0631

#define ACS_HLINE ((chtype)'-')
#define ACS_VLINE ((chtype)'|')
#define ACS_ULCORNER ((chtype)'+')
#define ACS_URCORNER ((chtype)'+')
#define ACS_LLCORNER ((chtype)'+')
#define ACS_LRCORNER ((chtype)'+')
#define ACS_PLUS ((chtype)'+')
#define ACS_LTEE ((chtype)'+')
#define ACS_RTEE ((chtype)'+')
#define ACS_TTEE ((chtype)'+')
#define ACS_BTEE ((chtype)'+')

#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_MAGENTA 5
#define COLOR_CYAN 6
#define COLOR_WHITE 7

typedef unsigned int chtype;
typedef unsigned int attr_t;

#define A_NORMAL    0x00000000u
#define A_STANDOUT  0x00010000u
#define A_UNDERLINE 0x00020000u
#define A_REVERSE   0x00040000u
#define A_BLINK     0x00080000u
#define A_DIM       0x00100000u
#define A_BOLD      0x00200000u
#define A_ALTCHARSET 0x00400000u
#define A_CHARTEXT  0x000000ffu
#define A_ATTRIBUTES 0xffffff00u
#define COLOR_PAIR(n) (((attr_t)(n) & 0xffu) << 24)

typedef struct WINDOW WINDOW;
typedef struct SCREEN SCREEN;
typedef struct {
    short id;
    int x;
    int y;
    int z;
    unsigned long bstate;
} MEVENT;

extern WINDOW* stdscr;
extern WINDOW* curscr;
extern int LINES;
extern int COLS;
extern int COLORS;
extern int COLOR_PAIRS;

struct WINDOW {
    int begy;
    int begx;
    int rows;
    int cols;
    int cury;
    int curx;
    int delay_ms;
    attr_t attrs;
};

struct SCREEN {
    int placeholder;
};

#define getmaxyx(win, y, x) do { (y) = (win)->rows; (x) = (win)->cols; } while (0)
#define getyx(win, y, x) do { (y) = (win)->cury; (x) = (win)->curx; } while (0)
#define getbegyx(win, y, x) do { (y) = (win)->begy; (x) = (win)->begx; } while (0)
#define getparyx(win, y, x) do { (void)(win); (y) = -1; (x) = -1; } while (0)

WINDOW* initscr(void);
int endwin(void);
int isendwin(void);
int def_shell_mode(void);
int reset_shell_mode(void);
int refresh(void);
int wrefresh(WINDOW* win);
int wnoutrefresh(WINDOW* win);
int doupdate(void);
int clear(void);
int erase(void);
int wclear(WINDOW* win);
int werase(WINDOW* win);
int clrtobot(void);
int wclrtobot(WINDOW* win);
int clrtoeol(void);
int wclrtoeol(WINDOW* win);
int move(int y, int x);
int wmove(WINDOW* win, int y, int x);
int addch(chtype ch);
int waddch(WINDOW* win, chtype ch);
int mvaddch(int y, int x, chtype ch);
int mvwaddch(WINDOW* win, int y, int x, chtype ch);
int addstr(const char* str);
int addnstr(const char* str, int n);
int addbytes(const char* str, int n);
int waddstr(WINDOW* win, const char* str);
int waddnstr(WINDOW* win, const char* str, int n);
int waddbytes(WINDOW* win, const char* str, int n);
int mvaddstr(int y, int x, const char* str);
int mvwaddstr(WINDOW* win, int y, int x, const char* str);
int mvaddnstr(int y, int x, const char* str, int n);
int mvwaddnstr(WINDOW* win, int y, int x, const char* str, int n);
int printw(const char* fmt, ...);
int wprintw(WINDOW* win, const char* fmt, ...);
int mvprintw(int y, int x, const char* fmt, ...);
int mvwprintw(WINDOW* win, int y, int x, const char* fmt, ...);
int vwprintw(WINDOW* win, const char* fmt, va_list ap);
int getch(void);
int wgetch(WINDOW* win);
int mvgetch(int y, int x);
int mvwgetch(WINDOW* win, int y, int x);
int getstr(char* str);
int getnstr(char* str, int n);
int wgetstr(WINDOW* win, char* str);
int wgetnstr(WINDOW* win, char* str, int n);
int mvgetstr(int y, int x, char* str);
int mvgetnstr(int y, int x, char* str, int n);
int mvwgetstr(WINDOW* win, int y, int x, char* str);
int mvwgetnstr(WINDOW* win, int y, int x, char* str, int n);
chtype inch(void);
chtype winch(WINDOW* win);
chtype mvinch(int y, int x);
chtype mvwinch(WINDOW* win, int y, int x);
int delch(void);
int wdelch(WINDOW* win);
int mvdelch(int y, int x);
int mvwdelch(WINDOW* win, int y, int x);
int insch(chtype ch);
int winsch(WINDOW* win, chtype ch);
int mvinsch(int y, int x, chtype ch);
int mvwinsch(WINDOW* win, int y, int x, chtype ch);
int ungetch(int ch);
int flushinp(void);
WINDOW* newwin(int nlines, int ncols, int begin_y, int begin_x);
WINDOW* subwin(WINDOW* orig, int nlines, int ncols, int begin_y, int begin_x);
WINDOW* derwin(WINDOW* orig, int nlines, int ncols, int begin_y, int begin_x);
int mvwin(WINDOW* win, int y, int x);
int delwin(WINDOW* win);
SCREEN* newterm(const char* type, FILE* outfd, FILE* infd);
SCREEN* set_term(SCREEN* screen);
void delscreen(SCREEN* screen);
int keypad(WINDOW* win, int bf);
int meta(WINDOW* win, int bf);
int nodelay(WINDOW* win, int bf);
int wtimeout(WINDOW* win, int delay);
void timeout(int delay);
int halfdelay(int tenths);
int cbreak(void);
int nocbreak(void);
int raw(void);
int noraw(void);
int echo(void);
int noecho(void);
int noqiflush(void);
void qiflush(void);
int nonl(void);
int nl(void);
int intrflush(WINDOW* win, int bf);
int scrollok(WINDOW* win, int bf);
int scroll(WINDOW* win);
int scrl(int n);
int wscrl(WINDOW* win, int n);
int idlok(WINDOW* win, int bf);
int idcok(WINDOW* win, int bf);
int leaveok(WINDOW* win, int bf);
int immedok(WINDOW* win, int bf);
int syncok(WINDOW* win, int bf);
int typeahead(int fd);
int curs_set(int visibility);
int baudrate(void);
attr_t termattrs(void);
int napms(int ms);
int beep(void);
int flash(void);
int start_color(void);
int has_colors(void);
int can_change_color(void);
int use_default_colors(void);
int init_color(short color, short r, short g, short b);
int color_content(short color, short* r, short* g, short* b);
int init_pair(short pair, short f, short b);
int pair_content(short pair, short* f, short* b);
int wattron(WINDOW* win, int attrs);
int wattroff(WINDOW* win, int attrs);
int wattrset(WINDOW* win, int attrs);
int wstandout(WINDOW* win);
int wstandend(WINDOW* win);
int wattr_get(WINDOW* win, attr_t* attrs, short* pair, void* opts);
int wattr_set(WINDOW* win, attr_t attrs, short pair, void* opts);
int attron(int attrs);
int attroff(int attrs);
int attrset(int attrs);
int standout(void);
int standend(void);
attr_t getattrs(WINDOW* win);
int box(WINDOW* win, chtype verch, chtype horch);
int wborder(WINDOW* win, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br);
int border(chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br);
int touchwin(WINDOW* win);
int untouchwin(WINDOW* win);
int wtouchln(WINDOW* win, int y, int n, int changed);
int touchline(WINDOW* win, int start, int count);
int is_wintouched(WINDOW* win);
int is_linetouched(WINDOW* win, int line);
int redrawwin(WINDOW* win);
int redrawln(int beg_line, int num_lines);
int wredrawln(WINDOW* win, int beg_line, int num_lines);
int clearok(WINDOW* win, int bf);
int is_term_resized(int lines, int columns);
int resizeterm(int lines, int columns);
int resize_term(int lines, int columns);
void use_env(int bf);
void use_tioctl(int bf);
int set_escdelay(int size);
int get_escdelay(void);
char* keyname(int ch);
int key_defined(const char* definition);
int define_key(const char* definition, int keycode);
int setupterm(char* term, int fd, int* errret);
char* tigetstr(const char* capname);
int tigetnum(const char* capname);
int tigetflag(const char* capname);
char* tparm(const char* str, ...);
int has_key(int ch);
unsigned long mousemask(unsigned long newmask, unsigned long* oldmask);
int getmouse(MEVENT* event);
int ungetmouse(MEVENT* event);
int getcurx(WINDOW* win);
int getcury(WINDOW* win);
int getbegx(WINDOW* win);
int getbegy(WINDOW* win);
int getmaxx(WINDOW* win);
int getmaxy(WINDOW* win);
int def_prog_mode(void);
int reset_prog_mode(void);

#endif // KIWILIB_CURSES_H
