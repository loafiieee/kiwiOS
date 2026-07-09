#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define EDIT_BUF_MAX 32768u
#define DRAW_BUF_MAX 32768u
#define EDIT_MAX_ROWS 64
#define EDIT_MAX_COLS 160

enum {
    KEY_NONE = 0,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_DELETE,
    KEY_HOME,
    KEY_END,
};

static char g_buf[EDIT_BUF_MAX];
static size_t g_len;
static size_t g_cursor;
static int g_dirty;
static int g_rows = 24;
static int g_cols = 80;
static int g_view_line;
static struct termios g_orig_termios;
static int g_raw_enabled;
static char g_draw_buf[DRAW_BUF_MAX];
static size_t g_draw_len;

static int write_all(const char* s, size_t len) {
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, s, len);
        if (n <= 0) {
            return -1;
        }
        s += n;
        len -= (size_t)n;
    }
    return 0;
}

static void write_str(const char* s) {
    (void)write_all(s, strlen(s));
}

static void draw_reset(void) {
    g_draw_len = 0;
}

static void draw_append(const char* s, size_t len) {
    if (!s || len == 0) {
        return;
    }
    if (len > DRAW_BUF_MAX - g_draw_len) {
        len = DRAW_BUF_MAX - g_draw_len;
    }
    if (len == 0) {
        return;
    }
    memcpy(g_draw_buf + g_draw_len, s, len);
    g_draw_len += len;
}

static void draw_append_str(const char* s) {
    draw_append(s, strlen(s));
}

static void draw_flush(void) {
    (void)write_all(g_draw_buf, g_draw_len);
    g_draw_len = 0;
}

static void disable_raw(void) {
    if (g_raw_enabled) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_raw_enabled = 0;
    }
}

static int enable_raw(void) {
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) {
        return -1;
    }
    raw = g_orig_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return -1;
    }
    g_raw_enabled = 1;
    return 0;
}

static void refresh_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 10) {
        g_rows = ws.ws_row;
        g_cols = ws.ws_col;
        if (g_rows > EDIT_MAX_ROWS) {
            g_rows = EDIT_MAX_ROWS;
        }
        if (g_cols > EDIT_MAX_COLS) {
            g_cols = EDIT_MAX_COLS;
        }
    }
}

static size_t line_start_for(int target_line) {
    int line = 0;
    size_t i = 0;

    while (i < g_len && line < target_line) {
        if (g_buf[i++] == '\n') {
            line++;
        }
    }
    return i;
}

static int cursor_line_col(int* col_out) {
    int line = 0;
    int col = 0;

    for (size_t i = 0; i < g_cursor && i < g_len; i++) {
        if (g_buf[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }

    if (col_out) {
        *col_out = col;
    }
    return line;
}

static size_t line_end_from(size_t start) {
    size_t i = start;
    while (i < g_len && g_buf[i] != '\n') {
        i++;
    }
    return i;
}

static void move_vertical(int delta) {
    int col = 0;
    int line = cursor_line_col(&col);
    int target = line + delta;
    size_t start;
    size_t end;
    size_t width;

    if (target < 0) {
        target = 0;
    }

    start = line_start_for(target);
    end = line_end_from(start);
    width = end - start;
    if ((size_t)col > width) {
        col = (int)width;
    }
    g_cursor = start + (size_t)col;
}

static void insert_char(char c) {
    if (g_len + 1u >= EDIT_BUF_MAX) {
        return;
    }
    memmove(g_buf + g_cursor + 1u, g_buf + g_cursor, g_len - g_cursor);
    g_buf[g_cursor++] = c;
    g_len++;
    g_dirty = 1;
}

static void delete_before_cursor(void) {
    if (g_cursor == 0) {
        return;
    }
    memmove(g_buf + g_cursor - 1u, g_buf + g_cursor, g_len - g_cursor);
    g_cursor--;
    g_len--;
    g_dirty = 1;
}

static void delete_at_cursor(void) {
    if (g_cursor >= g_len) {
        return;
    }
    memmove(g_buf + g_cursor, g_buf + g_cursor + 1u, g_len - g_cursor - 1u);
    g_len--;
    g_dirty = 1;
}

static int read_key(void) {
    unsigned char c;

    if (read(STDIN_FILENO, &c, 1) != 1) {
        return KEY_NONE;
    }

    if (c != 0x1b) {
        return c;
    }

    if (read(STDIN_FILENO, &c, 1) != 1 || c != '[') {
        return 0x1b;
    }
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return 0x1b;
    }

    if (c >= '0' && c <= '9') {
        unsigned char tail = 0;
        if (read(STDIN_FILENO, &tail, 1) != 1) {
            return KEY_NONE;
        }
        if (c == '3' && tail == '~') {
            return KEY_DELETE;
        }
        return KEY_NONE;
    }

    if (c == 'A') return KEY_UP;
    if (c == 'B') return KEY_DOWN;
    if (c == 'C') return KEY_RIGHT;
    if (c == 'D') return KEY_LEFT;
    if (c == 'H') return KEY_HOME;
    if (c == 'F') return KEY_END;
    return KEY_NONE;
}

static int load_file(const char* path) {
    int fd = open(path, O_RDONLY);
    ssize_t n;

    g_len = 0;
    g_cursor = 0;
    g_dirty = 0;

    if (fd < 0) {
        return 0;
    }

    while (g_len < EDIT_BUF_MAX) {
        n = read(fd, g_buf + g_len, EDIT_BUF_MAX - g_len);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        g_len += (size_t)n;
    }

    close(fd);
    return 0;
}

static int save_file(const char* path) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    size_t done = 0;

    if (fd < 0) {
        return -1;
    }

    while (done < g_len) {
        ssize_t n = write(fd, g_buf + done, g_len - done);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }

    close(fd);
    g_dirty = 0;
    return 0;
}

