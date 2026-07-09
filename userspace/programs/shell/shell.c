#include <stddef.h>
#include <stdint.h>
#include "glob.h"
#include "kiwi_syscall.h"
#include "string.h"

#define INPUT_BUFFER_SIZE 256u
#define PATH_BUFFER_SIZE 256u
#define IO_BUFFER_SIZE 256u
#define HISTORY_SIZE 32u
#define PROGRAM_ARG_MAX 32u

enum {
    SHELL_KEY_ARROW_UP = 0x100,
    SHELL_KEY_ARROW_DOWN,
    SHELL_KEY_ARROW_LEFT,
    SHELL_KEY_ARROW_RIGHT,
};

static char g_cwd[PATH_BUFFER_SIZE] = "/";
static char g_history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int g_history_count = 0;
static int g_history_cursor = -1;
static char g_history_scratch[INPUT_BUFFER_SIZE];
static int g_history_scratch_len = 0;

static void write_bytes(const char* s, size_t len) {
    if (s && len != 0) {
        (void)sys_write(1, s, (uint64_t)len);
    }
}

static void write_str(const char* s) {
    if (s) {
        write_bytes(s, strlen(s));
    }
}

static void write_line(const char* s) {
    write_str(s);
    write_str("\n");
}

static char* skip_spaces(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void trim_trailing_spaces(char* s) {
    size_t len = 0;

    if (!s) {
        return;
    }

    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        len--;
        s[len] = '\0';
    }
}

static int streq(const char* a, const char* b) {
    size_t i = 0;

    if (!a || !b) {
        return 0;
    }

    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }

    return a[i] == b[i];
}

static int build_prompt(char* out, size_t out_size) {
    static const char prefix[] = "kiwi:";
    static const char suffix[] = "$ ";
    size_t prefix_len = sizeof(prefix) - 1u;
    size_t cwd_len = strlen(g_cwd);
    size_t suffix_len = sizeof(suffix) - 1u;

    if (!out || out_size == 0) {
        return -1;
    }

    if (prefix_len + cwd_len + suffix_len + 1u > out_size) {
        return -1;
    }

    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, g_cwd, cwd_len);
    memcpy(out + prefix_len + cwd_len, suffix, suffix_len);
    out[prefix_len + cwd_len + suffix_len] = '\0';
    return 0;
}

static void redraw_input_line(const char* text, int text_len, int cursor_pos, int show_cursor) {
    char prompt[PATH_BUFFER_SIZE + 16u];

    if (!text || text_len < 0 || cursor_pos < 0) {
        return;
    }

    if (build_prompt(prompt, sizeof(prompt)) != 0) {
        return;
    }

    (void)sys_console_input(prompt,
                            text,
                            (uint64_t)(uint32_t)text_len,
                            (uint64_t)(uint32_t)cursor_pos,
                            show_cursor);
}

static void history_record(const char* line) {
    size_t len = 0;
    int slot = 0;

    if (!line || !*line) {
        return;
    }

    if (g_history_count > 0) {
        const char* last = g_history[(g_history_count - 1) % HISTORY_SIZE];
        if (streq(last, line)) {
            return;
        }
    }

    len = strlen(line);
    if (len > INPUT_BUFFER_SIZE - 1u) {
        len = INPUT_BUFFER_SIZE - 1u;
    }

    slot = g_history_count % HISTORY_SIZE;
    memcpy(g_history[slot], line, len);
    g_history[slot][len] = '\0';
    g_history_count++;
}

static void reset_history_navigation(void) {
    g_history_cursor = -1;
    g_history_scratch_len = 0;
    g_history_scratch[0] = '\0';
}

static const char* history_fetch(int cursor_from_newest) {
    int logical = 0;

    if (cursor_from_newest < 0 || cursor_from_newest >= g_history_count) {
        return NULL;
    }

    logical = g_history_count - 1 - cursor_from_newest;
    return g_history[logical % HISTORY_SIZE];
}

