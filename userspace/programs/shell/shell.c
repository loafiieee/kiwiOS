#include <stddef.h>
#include <stdint.h>
#include "kiwi_syscall.h"
#include "string.h"

#define INPUT_BUFFER_SIZE 256u
#define PATH_BUFFER_SIZE 256u
#define IO_BUFFER_SIZE 256u
#define HISTORY_SIZE 32u

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

static void write_u64(uint64_t value) {
    char buf[32];
    size_t i = 0;

    if (value == 0) {
        write_str("0");
        return;
    }

    while (value != 0 && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (i > 0) {
        i--;
        write_bytes(&buf[i], 1);
    }
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

    if (!cursor || !*cursor) {
        return NULL;
    }

    s = skip_spaces(*cursor);
    if (*s == '\0') {
        *cursor = s;
        return NULL;
    }

    start = s;
    while (*s && *s != ' ' && *s != '\t') {
        s++;
    }

    if (*s != '\0') {
        *s++ = '\0';
    }

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

static const char* path_basename(const char* path) {
    const char* last = NULL;

    if (!path || !*path) {
        return NULL;
    }

    last = path;
    while (*path) {
        if (*path == '/' && path[1] != '\0') {
            last = path + 1;
        }
        path++;
    }

    return last;
}

static int resolve_copy_target_path(const char* src_path,
                                    const char* dst_path,
                                    char* out,
                                    size_t out_size) {
    const char* base = NULL;
    size_t dst_len = 0;
    size_t base_len = 0;

    if (!src_path || !dst_path || !out || out_size < 2) {
        return -1;
    }

    if (!path_is_dir(dst_path)) {
        dst_len = strlen(dst_path);
        if (dst_len + 1 > out_size) {
            return -1;
        }
        memcpy(out, dst_path, dst_len + 1);
        return 0;
    }

    base = path_basename(src_path);
    if (!base || !*base) {
        return -1;
    }

    dst_len = strlen(dst_path);
    base_len = strlen(base);
    if (dst_len == 0) {
        return -1;
    }

    if (streq(dst_path, "/")) {
        if (1 + base_len + 1 > out_size) {
            return -1;
        }
        out[0] = '/';
        memcpy(out + 1, base, base_len + 1);
        return 0;
    }

    if (dst_len + 1 + base_len + 1 > out_size) {
        return -1;
    }

    memcpy(out, dst_path, dst_len);
    out[dst_len] = '/';
    memcpy(out + dst_len + 1, base, base_len + 1);
    return 0;
}

static void init_cwd(void) {
    static const char home_dir[] = "/home";

    if (path_is_dir(home_dir)) {
        memcpy(g_cwd, home_dir, sizeof(home_dir));
        return;
    }

    g_cwd[0] = '/';
    g_cwd[1] = '\0';
}

static void write_stat_line(const kiwi_stat_t* st) {
    if (!st) {
        return;
    }

    write_str("ino=");
    write_u64(st->ino);
    write_str(" type=");
    if (st->type == KIWI_VNODE_DIR) {
        write_str("dir");
    } else if (st->type == KIWI_VNODE_FILE) {
        write_str("file");
    } else {
        write_str("unknown");
    }
    write_str(" size=");
    write_u64(st->size);
    write_str(" links=");
    write_u64(st->link_count);
    write_str("\n");
}

static int copy_file_contents(const char* src_path, const char* dst_path) {
    char buf[IO_BUFFER_SIZE];
    int64_t src_fd = -1;
    int64_t dst_fd = -1;

    if (!src_path || !dst_path || streq(src_path, dst_path)) {
        return -1;
    }

    src_fd = sys_open(src_path, KIWI_O_RDONLY);
    if (src_fd < 0) {
        return -1;
    }

    dst_fd = sys_open(dst_path, KIWI_O_WRONLY | KIWI_O_CREAT | KIWI_O_TRUNC);
    if (dst_fd < 0) {
        (void)sys_close((int)src_fd);
        return -1;
    }

    for (;;) {
        int64_t n = sys_read((int)src_fd, buf, sizeof(buf));
        if (n < 0) {
            (void)sys_close((int)src_fd);
            (void)sys_close((int)dst_fd);
            (void)sys_unlink(dst_path);
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (sys_write((int)dst_fd, buf, (uint64_t)n) != n) {
            (void)sys_close((int)src_fd);
            (void)sys_close((int)dst_fd);
            (void)sys_unlink(dst_path);
            return -1;
        }
    }

    (void)sys_close((int)src_fd);
    (void)sys_close((int)dst_fd);
    return 0;
}

static void show_help(void) {
    write_line("Built-ins: help echo clear pwd cd ls stat cat touch mkdir rm cp mv mount rescan which exit");
}

static void cmd_pwd(char* rest) {
    if (has_extra_args(rest)) {
        write_line("usage: pwd");
        return;
    }

    write_line(g_cwd);
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

    memcpy(g_cwd, path, strlen(path) + 1);
}

static void cmd_ls(char* rest) {
    char path[PATH_BUFFER_SIZE];
    kiwi_stat_t st;
    kiwi_dirent_t ent;
    uint64_t index = 0;
    int64_t rc = 0;
    char* arg = next_arg(&rest);

    if (!arg) {
        memcpy(path, g_cwd, strlen(g_cwd) + 1);
    } else if (resolve_path_arg(arg, path, sizeof(path)) != 0 || has_extra_args(rest)) {
        write_line("usage: ls [path]");
        return;
    }

    if (stat_path(path, &st) != 0) {
        write_line("shell: ls failed");
        return;
    }
    if (st.type == KIWI_VNODE_FILE) {
        write_line(path);
        return;
    }
    if (st.type != KIWI_VNODE_DIR) {
        write_line("shell: ls failed");
        return;
    }

    for (;;) {
        memset(&ent, 0, sizeof(ent));
        rc = sys_readdir(path, index, &ent);
        if (rc < 0) {
            write_line("shell: ls failed");
            return;
        }
        if (rc == 0) {
            return;
        }
        write_line(ent.name);
        index++;
    }
}

static void cmd_stat(char* rest) {
    char path[PATH_BUFFER_SIZE];
    kiwi_stat_t st;
    char* arg = next_arg(&rest);

    if (!arg || resolve_path_arg(arg, path, sizeof(path)) != 0 || has_extra_args(rest)) {
        write_line("usage: stat <path>");
        return;
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(path, &st) != 0) {
        write_line("shell: stat failed");
        return;
    }

    write_stat_line(&st);
}

static void cmd_cat(char* rest) {
    char path[PATH_BUFFER_SIZE];
    char buf[IO_BUFFER_SIZE];
    int64_t fd = -1;
    int64_t n = 0;
    int saw_output = 0;
    char last_ch = '\0';
    char* arg = next_arg(&rest);

    if (!arg || resolve_path_arg(arg, path, sizeof(path)) != 0 || has_extra_args(rest)) {
        write_line("usage: cat <path>");
        return;
    }

    fd = sys_open(path, KIWI_O_RDONLY);
    if (fd < 0) {
        write_line("shell: cat failed");
        return;
    }

    for (;;) {
        n = sys_read((int)fd, buf, sizeof(buf));
        if (n < 0) {
            write_line("shell: cat read failed");
            break;
        }
        if (n == 0) {
            break;
        }
        saw_output = 1;
        last_ch = buf[n - 1];
        write_bytes(buf, (size_t)n);
    }

    (void)sys_close((int)fd);
    if (!saw_output || last_ch != '\n') {
        write_str("\n");
    }
}

static void cmd_touch(char* rest) {
    char path[PATH_BUFFER_SIZE];
    kiwi_stat_t st;
    int64_t fd = -1;
    char* arg = next_arg(&rest);

    if (!arg || resolve_path_arg(arg, path, sizeof(path)) != 0 || has_extra_args(rest)) {
        write_line("usage: touch <path>");
        return;
    }

    if (stat_path(path, &st) == 0) {
        if (st.type != KIWI_VNODE_FILE) {
            write_line("shell: touch: not a regular file");
        }
        return;
    }

    fd = sys_open(path, KIWI_O_WRONLY | KIWI_O_CREAT);
    if (fd < 0) {
        write_line("shell: touch failed");
        return;
    }

    (void)sys_close((int)fd);
}

static void cmd_mkdir(char* rest) {
    char path[PATH_BUFFER_SIZE];
    char* arg = next_arg(&rest);

    if (!arg || resolve_path_arg(arg, path, sizeof(path)) != 0 || has_extra_args(rest)) {
        write_line("usage: mkdir <path>");
        return;
    }

    if (sys_mkdir(path, 0755u) < 0) {
        write_line("shell: mkdir failed");
    }
}

static void cmd_rm(char* rest) {
    char path[PATH_BUFFER_SIZE];
    char* arg = next_arg(&rest);

    if (!arg || resolve_path_arg(arg, path, sizeof(path)) != 0 || has_extra_args(rest)) {
        write_line("usage: rm <path>");
        return;
    }

    if (sys_unlink(path) < 0) {
        write_line("shell: rm failed");
    }
}

static void cmd_cp(char* rest) {
    char src[PATH_BUFFER_SIZE];
    char dst[PATH_BUFFER_SIZE];
    char target[PATH_BUFFER_SIZE];
    kiwi_stat_t st;
    char* src_arg = next_arg(&rest);
    char* dst_arg = next_arg(&rest);

    if (!src_arg || !dst_arg ||
        resolve_path_arg(src_arg, src, sizeof(src)) != 0 ||
        resolve_path_arg(dst_arg, dst, sizeof(dst)) != 0 ||
        has_extra_args(rest)) {
        write_line("usage: cp <src> <dst>");
        return;
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(src, &st) != 0 || st.type != KIWI_VNODE_FILE) {
        write_line("shell: cp only supports regular files");
        return;
    }

    if (resolve_copy_target_path(src, dst, target, sizeof(target)) != 0) {
        write_line("shell: cp failed");
        return;
    }

    if (copy_file_contents(src, target) != 0) {
        write_line("shell: cp failed");
    }
}

static void cmd_mv(char* rest) {
    char src[PATH_BUFFER_SIZE];
    char dst[PATH_BUFFER_SIZE];
    char target[PATH_BUFFER_SIZE];
    kiwi_stat_t st;
    char* src_arg = next_arg(&rest);
    char* dst_arg = next_arg(&rest);

    if (!src_arg || !dst_arg ||
        resolve_path_arg(src_arg, src, sizeof(src)) != 0 ||
        resolve_path_arg(dst_arg, dst, sizeof(dst)) != 0 ||
        has_extra_args(rest)) {
        write_line("usage: mv <src> <dst>");
        return;
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(src, &st) != 0 || st.type != KIWI_VNODE_FILE) {
        write_line("shell: mv only supports regular files");
        return;
    }

    if (resolve_copy_target_path(src, dst, target, sizeof(target)) != 0) {
        write_line("shell: mv failed");
        return;
    }

    if (copy_file_contents(src, target) != 0 || sys_unlink(src) != 0) {
        write_line("shell: mv failed");
    }
}

static void cmd_which(char* rest) {
    char path[PATH_BUFFER_SIZE];
    char* arg = next_arg(&rest);

    if (!arg || has_extra_args(rest)) {
        write_line("usage: which <command>");
        return;
    }

    if (resolve_program_path(arg, path, sizeof(path)) != 0) {
        write_line("shell: command not found");
        return;
    }

    write_line(path);
}

static void cmd_mount(char* rest) {
    char target[PATH_BUFFER_SIZE];
    kiwi_stat_t st;
    char* source_arg = next_arg(&rest);
    char* target_arg = next_arg(&rest);

    if (!source_arg || has_extra_args(rest)) {
        write_line("usage: mount <device> [path]");
        return;
    }

    if (!target_arg) {
        target[0] = '/';
        target[1] = '\0';
    } else if (resolve_path_arg(target_arg, target, sizeof(target)) != 0) {
        write_line("usage: mount <device> [path]");
        return;
    }

    memset(&st, 0, sizeof(st));
    if (sys_stat(target, &st) != 0 || st.type != KIWI_VNODE_DIR) {
        write_line("shell: mount target must be an existing directory");
        return;
    }

    if (sys_mount(source_arg, target) != 0) {
        write_line("shell: mount failed");
    }
}

static void cmd_rescan(char* rest) {
    int64_t count = 0;

    if (has_extra_args(rest)) {
        write_line("usage: rescan");
        return;
    }

    count = sys_dev_rescan();
    if (count < 0) {
        write_line("shell: rescan failed");
        return;
    }

    write_str("rescan: found ");
    write_u64((uint64_t)count);
    write_line(" new disk(s)");
}

static void run_program(const char* cmd, char* rest) {
    char path[PATH_BUFFER_SIZE];
    int status = 0;
    int64_t pid = 0;

    if (rest && *skip_spaces(rest)) {
        write_line("shell: program arguments are not supported yet");
        return;
    }

    if (resolve_program_path(cmd, path, sizeof(path)) != 0) {
        write_line("shell: command not found");
        return;
    }

    pid = sys_spawn(path);
    if (pid < 0) {
        write_str("shell: failed to start ");
        write_str(path);
        write_str("\n");
        return;
    }

    if (sys_waitpid((int)pid, &status) < 0) {
        write_str("shell: wait failed for ");
        write_str(path);
        write_str("\n");
    }
}

static void cmd_clear(char* rest) {
    if (has_extra_args(rest)) {
        write_line("usage: clear");
        return;
    }

    (void)sys_console_clear();
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

        if (c == '\n') {
            char* raw = NULL;

            redraw_input_line(line, input_len, input_len, 0);
            write_str("\n");
            line[input_len] = '\0';
            trim_trailing_spaces(line);
            raw = skip_spaces(line);

            cursor = raw;
            cmd = next_arg(&cursor);
            if (cmd && *cmd != '\0') {
                history_record(raw);

                if (streq(cmd, "help")) {
                    show_help();
                } else if (streq(cmd, "echo")) {
                    write_line(skip_spaces(cursor));
                } else if (streq(cmd, "clear")) {
                    cmd_clear(cursor);
                } else if (streq(cmd, "pwd")) {
                    cmd_pwd(cursor);
                } else if (streq(cmd, "cd")) {
                    cmd_cd(cursor);
                } else if (streq(cmd, "ls")) {
                    cmd_ls(cursor);
                } else if (streq(cmd, "stat")) {
                    cmd_stat(cursor);
                } else if (streq(cmd, "cat")) {
                    cmd_cat(cursor);
                } else if (streq(cmd, "touch")) {
                    cmd_touch(cursor);
                } else if (streq(cmd, "mkdir")) {
                    cmd_mkdir(cursor);
                } else if (streq(cmd, "rm")) {
                    cmd_rm(cursor);
                } else if (streq(cmd, "cp")) {
                    cmd_cp(cursor);
                } else if (streq(cmd, "mv")) {
                    cmd_mv(cursor);
                } else if (streq(cmd, "mount")) {
                    cmd_mount(cursor);
                } else if (streq(cmd, "rescan")) {
                    cmd_rescan(cursor);
                } else if (streq(cmd, "which")) {
                    cmd_which(cursor);
                } else if (streq(cmd, "exit")) {
                    return 0;
                } else {
                    run_program(cmd, cursor);
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