static void draw_screen(const char* path, const char* message) {
    int col = 0;
    int cursor_line = cursor_line_col(&col);
    int text_rows = g_rows - 2;
    char tmp[128];

    if (cursor_line < g_view_line) {
        g_view_line = cursor_line;
    }
    if (cursor_line >= g_view_line + text_rows) {
        g_view_line = cursor_line - text_rows + 1;
    }

    draw_reset();
    draw_append_str("\x1b[?25l\x1b[H");
    for (int row = 0; row < text_rows; row++) {
        size_t start = line_start_for(g_view_line + row);
        size_t end = line_end_from(start);
        size_t len = end > start ? end - start : 0;

        draw_append_str("\x1b[K");
        if (start < g_len) {
            if (len > (size_t)g_cols) {
                len = (size_t)g_cols;
            }
            draw_append(g_buf + start, len);
        } else {
            draw_append_str("~");
        }
        draw_append_str("\r\n");
    }

    snprintf(tmp, sizeof(tmp), "\x1b[7m %.40s %s | Ctrl+S save Ctrl+Q quit \x1b[m",
             path, g_dirty ? "[modified]" : "");
    draw_append_str("\x1b[K");
    draw_append_str(tmp);
    draw_append_str("\r\n\x1b[K");
    draw_append_str(message ? message : "");

    snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH\x1b[?25h",
             cursor_line - g_view_line + 1,
             col + 1 > g_cols ? g_cols : col + 1);
    draw_append_str(tmp);
    draw_flush();
}

int main(int argc, char** argv) {
    char path[PATH_MAX];
    const char* message = "";

    if (argc < 2) {
        puts("usage: edit <file>");
        return 1;
    }

    strncpy(path, argv[1], sizeof(path));
    path[sizeof(path) - 1u] = '\0';

    if (load_file(path) != 0) {
        puts("edit: failed to read file");
        return 1;
    }
    if (enable_raw() != 0) {
        puts("edit: failed to enter raw mode");
        return 1;
    }

    refresh_size();
    for (;;) {
        int key;

        draw_screen(path, message);
        message = "";
        key = read_key();

        if (key == 0x11) {
            break;
        } else if (key == 0x13) {
            message = save_file(path) == 0 ? "saved" : "save failed";
        } else if (key == KEY_LEFT) {
            if (g_cursor > 0) g_cursor--;
        } else if (key == KEY_RIGHT) {
            if (g_cursor < g_len) g_cursor++;
        } else if (key == KEY_UP) {
            move_vertical(-1);
        } else if (key == KEY_DOWN) {
            move_vertical(1);
        } else if (key == KEY_HOME) {
            g_cursor = line_start_for(cursor_line_col(NULL));
        } else if (key == KEY_END) {
            g_cursor = line_end_from(line_start_for(cursor_line_col(NULL)));
        } else if (key == KEY_DELETE) {
            delete_at_cursor();
        } else if (key == 127 || key == '\b') {
            delete_before_cursor();
        } else if (key == '\r' || key == '\n') {
            insert_char('\n');
        } else if (key >= 32 && key < 127) {
            insert_char((char)key);
        }
    }

    disable_raw();
    write_str("\x1b[?25h\x1b[m\r\n");
    return 0;
}