static void replace_input_line(char* buffer, int* len, int* cursor, const char* text) {
    size_t text_len = 0;

    if (!buffer || !len || !cursor || !text) {
        return;
    }

    text_len = strlen(text);
    if (text_len > INPUT_BUFFER_SIZE - 1u) {
        text_len = INPUT_BUFFER_SIZE - 1u;
    }

    memcpy(buffer, text, text_len);
    buffer[text_len] = '\0';
    *len = (int)text_len;
    *cursor = (int)text_len;
    redraw_input_line(buffer, *len, *cursor, 1);
}

static int read_key(void) {
    unsigned char ch = 0;

    if (sys_read(0, &ch, 1) != 1) {
        return -1;
    }

    if (ch != 0x1Bu) {
        return (int)ch;
    }

    if (sys_read(0, &ch, 1) != 1) {
        return 0x1B;
    }
    if (ch != '[') {
        return 0x1B;
    }
    if (sys_read(0, &ch, 1) != 1) {
        return 0x1B;
    }

    switch (ch) {
        case 'A':
            return SHELL_KEY_ARROW_UP;
        case 'B':
            return SHELL_KEY_ARROW_DOWN;
        case 'C':
            return SHELL_KEY_ARROW_RIGHT;
        case 'D':
            return SHELL_KEY_ARROW_LEFT;
        default:
            return 0x1B;
    }
}

static char* next_arg(char** cursor) {
    char* s = NULL;
    char* start = NULL;
    char* out = NULL;
    char quote = '\0';

    if (!cursor || !*cursor) {
        return NULL;
    }

    s = skip_spaces(*cursor);
    if (*s == '\0') {
        *cursor = s;
        return NULL;
    }

    start = s;
    out = s;
    while (*s) {
        if (quote) {
            if (*s == quote) {
                quote = '\0';
                s++;
                continue;
            }
            if (quote == '"' && *s == '\\' && s[1] != '\0') {
                s++;
            }
            *out++ = *s++;
            continue;
        }

        if (*s == ' ' || *s == '\t') {
            break;
        }

        if (*s == '"' || *s == '\'') {
            quote = *s++;
            continue;
        }

        if (*s == '\\' && s[1] != '\0') {
            s++;
        }

        *out++ = *s++;
    }

    if (*s != '\0') {
        s++;
    }
    *out = '\0';

    *cursor = s;
    return start;
}

static int has_extra_args(char* cursor) {
    return cursor && *skip_spaces(cursor) != '\0';
}

static int path_has_slash(const char* s) {
    if (!s) {
        return 0;
    }

    while (*s) {
        if (*s == '/') {
            return 1;
        }
        s++;
    }

    return 0;
}

static void path_pop_component(char* path) {
    size_t len = 0;

    if (!path) {
        return;
    }

    len = strlen(path);
    if (len <= 1) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    while (len > 1 && path[len - 1] != '/') {
        len--;
    }

    if (len <= 1) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    path[len - 1] = '\0';
}

static int path_append_component(char* path, size_t out_size, const char* comp, size_t comp_len) {
    size_t cur_len = 0;

    if (!path || !comp || comp_len == 0) {
        return 0;
    }

    if (comp_len == 1 && comp[0] == '.') {
        return 0;
    }

    if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') {
        path_pop_component(path);
        return 0;
    }

    cur_len = strlen(path);
    if (cur_len == 0) {
        if (out_size < 2) {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
        cur_len = 1;
    }

    if (cur_len > 1) {
        if (cur_len + 1 >= out_size) {
            return -1;
        }
        path[cur_len++] = '/';
        path[cur_len] = '\0';
    }

    if (cur_len + comp_len + 1 > out_size) {
        return -1;
    }

    memcpy(path + cur_len, comp, comp_len);
    path[cur_len + comp_len] = '\0';
    return 0;
}

