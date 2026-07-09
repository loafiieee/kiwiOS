#include <curses.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int rows = 0;
    int cols = 0;
    WINDOW* win = NULL;
    short r = 0;
    short g = 0;
    short b = 0;

    if (!initscr()) {
        puts("cursestest: FAIL initscr");
        return 1;
    }

    getmaxyx(stdscr, rows, cols);
    if (rows <= 0 || cols <= 0) {
        endwin();
        puts("cursestest: FAIL getmaxyx");
        return 2;
    }

    clear();
    mvaddstr(0, 0, "cursestest: top-left");
    attron(A_REVERSE);
    mvprintw(1, 0, "size %dx%d", rows, cols);
    attroff(A_REVERSE);
    if (standout() != OK || getattrs(stdscr) == A_NORMAL || standend() != OK) {
        endwin();
        puts("cursestest: FAIL standout");
        return 3;
    }
    if (!has_key(KEY_UP) || strcmp(keyname(KEY_UP), "KEY_UP") != 0 ||
        !has_key(KEY_F(1)) || strcmp(keyname(KEY_F(1)), "KEY_F(1)") != 0 ||
        set_escdelay(25) != OK || get_escdelay() != 25 ||
        termattrs() == A_NORMAL || baudrate() <= 0) {
        endwin();
        puts("cursestest: FAIL key/terminal helpers");
        return 4;
    }
    win = newwin(3, 24, 3, 0);
    if (!win) {
        endwin();
        puts("cursestest: FAIL newwin");
        return 5;
    }
    if (wstandout(win) != OK || wstandend(win) != OK ||
        touchline(win, 0, 1) != OK || redrawln(0, 1) != OK ||
        scroll(win) != OK || mvwinch(win, 0, 0) == (chtype)ERR ||
        mvwinsch(win, 1, 1, (chtype)'X') != OK ||
        mvwdelch(win, 1, 1) != OK ||
        init_color(COLOR_RED, 1000, 0, 0) != OK ||
        color_content(COLOR_GREEN, &r, &g, &b) != OK ||
        g <= r || g <= b) {
        delwin(win);
        endwin();
        puts("cursestest: FAIL window helpers");
        return 6;
    }
    box(win, 0, 0);
    mvwaddstr(win, 1, 2, "window draw ok");
    wrefresh(win);
    delwin(win);
    refresh();
    endwin();
    puts("cursestest: PASS curses shim");
    return 0;
}