static int resolve_path_arg(const char* arg, char* out, size_t out_size) {
    const char* cur = NULL;

    if (!arg || !*arg || !out || out_size < 2) {
        return -1;
    }

    if (arg[0] == '/') {
        out[0] = '/';
        out[1] = '\0';
        cur = arg + 1;
    } else {
        size_t cwd_len = strlen(g_cwd);
        if (cwd_len + 1 > out_size) {
            return -1;
        }
        memcpy(out, g_cwd, cwd_len + 1);
        cur = arg;
    }

    while (*cur) {
        const char* comp = NULL;
        size_t comp_len = 0;

        while (*cur == '/') {
            cur++;
        }

        if (*cur == '\0') {
            break;
        }

        comp = cur;
        while (*cur && *cur != '/') {
            cur++;
        }
        comp_len = (size_t)(cur - comp);

        if (path_append_component(out, out_size, comp, comp_len) != 0) {
            return -1;
        }
    }

    return 0;
}

static int resolve_program_path(const char* cmd, char* out, size_t out_size) {
    kiwi_stat_t st;
    const char* root_prefix = "/";
    const char* bin_prefix = "/bin/";
    size_t cmd_len = 0;
    size_t prefix_len = 0;

    if (!cmd || !*cmd || !out || out_size < 2) {
        return -1;
    }

    if (cmd[0] == '/' || path_has_slash(cmd)) {
        return resolve_path_arg(cmd, out, out_size);
    }

    cmd_len = strlen(cmd);

    prefix_len = strlen(bin_prefix);
    if (prefix_len + cmd_len + 1 <= out_size) {
        memcpy(out, bin_prefix, prefix_len);
        memcpy(out + prefix_len, cmd, cmd_len + 1);
        memset(&st, 0, sizeof(st));
        if (sys_stat(out, &st) == 0 && st.type == KIWI_VNODE_FILE) {
            return 0;
        }
    }

    prefix_len = strlen(root_prefix);
    if (prefix_len + cmd_len + 1 <= out_size) {
        memcpy(out, root_prefix, prefix_len);
        memcpy(out + prefix_len, cmd, cmd_len + 1);
        memset(&st, 0, sizeof(st));
        if (sys_stat(out, &st) == 0 && st.type == KIWI_VNODE_FILE) {
            return 0;
        }
    }

    return -1;
}

static int shell_arg_has_glob_magic(const char* s) {
    for (; s && *s; s++) {
        if (*s == '*' || *s == '?' || *s == '[') {
            return 1;
        }
    }
    return 0;
}

static int append_program_arg(const char** argv,
                              int* argc,
                              char expanded[PROGRAM_ARG_MAX][PATH_BUFFER_SIZE],
                              int* expanded_count,
                              const char* arg) {
    glob_t g;

    if (!argv || !argc || !expanded || !expanded_count || !arg) {
        return -1;
    }

    if (!shell_arg_has_glob_magic(arg)) {
        if (*argc >= (int)PROGRAM_ARG_MAX) {
            return -1;
        }
        argv[(*argc)++] = arg;
        return 0;
    }

    memset(&g, 0, sizeof(g));
    if (glob(arg, 0, NULL, &g) != 0) {
        globfree(&g);
        if (*argc >= (int)PROGRAM_ARG_MAX) {
            return -1;
        }
        argv[(*argc)++] = arg;
        return 0;
    }

    for (size_t i = 0; i < g.gl_pathc; i++) {
        size_t len = strlen(g.gl_pathv[i]);
        if (*argc >= (int)PROGRAM_ARG_MAX || *expanded_count >= (int)PROGRAM_ARG_MAX ||
            len + 1u > PATH_BUFFER_SIZE) {
            globfree(&g);
            return -1;
        }
        memcpy(expanded[*expanded_count], g.gl_pathv[i], len + 1u);
        argv[(*argc)++] = expanded[*expanded_count];
        (*expanded_count)++;
    }

    globfree(&g);
    return 0;
}

static int stat_path(const char* path, kiwi_stat_t* out) {
    if (!path || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    return (sys_stat(path, out) == 0) ? 0 : -1;
}

static int path_is_dir(const char* path) {
    kiwi_stat_t st;

    return stat_path(path, &st) == 0 && st.type == KIWI_VNODE_DIR;
}

static void init_cwd(void) {
    static const char home_dir[] = "/home";

    if (path_is_dir(home_dir)) {
        memcpy(g_cwd, home_dir, sizeof(home_dir));
        (void)sys_chdir(g_cwd);
        return;
    }

    g_cwd[0] = '/';
    g_cwd[1] = '\0';
    (void)sys_chdir(g_cwd);
}

static void show_help(void) {
    write_line("Built-in: help cd exit");
    write_line("Programs (/bin): echo clear pwd ls stat cat touch mkdir rmdir rm cp mv mount rescan which");
    write_line("Programs (/bin): date poweroff reboot shutdown");
    write_line("Usage notes: rm supports -r, -f, *, and path/*; mount takes <device> [path].");
}

static void cmd_cd(char* rest) {
    char path[PATH_BUFFER_SIZE];
    kiwi_stat_t st;
    char* arg = next_arg(&rest);

    if (!arg) {
        if (path_is_dir("/home")) {
            memcpy(path, "/home", sizeof("/home"));
        } else {
            path[0] = '/';
            path[1] = '\0';
        }
    } else if (resolve_path_arg(arg, path, sizeof(path)) != 0 || has_extra_args(rest)) {
        write_line("usage: cd [path]");
        return;
    }

    if (stat_path(path, &st) != 0) {
        write_line("shell: cd failed");
        return;
    }
    if (st.type != KIWI_VNODE_DIR) {
        write_line("shell: cd: not a directory");
        return;
    }

    if (sys_chdir(path) != 0) {
        write_line("shell: cd failed");
        return;
    }

    memcpy(g_cwd, path, strlen(path) + 1);
}

/* Returns 0 when an external program was found and executed (or its own
 * startup error was reported); returns -1 when the command was not found in
 * PATH, so the caller can fall back to a shell built-in. */
static int run_program(const char* cmd, char* rest) {
    char path[PATH_BUFFER_SIZE];
    const char* argv[PROGRAM_ARG_MAX];
    char expanded[PROGRAM_ARG_MAX][PATH_BUFFER_SIZE];
    int expanded_count = 0;
    char* arg = NULL;
    int argc = 0;
    int status = 0;
    int64_t pid = 0;

    if (resolve_program_path(cmd, path, sizeof(path)) != 0) {
        return -1;
    }

    argv[argc++] = cmd;
    while ((arg = next_arg(&rest)) != NULL) {
        if (append_program_arg(argv, &argc, expanded, &expanded_count, arg) != 0) {
            write_line("shell: too many program arguments");
            return 0;
        }
    }

    pid = sys_spawn_argv(path, argc, argv);
    if (pid < 0) {
        write_str("shell: failed to start ");
        write_str(path);
        write_str("\n");
        return 0;
    }

    if (sys_waitpid((int)pid, &status) < 0) {
        write_str("shell: wait failed for ");
        write_str(path);
        write_str("\n");
    }
    return 0;
}

int main(void) {
    char line[INPUT_BUFFER_SIZE];
    int input_len = 0;
    int cursor_pos = 0;

    init_cwd();
    write_line("Kiwi userspace shell");
    write_line("Type 'help' for commands.");
    line[0] = '\0';
    redraw_input_line(line, 0, 0, 1);

    for (;;) {
        int c = read_key();
        char* cursor = NULL;
        char* cmd = NULL;

        if (c < 0) {
            write_line("shell: stdin read failed");
            return 1;
        }

        if (c == SHELL_KEY_ARROW_UP) {
            if (g_history_cursor == -1) {
                g_history_scratch_len = input_len;
                if (g_history_scratch_len > (int)(INPUT_BUFFER_SIZE - 1u)) {
                    g_history_scratch_len = (int)(INPUT_BUFFER_SIZE - 1u);
                }
                memcpy(g_history_scratch, line, (size_t)g_history_scratch_len);
                g_history_scratch[g_history_scratch_len] = '\0';
            }

            if (g_history_cursor + 1 < g_history_count) {
                const char* entry = NULL;
                g_history_cursor++;
                entry = history_fetch(g_history_cursor);
                if (entry) {
                    replace_input_line(line, &input_len, &cursor_pos, entry);
                }
            }
            continue;
        }

        if (c == SHELL_KEY_ARROW_DOWN) {
            if (g_history_cursor > 0) {
                const char* entry = NULL;
                g_history_cursor--;
                entry = history_fetch(g_history_cursor);
                if (entry) {
                    replace_input_line(line, &input_len, &cursor_pos, entry);
                }
            } else if (g_history_cursor == 0) {
                g_history_cursor = -1;
                replace_input_line(line, &input_len, &cursor_pos, g_history_scratch);
            }
            continue;
        }

        if (c == SHELL_KEY_ARROW_LEFT) {
            if (cursor_pos > 0) {
                cursor_pos--;
                redraw_input_line(line, input_len, cursor_pos, 1);
            }
            continue;
        }

        if (c == SHELL_KEY_ARROW_RIGHT) {
            if (cursor_pos < input_len) {
                cursor_pos++;
                redraw_input_line(line, input_len, cursor_pos, 1);
            }
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == 1) {
            cursor_pos = 0;
            redraw_input_line(line, input_len, cursor_pos, 1);
            continue;
        }

        if (c == 3) {
            redraw_input_line(line, input_len, cursor_pos, 0);
            write_str("^C\n");
            input_len = 0;
            cursor_pos = 0;
            line[0] = '\0';
            reset_history_navigation();
            redraw_input_line(line, 0, 0, 1);
            continue;
        }

        if (c == 4) {
            if (input_len == 0) {
                redraw_input_line(line, input_len, cursor_pos, 0);
                write_str("\n");
                return 0;
            }
            continue;
        }

        if (c == 5) {
            cursor_pos = input_len;
            redraw_input_line(line, input_len, cursor_pos, 1);
            continue;
        }

        if (c == 21) {
            input_len = 0;
            cursor_pos = 0;
            line[0] = '\0';
            reset_history_navigation();
            redraw_input_line(line, input_len, cursor_pos, 1);
            continue;
        }

        if (c == '\n') {
            char* raw = NULL;
            char raw_for_history[INPUT_BUFFER_SIZE];

            redraw_input_line(line, input_len, input_len, 0);
            write_str("\n");
            line[input_len] = '\0';
            trim_trailing_spaces(line);
            raw = skip_spaces(line);
            memcpy(raw_for_history, raw, strlen(raw) + 1);

            cursor = raw;
            cmd = next_arg(&cursor);
            if (cmd && *cmd != '\0') {
                history_record(raw_for_history);

                /* cd/exit/help are the only built-ins (they change shell state
                 * or are shell-specific). Every other command runs from /bin. */
                if (streq(cmd, "help")) {
                    show_help();
                } else if (streq(cmd, "cd")) {
                    cmd_cd(cursor);
                } else if (streq(cmd, "exit")) {
                    return 0;
                } else if (run_program(cmd, cursor) != 0) {
                    write_line("shell: command not found");
                }
            }

            input_len = 0;
            cursor_pos = 0;
            line[0] = '\0';
            reset_history_navigation();
            redraw_input_line(line, 0, 0, 1);
            continue;
        }

        if (c == '\b') {
            if (cursor_pos > 0) {
                memmove(&line[cursor_pos - 1],
                        &line[cursor_pos],
                        (size_t)(input_len - cursor_pos));
                input_len--;
                cursor_pos--;
                line[input_len] = '\0';
                redraw_input_line(line, input_len, cursor_pos, 1);
            }
            continue;
        }

        if (c == 12) {
            (void)sys_console_clear();
            redraw_input_line(line, input_len, cursor_pos, 1);
            continue;
        }

        if (c >= 32 && c <= 126 && input_len < (int)(INPUT_BUFFER_SIZE - 1u)) {
            memmove(&line[cursor_pos + 1],
                    &line[cursor_pos],
                    (size_t)(input_len - cursor_pos));
            line[cursor_pos] = (char)c;
            input_len++;
            cursor_pos++;
            line[input_len] = '\0';
            redraw_input_line(line, input_len, cursor_pos, 1);
        }
    }
}
