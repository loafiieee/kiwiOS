#include <stddef.h>
#include <stdint.h>
#include "arch/x86/io.h"
#include "core/syscall.h"
#include "core/time.h"
#include "drivers/acpi/acpi.h"
#include "core/console.h"
#include "core/keyboard.h"
#include "core/kxe.h"
#include "core/process.h"
#include "core/scheduler.h"
#include "drivers/serial/serial.h"
#include "drivers/block/block.h"
#include "libc/string.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "vfs/vfs.h"

#define USER_VADDR_MIN 0x0000000000001000ull
#define USER_VADDR_MAX 0x0000800000000000ull
#define SYSCALL_PATH_MAX 256u
#define FD_STDIN 0
#define FD_STDOUT 1
#define FD_STDERR 2
#define FD_FIRST_USER 3
#define WAIT_ANY_PID 0xffffffffu
#define USER_HEAP_MAX (KXE_USER_STACK_TOP - ((uint64_t)KXE_USER_STACK_PAGES * PAGE_SIZE))
#define PIPE_MAX 32u
#define PIPE_CAPACITY 4096u

static uint32_t g_next_fd_description_id = 1;

struct process_pipe {
    uint8_t used;
    uint32_t refs;
    uint32_t readers;
    uint32_t writers;
    uint64_t read_pos;
    uint64_t write_pos;
    uint64_t count;
    uint8_t buffer[PIPE_CAPACITY];
};

static struct process_pipe g_pipes[PIPE_MAX];

static struct process_pipe* pipe_alloc(void) {
    for (uint32_t i = 0; i < PIPE_MAX; i++) {
        struct process_pipe* pipe = &g_pipes[i];
        if (pipe->used) {
            continue;
        }

        memset(pipe, 0, sizeof(*pipe));
        pipe->used = 1;
        return pipe;
    }

    return NULL;
}

static void pipe_ref_endpoint(struct process_pipe* pipe, bool can_read, bool can_write) {
    if (!pipe || !pipe->used) {
        return;
    }

    pipe->refs++;
    if (can_read) {
        pipe->readers++;
    }
    if (can_write) {
        pipe->writers++;
    }
}

static void pipe_release_endpoint(struct process_pipe* pipe, bool can_read, bool can_write) {
    if (!pipe || !pipe->used) {
        return;
    }

    if (can_read && pipe->readers > 0) {
        pipe->readers--;
    }
    if (can_write && pipe->writers > 0) {
        pipe->writers--;
    }
    if (pipe->refs > 0) {
        pipe->refs--;
    }
    if (pipe->refs == 0) {
        memset(pipe, 0, sizeof(*pipe));
    }
}

static int64_t pipe_read_bytes(struct process_pipe* pipe, void* buf, uint64_t len) {
    uint8_t* out = (uint8_t*)buf;
    uint64_t nread = 0;

    if (!pipe || !pipe->used || !out) {
        return -1;
    }

    while (nread < len && pipe->count > 0) {
        out[nread++] = pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1u) % PIPE_CAPACITY;
        pipe->count--;
    }

    if (nread == 0 && pipe->writers != 0) {
        return 0;
    }

    return (int64_t)nread;
}

static int64_t pipe_write_bytes(struct process_pipe* pipe, const void* buf, uint64_t len) {
    const uint8_t* in = (const uint8_t*)buf;
    uint64_t nwritten = 0;

    if (!pipe || !pipe->used || !in || pipe->readers == 0) {
        return -1;
    }

    while (nwritten < len && pipe->count < PIPE_CAPACITY) {
        pipe->buffer[pipe->write_pos] = in[nwritten++];
        pipe->write_pos = (pipe->write_pos + 1u) % PIPE_CAPACITY;
        pipe->count++;
    }

    return (int64_t)nwritten;
}

static uint32_t process_fd_next_description_id(void) {
    uint32_t id = g_next_fd_description_id++;

    if (id == 0) {
        id = g_next_fd_description_id++;
    }

    return id;
}

static process_fd_t* process_fd_lookup_used(process_t* proc, int fd) {
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) {
        return NULL;
    }

    if (!proc->fds[fd].used) {
        return NULL;
    }

    return &proc->fds[fd];
}

static process_fd_t* process_fd_lookup(process_t* proc, int fd) {
    process_fd_t* entry = process_fd_lookup_used(proc, fd);

    if (!entry || entry->is_console || entry->is_pipe || !entry->vnode) {
        return NULL;
    }

    return entry;
}

static bool process_fd_default_console(int fd, process_fd_t* out) {
    if (!out || fd < FD_STDIN || fd > FD_STDERR) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->used = 1;
    out->is_console = 1;
    out->console_fd = (uint8_t)fd;
    out->flags = (fd == FD_STDIN) ? KIWI_O_RDONLY : KIWI_O_WRONLY;
    return true;
}

static int process_fd_alloc(process_t* proc, vnode_t* vnode, uint32_t flags, uint64_t offset) {
    if (!proc || !vnode) {
        return -1;
    }

    for (uint32_t i = FD_FIRST_USER; i < PROC_MAX_FDS; i++) {
        if (proc->fds[i].used) {
            continue;
        }

        memset(&proc->fds[i], 0, sizeof(proc->fds[i]));
        proc->fds[i].used = 1;
        proc->fds[i].flags = flags;
        proc->fds[i].description_id = process_fd_next_description_id();
        proc->fds[i].vnode = vnode;
        proc->fds[i].offset = offset;
        return (int)i;
    }

    return -1;
}

static int process_fd_find_free(process_t* proc, int min_fd) {
    if (!proc || min_fd < 0 || min_fd >= PROC_MAX_FDS) {
        return -1;
    }

    for (int fd = min_fd; fd < PROC_MAX_FDS; fd++) {
        if (!proc->fds[fd].used) {
            return fd;
        }
    }

    return -1;
}

static void process_fd_install_pipe(process_t* proc,
                                    int fd,
                                    struct process_pipe* pipe,
                                    bool can_read,
                                    bool can_write) {
    process_fd_t* entry = &proc->fds[fd];

    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    entry->is_pipe = 1;
    entry->pipe_read = can_read ? 1u : 0u;
    entry->pipe_write = can_write ? 1u : 0u;
    entry->flags = can_read ? KIWI_O_RDONLY : KIWI_O_WRONLY;
    entry->description_id = process_fd_next_description_id();
    entry->pipe = pipe;
    pipe_ref_endpoint(pipe, can_read, can_write);
}

void syscall_fd_release_resources(process_fd_t* entry) {
    if (!entry || !entry->used) {
        return;
    }

    if (entry->is_pipe && entry->pipe) {
        pipe_release_endpoint(entry->pipe,
                              entry->pipe_read != 0,
                              entry->pipe_write != 0);
    } else if (!entry->is_console && entry->vnode) {
        vfs_vnode_put(entry->vnode);
    }
}

static void process_fd_clear(process_fd_t* entry) {
    if (!entry || !entry->used) {
        return;
    }

    syscall_fd_release_resources(entry);
    memset(entry, 0, sizeof(*entry));
}

static void process_fd_sync_offset(process_t* proc, const process_fd_t* src, uint64_t offset) {
    if (!proc || !src || src->description_id == 0) {
        return;
    }

    for (uint32_t i = 0; i < PROC_MAX_FDS; i++) {
        process_fd_t* entry = &proc->fds[i];
        if (!entry->used || entry->is_console || entry->is_pipe ||
            entry->description_id != src->description_id) {
            continue;
        }
        entry->offset = offset;
    }
}

static void process_fd_sync_flags(process_t* proc, const process_fd_t* src, uint32_t flags) {
    if (!proc || !src || src->description_id == 0) {
        return;
    }

    for (uint32_t i = 0; i < PROC_MAX_FDS; i++) {
        process_fd_t* entry = &proc->fds[i];
        if (!entry->used || entry->is_console || entry->is_pipe ||
            entry->description_id != src->description_id) {
            continue;
        }
        entry->flags = flags;
    }
}

static bool process_fd_install_copy(process_t* proc, int target_fd, const process_fd_t* src) {
    process_fd_t* dst = NULL;

    if (!proc || !src || !src->used || target_fd < 0 || target_fd >= PROC_MAX_FDS) {
        return false;
    }

    dst = &proc->fds[target_fd];
    process_fd_clear(dst);
    memcpy(dst, src, sizeof(*dst));
    dst->fd_flags = 0;
    if (dst->is_pipe && dst->pipe) {
        pipe_ref_endpoint(dst->pipe,
                          dst->pipe_read != 0,
                          dst->pipe_write != 0);
    } else if (!dst->is_console && dst->vnode) {
        vfs_vnode_get(dst->vnode);
    }
    return true;
}

static bool process_fd_source(process_t* proc, int fd, process_fd_t* scratch, process_fd_t** out) {
    process_fd_t* entry = NULL;

    if (!out) {
        return false;
    }

    *out = NULL;
    entry = process_fd_lookup_used(proc, fd);
    if (entry) {
        *out = entry;
        return true;
    }

    if (process_fd_default_console(fd, scratch)) {
        *out = scratch;
        return true;
    }

    return false;
}

static bool fd_can_read(const process_fd_t* entry) {
    uint32_t accmode = 0;

    if (!entry || !entry->used) {
        return false;
    }

    accmode = entry->flags & KIWI_O_ACCMODE;
    return accmode == KIWI_O_RDONLY || accmode == KIWI_O_RDWR;
}

static bool fd_can_write(const process_fd_t* entry) {
    uint32_t accmode = 0;

    if (!entry || !entry->used) {
        return false;
    }

    accmode = entry->flags & KIWI_O_ACCMODE;
    return accmode == KIWI_O_WRONLY || accmode == KIWI_O_RDWR;
}

static bool add_signed_offset(uint64_t base, int64_t delta, uint64_t* out) {
    if (!out) {
        return false;
    }

    if (delta >= 0) {
        uint64_t amount = (uint64_t)delta;
        if (base > (UINT64_MAX - amount)) {
            return false;
        }

        *out = base + amount;
        return true;
    }

    uint64_t amount = (uint64_t)(-(delta + 1)) + 1u;
    if (amount > base) {
        return false;
    }

    *out = base - amount;
    return true;
}

static bool parse_u32_strict_kernel(const char* s, uint32_t* out) {
    uint64_t value = 0;

    if (!s || !out) {
        return false;
    }

    if (*s == '\0') {
        return false;
    }

    while (*s >= '0' && *s <= '9') {
        value = value * 10u + (uint64_t)(*s - '0');
        if (value > 0xffffffffu) {
            return false;
        }
        s++;
    }

    if (*s != '\0') {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

static block_device_t* resolve_mount_device_spec(const char* spec) {
    const char* name = spec;
    uint32_t part_index = 0;

    if (!spec || !*spec) {
        return NULL;
    }

    if (parse_u32_strict_kernel(spec, &part_index)) {
        return block_partition_device(part_index);
    }

    if (strncmp(name, "/dev/", 5u) == 0) {
        name += 5u;
    }

    return block_device_by_name(name);
}

static process_t* process_child_lookup(process_t* parent, uint32_t pid) {
    process_t* child = NULL;

    if (!parent || pid == 0) {
        return NULL;
    }

    child = process_by_pid(pid);
    if (!child || child->ppid != parent->pid) {
        return NULL;
    }

    return child;
}

typedef struct {
    uint64_t want_index;
    uint64_t cur_index;
    bool found;
    kiwi_dirent_t ent;
} readdir_index_ctx_t;

static bool readdir_index_cb(const char* name, uint32_t ino, void* user) {
    readdir_index_ctx_t* ctx = (readdir_index_ctx_t*)user;
    uint64_t name_len = 0;

    if (!ctx || !name) {
        return true;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return true;
    }

    if (ctx->cur_index == ctx->want_index) {
        memset(&ctx->ent, 0, sizeof(ctx->ent));
        ctx->ent.ino = ino;
        name_len = strlen(name);
        if (name_len >= KIWI_DIRENT_NAME_MAX) {
            name_len = KIWI_DIRENT_NAME_MAX - 1u;
        }
        memcpy(ctx->ent.name, name, (size_t)name_len);
        ctx->found = true;
        return false;
    }

    ctx->cur_index++;
    return true;
}

bool validate_user_buffer(uint64_t addr, uint64_t len, bool needs_write) {
    process_t* proc = process_current();
    uint64_t end = 0;
    uint64_t page = 0;
    uint64_t last_page = 0;

    if (len == 0) {
        return true;
    }

    if (!proc || !proc->page_table) {
        return false;
    }

    if (addr < USER_VADDR_MIN || addr >= USER_VADDR_MAX) {
        return false;
    }

    end = addr + len;
    if (end < addr || end > USER_VADDR_MAX) {
        return false;
    }

    page = PAGE_ALIGN_DOWN(addr);
    last_page = PAGE_ALIGN_DOWN(end - 1u);

    for (;;) {
        uint64_t flags = 0;

        if (!vmm_get_mapping(proc->page_table, page, NULL, &flags)) {
            return false;
        }

        if (!(flags & PAGE_USER)) {
            return false;
        }

        if (needs_write && !(flags & PAGE_WRITE)) {
            return false;
        }

        if (page == last_page) {
            break;
        }

        if (page > (UINT64_MAX - PAGE_SIZE)) {
            return false;
        }
        page += PAGE_SIZE;
    }

    return true;
}

bool copy_user_string(const char* user_str, char* kernel_buf, uint64_t kernel_buf_size) {
    if (!user_str || !kernel_buf || kernel_buf_size == 0) {
        return false;
    }

    for (uint64_t i = 0; i < kernel_buf_size; i++) {
        uint64_t addr = (uint64_t)(uintptr_t)(user_str + i);
        if (!validate_user_buffer(addr, 1, false)) {
            return false;
        }

        kernel_buf[i] = user_str[i];
        if (kernel_buf[i] == '\0') {
            return true;
        }
    }

    kernel_buf[kernel_buf_size - 1] = '\0';
    return false;
}

static void pop_path_component(char* path) {
    uint64_t len = 0;

    if (!path || path[0] == '\0') {
        return;
    }

    len = strlen(path);
    while (len > 0 && path[len - 1u] != '/') {
        len--;
    }

    if (len <= 1u) {
        path[0] = '\0';
        return;
    }

    path[len - 1u] = '\0';
}

static bool append_path_component(char* out, uint64_t out_size, const char* comp, uint64_t comp_len) {
    uint64_t out_len = 0;

    if (!out || !comp || out_size < 2u || comp_len == 0u) {
        return false;
    }

    if (comp_len == 1u && comp[0] == '.') {
        return true;
    }
    if (comp_len == 2u && comp[0] == '.' && comp[1] == '.') {
        pop_path_component(out);
        return true;
    }

    out_len = strlen(out);
    if (out_len == 0u) {
        if (comp_len + 2u > out_size) {
            return false;
        }
        out[0] = '/';
        memcpy(out + 1u, comp, (size_t)comp_len);
        out[comp_len + 1u] = '\0';
        return true;
    }

    if (out_len + 1u + comp_len + 1u > out_size) {
        return false;
    }

    out[out_len] = '/';
    memcpy(out + out_len + 1u, comp, (size_t)comp_len);
    out[out_len + 1u + comp_len] = '\0';
    return true;
}

static bool normalize_absolute_path(const char* in, char* out, uint64_t out_size) {
    const char* p = in;

    if (!in || !out || out_size < 2u || in[0] != '/') {
        return false;
    }

    out[0] = '\0';
    while (*p) {
        const char* start = NULL;
        uint64_t len = 0;

        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }

        len = (uint64_t)(p - start);
        if (!append_path_component(out, out_size, start, len)) {
            return false;
        }
    }

    if (out[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
    }

    return true;
}

static bool resolve_kernel_path(const char* path, char* out, uint64_t out_size) {
    process_t* proc = process_current();
    char combined[SYSCALL_PATH_MAX * 2u];
    const char* cwd = "/";
    uint64_t cwd_len = 0;
    uint64_t path_len = 0;

    if (!path || !out || out_size < 2u || path[0] == '\0') {
        return false;
    }

    if (path[0] == '/') {
        return normalize_absolute_path(path, out, out_size);
    }

    if (proc && proc->cwd[0] == '/') {
        cwd = proc->cwd;
    }

    cwd_len = strlen(cwd);
    path_len = strlen(path);

    if (strcmp(cwd, "/") == 0) {
        if (1u + path_len + 1u > sizeof(combined)) {
            return false;
        }
        combined[0] = '/';
        memcpy(combined + 1u, path, (size_t)path_len + 1u);
    } else {
        if (cwd_len + 1u + path_len + 1u > sizeof(combined)) {
            return false;
        }
        memcpy(combined, cwd, (size_t)cwd_len);
        combined[cwd_len] = '/';
        memcpy(combined + cwd_len + 1u, path, (size_t)path_len + 1u);
    }

    return normalize_absolute_path(combined, out, out_size);
}

static bool copy_user_path_resolved(const char* user_path, char* out, uint64_t out_size) {
    char raw[SYSCALL_PATH_MAX];

    if (!copy_user_string(user_path, raw, sizeof(raw))) {
        return false;
    }

    return resolve_kernel_path(raw, out, out_size);
}

static bool copy_user_argv(const char* const* user_argv,
                           uint64_t argc,
                           char storage[KXE_MAX_ARGC][KXE_ARG_MAX],
                           const char* ptrs[KXE_MAX_ARGC]) {
    if (argc > KXE_MAX_ARGC || (argc != 0 && !user_argv) || !storage || !ptrs) {
        return false;
    }

    for (uint64_t i = 0; i < argc; i++) {
        const char* user_arg = NULL;
        if (!validate_user_buffer((uint64_t)(uintptr_t)&user_argv[i], sizeof(user_argv[i]), false)) {
            return false;
        }

        user_arg = user_argv[i];
        if (!copy_user_string(user_arg, storage[i], KXE_ARG_MAX)) {
            return false;
        }

        ptrs[i] = storage[i];
    }

    return true;
}

static void stdin_queue_escape_string(process_t* proc, const char* seq) {
    uint8_t len = 0;

    if (!proc) {
        return;
    }

    while (seq && seq[len] != '\0' && len < sizeof(proc->stdin_pending)) {
        proc->stdin_pending[len] = (uint8_t)seq[len];
        len++;
    }

    proc->stdin_pending_len = len;
    proc->stdin_pending_pos = 0u;
}

static void stdin_queue_escape(process_t* proc, char code) {
    char seq[4];

    seq[0] = 0x1B;
    seq[1] = '[';
    seq[2] = code;
    seq[3] = '\0';
    stdin_queue_escape_string(proc, seq);
}

int64_t sys_write(int fd, const void* buf, uint64_t len) {
    process_t* proc = process_current();
    process_fd_t* entry = NULL;
    int64_t nwritten = 0;

    if (!buf && len != 0) {
        return -1;
    }

    if (!validate_user_buffer((uint64_t)(uintptr_t)buf, len, false)) {
        return -1;
    }

    if (proc) {
        entry = process_fd_lookup_used(proc, fd);
    }

    if (entry && entry->is_console) {
        if (entry->console_fd != FD_STDOUT && entry->console_fd != FD_STDERR) {
            return -1;
        }
        const char* s = (const char*)buf;
        for (uint64_t i = 0; i < len; i++) {
            putc_fb(NULL, s[i]);
            if (serial_is_ready()) {
                serial_putc(s[i]);
            }
        }
        return (int64_t)len;
    }

    if (!entry && (fd == FD_STDOUT || fd == FD_STDERR)) {
        const char* s = (const char*)buf;
        for (uint64_t i = 0; i < len; i++) {
            putc_fb(NULL, s[i]);
            if (serial_is_ready()) {
                serial_putc(s[i]);
            }
        }
        return (int64_t)len;
    }

    if (!proc) {
        return -1;
    }

    if (entry && entry->is_pipe) {
        if (!entry->pipe || !entry->pipe_write || !fd_can_write(entry)) {
            return -1;
        }
        return pipe_write_bytes(entry->pipe, buf, len);
    }

    if (entry && entry->is_console) {
        return -1;
    }
    if (!entry) {
        entry = process_fd_lookup(proc, fd);
    }
    if (!entry || !entry->vnode || !entry->vnode->ops || !entry->vnode->ops->write || !fd_can_write(entry)) {
        return -1;
    }

    if ((entry->flags & KIWI_O_APPEND) != 0) {
        process_fd_sync_offset(proc, entry, entry->vnode->size);
    }
    nwritten = entry->vnode->ops->write(entry->vnode, entry->offset, buf, len);
    if (nwritten > 0) {
        process_fd_sync_offset(proc, entry, entry->offset + (uint64_t)nwritten);
    }

    return nwritten;
}

int64_t sys_read(int fd, void* buf, uint64_t len) {
    process_t* proc = process_current();
    process_fd_t* entry = NULL;
    int64_t nread = 0;

    if (!proc || (!buf && len != 0)) {
        return -1;
    }

    if (!validate_user_buffer((uint64_t)(uintptr_t)buf, len, true)) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    entry = process_fd_lookup_used(proc, fd);
    if (entry && entry->is_console && entry->console_fd != FD_STDIN) {
        return -1;
    }

    if ((entry && entry->is_console) || (!entry && fd == FD_STDIN)) {
        char* out = (char*)buf;
        uint64_t i = 0;

        (void)block_poll_hotplug();

        while (i < len) {
            int ch = keyboard_getchar_nonblocking();

            while (proc->stdin_pending_pos < proc->stdin_pending_len && i < len) {
                out[i++] = (char)proc->stdin_pending[proc->stdin_pending_pos++];
            }
            if (proc->stdin_pending_pos >= proc->stdin_pending_len) {
                proc->stdin_pending_len = 0;
                proc->stdin_pending_pos = 0;
            }
            if (i >= len) {
                return (int64_t)i;
            }

            if (ch == -1) {
                proc->state = PROC_BLOCKED;
                keyboard_enqueue_waiter(proc);
                if (scheduler_checkpoint_process_kernel(proc) == 0) {
                    scheduler_switch();
                    __builtin_unreachable();
                }
                continue;
            }

            if (ch == KEY_PAGE_UP) {
                if (proc->tty_raw_mode) {
                    stdin_queue_escape_string(proc, "\x1b[5~");
                    continue;
                }
                console_page_up();
                continue;
            }
            if (ch == KEY_PAGE_DOWN) {
                if (proc->tty_raw_mode) {
                    stdin_queue_escape_string(proc, "\x1b[6~");
                    continue;
                }
                console_page_down();
                continue;
            }
            if (ch == KEY_HOME) {
                stdin_queue_escape_string(proc, "\x1b[H");
                continue;
            }
            if (ch == KEY_END) {
                stdin_queue_escape_string(proc, "\x1b[F");
                continue;
            }
            if (ch == KEY_DELETE) {
                stdin_queue_escape_string(proc, "\x1b[3~");
                continue;
            }

            if (ch == KEY_ARROW_UP) {
                stdin_queue_escape(proc, 'A');
                continue;
            }
            if (ch == KEY_ARROW_DOWN) {
                stdin_queue_escape(proc, 'B');
                continue;
            }
            if (ch == KEY_ARROW_RIGHT) {
                stdin_queue_escape(proc, 'C');
                continue;
            }
            if (ch == KEY_ARROW_LEFT) {
                stdin_queue_escape(proc, 'D');
                continue;
            }

            if (proc->tty_raw_mode && ch == '\n') {
                /* Real terminals deliver the Return key as carriage return
                 * (CR, 0x0D) in raw mode; the PS/2 driver emits '\n' (LF).
                 * Translate so raw-mode programs see Enter as '\r'. nano, for
                 * example, binds '\r'/KEY_ENTER to insert-newline but treats
                 * '\n' (^J) as the Justify command, so without this Enter runs
                 * Justify instead of splitting the line. Cooked mode keeps '\n'
                 * so shell line editing is unaffected. */
                out[i] = '\r';
                return (int64_t)(i + 1u);
            }
            out[i] = (char)ch;
            if (out[i] == '\n') {
                return (int64_t)(i + 1u);
            }
            i++;
        }
        return (int64_t)len;
    }

    if (entry && entry->is_pipe) {
        if (!entry->pipe || !entry->pipe_read || !fd_can_read(entry)) {
            return -1;
        }
        return pipe_read_bytes(entry->pipe, buf, len);
    }

    if (!entry) {
        entry = process_fd_lookup(proc, fd);
    }
    if (!entry || !entry->vnode || !entry->vnode->ops || !entry->vnode->ops->read || !fd_can_read(entry)) {
        return -1;
    }

    nread = entry->vnode->ops->read(entry->vnode, entry->offset, buf, len);
    if (nread > 0) {
        process_fd_sync_offset(proc, entry, entry->offset + (uint64_t)nread);
    }

    return nread;
}

int64_t sys_open(const char* path, int flags) {
    process_t* proc = process_current();
    char kernel_path[SYSCALL_PATH_MAX];
    vnode_t* vn = NULL;
    uint32_t accmode = 0;
    bool want_read = false;
    bool want_write = false;
    bool created = false;
    uint64_t initial_offset = 0;
    int fd = -1;

    if (!proc) {
        return -1;
    }

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    accmode = (uint32_t)flags & KIWI_O_ACCMODE;
    if (accmode > KIWI_O_RDWR ||
        ((uint32_t)flags & ~(KIWI_O_ACCMODE | KIWI_O_CREAT | KIWI_O_TRUNC | KIWI_O_APPEND)) != 0) {
        return -1;
    }

    want_read = (accmode == KIWI_O_RDONLY) || (accmode == KIWI_O_RDWR);
    want_write = (accmode == KIWI_O_WRONLY) || (accmode == KIWI_O_RDWR);

    if (!want_write && (((uint32_t)flags & KIWI_O_CREAT) != 0 ||
                        ((uint32_t)flags & KIWI_O_TRUNC) != 0 ||
                        ((uint32_t)flags & KIWI_O_APPEND) != 0)) {
        return -1;
    }

    if (!vfs_resolve(kernel_path, &vn) || !vn) {
        if (!want_write || (((uint32_t)flags & KIWI_O_CREAT) == 0) ||
            !vfs_create(kernel_path, 0644u, &vn) || !vn) {
            return -1;
        }
        created = true;
    }

    if ((want_read && (!vn->ops || !vn->ops->read)) ||
        (want_write && (!vn->ops || !vn->ops->write))) {
        vfs_vnode_put(vn);
        return -1;
    }

    if (!created && want_write && (((uint32_t)flags & KIWI_O_TRUNC) != 0)) {
        if (!vn->ops || !vn->ops->truncate || !vn->ops->truncate(vn, 0)) {
            vfs_vnode_put(vn);
            return -1;
        }
    }

    if (want_write && (((uint32_t)flags & KIWI_O_APPEND) != 0)) {
        initial_offset = vn->size;
    }

    fd = process_fd_alloc(proc, vn, (uint32_t)flags, initial_offset);
    if (fd < 0) {
        vfs_vnode_put(vn);
        return -1;
    }

    return fd;
}

int64_t sys_close(int fd) {
    process_t* proc = process_current();
    process_fd_t* entry = NULL;

    if (!proc) {
        return -1;
    }

    entry = process_fd_lookup_used(proc, fd);
    if (!entry) {
        return -1;
    }

    process_fd_clear(entry);
    return 0;
}

int64_t sys_dup(int oldfd) {
    process_t* proc = process_current();
    process_fd_t scratch;
    process_fd_t* src = NULL;

    if (!proc || !process_fd_source(proc, oldfd, &scratch, &src)) {
        return -1;
    }

    for (int fd = FD_FIRST_USER; fd < PROC_MAX_FDS; fd++) {
        if (proc->fds[fd].used) {
            continue;
        }
        return process_fd_install_copy(proc, fd, src) ? fd : -1;
    }

    return -1;
}

int64_t sys_dup2(int oldfd, int newfd) {
    process_t* proc = process_current();
    process_fd_t scratch;
    process_fd_t* src = NULL;

    if (!proc || newfd < 0 || newfd >= PROC_MAX_FDS ||
        !process_fd_source(proc, oldfd, &scratch, &src)) {
        return -1;
    }

    if (oldfd == newfd) {
        return newfd;
    }

    return process_fd_install_copy(proc, newfd, src) ? newfd : -1;
}

int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    process_t* proc = process_current();
    process_fd_t scratch;
    process_fd_t* src = NULL;

    if (!proc || !process_fd_source(proc, fd, &scratch, &src)) {
        return -1;
    }

    switch (cmd) {
        case KIWI_F_DUPFD: {
            if (arg >= PROC_MAX_FDS) {
                return -1;
            }
            for (int newfd = (int)arg; newfd < PROC_MAX_FDS; newfd++) {
                if (proc->fds[newfd].used) {
                    continue;
                }
                return process_fd_install_copy(proc, newfd, src) ? newfd : -1;
            }
            return -1;
        }
        case KIWI_F_GETFD:
            return (int64_t)(src->fd_flags & KIWI_FD_CLOEXEC);
        case KIWI_F_SETFD: {
            process_fd_t* entry = process_fd_lookup_used(proc, fd);
            if ((arg & ~((uint64_t)KIWI_FD_CLOEXEC)) != 0) {
                return -1;
            }
            if (!entry) {
                return process_fd_default_console(fd, &scratch) ? 0 : -1;
            }
            entry->fd_flags = (uint8_t)(arg & KIWI_FD_CLOEXEC);
            return 0;
        }
        case KIWI_F_GETFL:
            return (int64_t)src->flags;
        case KIWI_F_SETFL: {
            process_fd_t* entry = process_fd_lookup_used(proc, fd);
            uint32_t new_flags = 0;
            if (!entry) {
                return 0;
            }
            new_flags = (entry->flags & ~KIWI_O_APPEND) | ((uint32_t)arg & KIWI_O_APPEND);
            process_fd_sync_flags(proc, entry, new_flags);
            return 0;
        }
        default:
            return -1;
    }
}

int64_t sys_clock_gettime(int clock_id, kiwi_timespec_t* out) {
    kiwi_timespec_t ts;
    uint64_t ns = 0;

    if (!out || !validate_user_buffer((uint64_t)(uintptr_t)out, sizeof(*out), true)) {
        return -1;
    }

    if (clock_id == KIWI_CLOCK_REALTIME) {
        ns = time_realtime_ns();
        ts.tv_sec = (int64_t)(ns / 1000000000ull);
        ts.tv_nsec = (int64_t)(ns % 1000000000ull);
    } else if (clock_id == KIWI_CLOCK_MONOTONIC) {
        ns = time_monotonic_ns();
        ts.tv_sec = (int64_t)(ns / 1000000000ull);
        ts.tv_nsec = (int64_t)(ns % 1000000000ull);
    } else {
        return -1;
    }

    memcpy(out, &ts, sizeof(ts));
    return 0;
}

static void syscall_legacy_i8042_reboot(void) __attribute__((noreturn));
static void syscall_legacy_i8042_reboot(void) {
    asm volatile("cli");

    for (uint32_t i = 0; i < 100000u; i++) {
        if ((inb(0x64) & 0x02u) == 0u) {
            break;
        }
        asm volatile("pause");
    }

    outb(0x64, 0xfe);
    for (;;) {
        asm volatile("hlt");
    }
}

int64_t sys_poweroff(void) {
    return acpi_poweroff() ? 0 : -1;
}

int64_t sys_reboot(void) {
    (void)acpi_reboot();
    syscall_legacy_i8042_reboot();
}

int64_t sys_pipe(int* fds) {
    process_t* proc = process_current();
    struct process_pipe* pipe = NULL;
    int read_fd = -1;
    int write_fd = -1;
    int out[2];

    if (!proc || !fds ||
        !validate_user_buffer((uint64_t)(uintptr_t)fds, sizeof(out), true)) {
        return -1;
    }

    read_fd = process_fd_find_free(proc, FD_FIRST_USER);
    if (read_fd < 0) {
        return -1;
    }
    write_fd = process_fd_find_free(proc, read_fd + 1);
    if (write_fd < 0) {
        return -1;
    }

    pipe = pipe_alloc();
    if (!pipe) {
        return -1;
    }

    process_fd_install_pipe(proc, read_fd, pipe, true, false);
    process_fd_install_pipe(proc, write_fd, pipe, false, true);

    out[0] = read_fd;
    out[1] = write_fd;
    memcpy(fds, out, sizeof(out));
    return 0;
}

void sys_exit(int code) {
    process_t* proc = process_current();
    process_t* parent = NULL;
    if (proc) {
        proc->state = PROC_ZOMBIE;
        proc->exit_code = code;
        process_close_files(proc);
        parent = process_by_pid(proc->ppid);
        if (parent &&
            parent->state == PROC_BLOCKED &&
            (parent->wait_target_pid == WAIT_ANY_PID ||
             parent->wait_target_pid == proc->pid)) {
            scheduler_add(parent);
        }

        /* Process exit is normal and frequent (every spawned program), so keep
         * it off the framebuffer console. The serial log still records it for
         * debugging. */
        if (serial_is_ready()) {
            serial_kprintf("\n[sys_exit] pid=%u exited with code %u.\n",
                           proc->pid,
                           (uint32_t)code);
        }
        scheduler_switch();
        __builtin_unreachable();
    } else {
        print(NULL, "\n[sys_exit] Process exited with code ");
        print_u64(NULL, (uint64_t)(uint32_t)code);
        print(NULL, "\n");
        if (serial_is_ready()) {
            serial_kprintf("\n[sys_exit] Process exited with code %u.\n",
                           (uint32_t)code);
        }
    }

    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

int64_t sys_brk(uint64_t addr) {
    process_t* proc = process_current();
    uint64_t old_break = 0;
    uint64_t old_top = 0;
    uint64_t new_top = 0;

    if (!proc || !proc->page_table) {
        return -1;
    }

    if (addr == 0) {
        return (int64_t)proc->brk_current;
    }

    if (addr < proc->brk_base || addr > USER_HEAP_MAX) {
        return (int64_t)proc->brk_current;
    }

    old_break = proc->brk_current;
    old_top = PAGE_ALIGN_UP(old_break);
    new_top = PAGE_ALIGN_UP(addr);

    if (new_top > old_top) {
        for (uint64_t va = old_top; va < new_top; va += PAGE_SIZE) {
            uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc();
            if (!phys) {
                return (int64_t)old_break;
            }

            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            if (!vmm_map_page(proc->page_table, va, phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITE)) {
                pmm_free((void*)(uintptr_t)phys);
                for (uint64_t rollback = old_top; rollback < va; rollback += PAGE_SIZE) {
                    uint64_t mapped_phys = vmm_get_physical(proc->page_table, rollback);
                    if (!mapped_phys) {
                        continue;
                    }

                    vmm_unmap_page(proc->page_table, rollback);
                    pmm_free((void*)(uintptr_t)mapped_phys);
                }
                return (int64_t)old_break;
            }
        }
    } else if (new_top < old_top) {
        for (uint64_t va = new_top; va < old_top; va += PAGE_SIZE) {
            uint64_t phys = vmm_get_physical(proc->page_table, va);
            if (!phys) {
                continue;
            }

            vmm_unmap_page(proc->page_table, va);
            pmm_free((void*)(uintptr_t)phys);
        }
    }

    proc->brk_current = addr;
    return (int64_t)addr;
}

int64_t sys_getpid(void) {
    process_t* proc = process_current();
    if (!proc) {
        return -1;
    }

    return (int64_t)proc->pid;
}

static __attribute__((noreturn)) void exec_replace_current(process_t* current,
                                                           process_t* next,
                                                           const char* kernel_path) {
    process_t* parent = NULL;

    if (serial_is_ready()) {
        serial_kprintf("[sys_exec] pid=%u -> pid=%u path=%s rip=%p rsp=%p kstack=%p\n",
                       current->pid,
                       next->pid,
                       kernel_path,
                       (void*)(uintptr_t)next->context.rip,
                       (void*)(uintptr_t)next->context.rsp,
                       (void*)(uintptr_t)next->kernel_stack_top);
    }

    parent = process_by_pid(current->ppid);
    next->ppid = current->ppid;
    current->exec_replaced_by_pid = next->pid;
    if (parent &&
        parent->state == PROC_BLOCKED &&
        parent->wait_target_pid != WAIT_ANY_PID &&
        parent->wait_target_pid == current->pid) {
        parent->wait_target_pid = next->pid;
    }

    current->state = PROC_ZOMBIE;
    process_close_files(current);
    scheduler_add(next);
    scheduler_switch();
    __builtin_unreachable();
}

int64_t sys_exec(const char* path) {
    process_t* current = process_current();
    process_t* next = NULL;
    char kernel_path[SYSCALL_PATH_MAX];

    if (!current) {
        return -1;
    }

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    if (serial_is_ready()) {
        serial_kprintf("[sys_exec] pid=%u loading path=%s\n",
                       current->pid,
                       kernel_path);
    }

    next = kxe_load(kernel_path);
    if (!next) {
        return -1;
    }

    exec_replace_current(current, next, kernel_path);
}

int64_t sys_spawn(const char* path) {
    process_t* current = process_current();
    process_t* child = NULL;
    char kernel_path[SYSCALL_PATH_MAX];

    if (!current) {
        return -1;
    }

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    child = kxe_load(kernel_path);
    if (!child) {
        return -1;
    }

    child->ppid = current->pid;
    scheduler_add(child);
    return (int64_t)child->pid;
}

int64_t sys_exec_argv(const char* path, uint64_t argc, const char* const* argv) {
    process_t* current = process_current();
    process_t* next = NULL;
    char kernel_path[SYSCALL_PATH_MAX];
    char arg_storage[KXE_MAX_ARGC][KXE_ARG_MAX];
    const char* arg_ptrs[KXE_MAX_ARGC];

    if (!current || argc == 0 || argc > KXE_MAX_ARGC) {
        return -1;
    }

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path)) ||
        !copy_user_argv(argv, argc, arg_storage, arg_ptrs)) {
        return -1;
    }

    if (serial_is_ready()) {
        serial_kprintf("[sys_exec] pid=%u loading path=%s argc=%u\n",
                       current->pid,
                       kernel_path,
                       (uint32_t)argc);
    }

    next = kxe_load_argv(kernel_path, argc, arg_ptrs);
    if (!next) {
        return -1;
    }

    exec_replace_current(current, next, kernel_path);
}

int64_t sys_spawn_argv(const char* path, uint64_t argc, const char* const* argv) {
    process_t* current = process_current();
    process_t* child = NULL;
    char kernel_path[SYSCALL_PATH_MAX];
    char arg_storage[KXE_MAX_ARGC][KXE_ARG_MAX];
    const char* arg_ptrs[KXE_MAX_ARGC];

    if (!current || argc == 0 || argc > KXE_MAX_ARGC) {
        return -1;
    }

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path)) ||
        !copy_user_argv(argv, argc, arg_storage, arg_ptrs)) {
        return -1;
    }

    child = kxe_load_argv(kernel_path, argc, arg_ptrs);
    if (!child) {
        return -1;
    }

    child->ppid = current->pid;
    scheduler_add(child);
    return (int64_t)child->pid;
}

int64_t sys_waitpid(int pid, int* status) {
    process_t* current = process_current();
    process_t* child = NULL;
    volatile int requested_pid = pid;

    if (!current || (pid <= 0 && pid != -1)) {
        return -1;
    }

    if (status &&
        !validate_user_buffer((uint64_t)(uintptr_t)status, sizeof(*status), true)) {
        return -1;
    }

    for (;;) {
        int target_pid = requested_pid;

        if (current->wait_target_pid != 0) {
            target_pid = (current->wait_target_pid == WAIT_ANY_PID)
                ? -1
                : (int)current->wait_target_pid;
        }

        if (target_pid == -1) {
            child = process_first_child(current->pid, true);
        } else {
            child = process_child_lookup(current, (uint32_t)target_pid);
        }
        if (!child) {
            current->wait_target_pid = 0;
            return -1;
        }

        if (child->state == PROC_ZOMBIE && child->exec_replaced_by_pid != 0) {
            target_pid = (int)child->exec_replaced_by_pid;
            current->wait_target_pid = (uint32_t)target_pid;
            process_destroy(child);
            continue;
        }

        if (child->state == PROC_ZOMBIE) {
            int64_t done_pid = child->pid;
            if (status) {
                *status = child->exit_code;
            }
            current->wait_target_pid = 0;
            process_destroy(child);
            return done_pid;
        }

        current->state = PROC_BLOCKED;
        current->wait_target_pid = (target_pid == -1) ? WAIT_ANY_PID : (uint32_t)target_pid;
        if (scheduler_checkpoint_process_kernel(current) == 0) {
            scheduler_switch();
            __builtin_unreachable();
        }
    }
}

static void kiwi_stat_from_vfs(const vfs_stat_t* st, kiwi_stat_t* out) {
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)st->type;
    out->ino = st->ino;
    out->size = st->size;
    out->mode = st->mode;
    out->link_count = st->link_count;
    out->mtime = st->mtime;
    out->ctime = st->ctime;
}

int64_t sys_stat(const char* path, kiwi_stat_t* out) {
    char kernel_path[SYSCALL_PATH_MAX];
    vnode_t* vn = NULL;
    vfs_stat_t st;

    if (!out) {
        return -1;
    }

    if (!validate_user_buffer((uint64_t)(uintptr_t)out, sizeof(*out), true)) {
        return -1;
    }

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    if (!vfs_resolve(kernel_path, &vn) || !vn) {
        return -1;
    }

    if (!vn->ops || !vn->ops->stat || !vn->ops->stat(vn, &st)) {
        vfs_vnode_put(vn);
        return -1;
    }

    {
        kiwi_stat_t user_st;
        kiwi_stat_from_vfs(&st, &user_st);
        memcpy(out, &user_st, sizeof(user_st));
    }

    vfs_vnode_put(vn);
    return 0;
}

int64_t sys_readdir(const char* path, uint64_t index, kiwi_dirent_t* out) {
    char kernel_path[SYSCALL_PATH_MAX];
    readdir_index_ctx_t ctx;

    if (!out) {
        return -1;
    }

    if (!validate_user_buffer((uint64_t)(uintptr_t)out, sizeof(*out), true)) {
        return -1;
    }

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.want_index = index;
    if (!vfs_readdir(kernel_path, readdir_index_cb, &ctx)) {
        return -1;
    }

    if (!ctx.found) {
        return 0;
    }

    memcpy(out, &ctx.ent, sizeof(ctx.ent));
    return 1;
}

int64_t sys_mkdir(const char* path, uint32_t mode) {
    char kernel_path[SYSCALL_PATH_MAX];

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    return vfs_mkdir(kernel_path, mode) ? 0 : -1;
}

int64_t sys_unlink(const char* path) {
    char kernel_path[SYSCALL_PATH_MAX];

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    return vfs_unlink(kernel_path) ? 0 : -1;
}

int64_t sys_chdir(const char* path) {
    process_t* proc = process_current();
    char kernel_path[SYSCALL_PATH_MAX];
    vnode_t* vn = NULL;

    if (!proc || !copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    if (!vfs_resolve(kernel_path, &vn) || !vn || vn->type != VNODE_DIR) {
        if (vn) {
            vfs_vnode_put(vn);
        }
        return -1;
    }

    vfs_vnode_put(vn);
    memcpy(proc->cwd, kernel_path, strlen(kernel_path) + 1u);
    return 0;
}

int64_t sys_getcwd(char* buf, uint64_t size) {
    process_t* proc = process_current();
    uint64_t len = 0;

    if (!proc || !buf || size == 0) {
        return -1;
    }

    len = strlen(proc->cwd) + 1u;
    if (len > size || !validate_user_buffer((uint64_t)(uintptr_t)buf, len, true)) {
        return -1;
    }

    memcpy(buf, proc->cwd, (size_t)len);
    return 0;
}

int64_t sys_truncate(const char* path, uint64_t size) {
    char kernel_path[SYSCALL_PATH_MAX];
    vnode_t* vn = NULL;
    bool ok = false;

    if (!copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    if (!vfs_resolve(kernel_path, &vn) || !vn || vn->type != VNODE_FILE || !vn->ops || !vn->ops->truncate) {
        if (vn) {
            vfs_vnode_put(vn);
        }
        return -1;
    }

    if (vn->mount && vn->mount->readonly) {
        vfs_vnode_put(vn);
        return -1;
    }

    ok = vn->ops->truncate(vn, size);
    vfs_vnode_put(vn);
    return ok ? 0 : -1;
}

int64_t sys_ftruncate(int fd, uint64_t size) {
    process_t* proc = process_current();
    process_fd_t* entry = NULL;

    if (!proc) {
        return -1;
    }

    entry = process_fd_lookup(proc, fd);
    if (!entry || !entry->vnode || entry->vnode->type != VNODE_FILE ||
        !entry->vnode->ops || !entry->vnode->ops->truncate || !fd_can_write(entry)) {
        return -1;
    }

    if (entry->vnode->mount && entry->vnode->mount->readonly) {
        return -1;
    }

    return entry->vnode->ops->truncate(entry->vnode, size) ? 0 : -1;
}

int64_t sys_fstat(int fd, kiwi_stat_t* out) {
    process_t* proc = process_current();
    process_fd_t* entry = NULL;
    vfs_stat_t st;
    kiwi_stat_t user_st;

    if (!proc || !out ||
        !validate_user_buffer((uint64_t)(uintptr_t)out, sizeof(*out), true)) {
        return -1;
    }

    entry = process_fd_lookup_used(proc, fd);
    if (entry && entry->is_pipe && entry->pipe) {
        memset(&user_st, 0, sizeof(user_st));
        user_st.type = KIWI_VNODE_PIPE;
        user_st.mode = 0600u;
        user_st.size = entry->pipe->count;
        user_st.link_count = 1;
        memcpy(out, &user_st, sizeof(user_st));
        return 0;
    }

    if (!entry) {
        entry = process_fd_lookup(proc, fd);
    }
    if (!entry || entry->is_console || !entry->vnode || !entry->vnode->ops || !entry->vnode->ops->stat) {
        return -1;
    }

    if (!entry->vnode->ops->stat(entry->vnode, &st)) {
        return -1;
    }

    kiwi_stat_from_vfs(&st, &user_st);
    memcpy(out, &user_st, sizeof(user_st));
    return 0;
}

int64_t sys_access(const char* path, int mode) {
    char kernel_path[SYSCALL_PATH_MAX];
    vnode_t* vn = NULL;
    vfs_stat_t st;

    if ((mode & ~7) != 0 || !copy_user_path_resolved(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    if (!vfs_resolve(kernel_path, &vn) || !vn) {
        return -1;
    }

    if (!vn->ops || !vn->ops->stat || !vn->ops->stat(vn, &st)) {
        vfs_vnode_put(vn);
        return -1;
    }

    vfs_vnode_put(vn);
    if ((mode & 4) && (st.mode & 0444u) == 0) return -1;
    if ((mode & 2) && (st.mode & 0222u) == 0) return -1;
    if ((mode & 1) && (st.mode & 0111u) == 0) return -1;
    return 0;
}

int64_t sys_rename(const char* old_path, const char* new_path) {
    char old_kernel[SYSCALL_PATH_MAX];
    char new_kernel[SYSCALL_PATH_MAX];
    vnode_t* src = NULL;
    vnode_t* dst = NULL;
    vfs_stat_t st;
    vnode_t* created = NULL;
    uint8_t buffer[256];
    uint64_t offset = 0;
    bool ok = false;

    if (!copy_user_path_resolved(old_path, old_kernel, sizeof(old_kernel)) ||
        !copy_user_path_resolved(new_path, new_kernel, sizeof(new_kernel))) {
        return -1;
    }

    if (strcmp(old_kernel, new_kernel) == 0) {
        return 0;
    }

    if (!vfs_resolve(old_kernel, &src) || !src || src->type != VNODE_FILE ||
        !src->ops || !src->ops->read || !src->ops->stat) {
        if (src) {
            vfs_vnode_put(src);
        }
        return -1;
    }

    if (!src->ops->stat(src, &st)) {
        vfs_vnode_put(src);
        return -1;
    }

    if (vfs_resolve(new_kernel, &dst) && dst) {
        if (dst->type != VNODE_FILE) {
            vfs_vnode_put(dst);
            vfs_vnode_put(src);
            return -1;
        }
        vfs_vnode_put(dst);
        if (!vfs_unlink(new_kernel)) {
            vfs_vnode_put(src);
            return -1;
        }
    }

    if (!vfs_create(new_kernel, st.mode ? st.mode : 0644u, &created) || !created ||
        !created->ops || !created->ops->write) {
        if (created) {
            vfs_vnode_put(created);
        }
        vfs_vnode_put(src);
        return -1;
    }

    ok = true;
    while (offset < src->size) {
        uint64_t want = src->size - offset;
        int64_t nread = 0;
        int64_t nwritten = 0;

        if (want > sizeof(buffer)) {
            want = sizeof(buffer);
        }

        nread = src->ops->read(src, offset, buffer, want);
        if (nread < 0 || (uint64_t)nread != want) {
            ok = false;
            break;
        }

        nwritten = created->ops->write(created, offset, buffer, want);
        if (nwritten < 0 || (uint64_t)nwritten != want) {
            ok = false;
            break;
        }

        offset += want;
    }

    vfs_vnode_put(created);
    if (!ok || !vfs_unlink(old_kernel)) {
        (void)vfs_unlink(new_kernel);
        vfs_vnode_put(src);
        return -1;
    }

    vfs_vnode_put(src);
    return 0;
}

int64_t sys_mount(const char* source, const char* target) {
    char kernel_source[SYSCALL_PATH_MAX];
    char kernel_target[SYSCALL_PATH_MAX];
    block_device_t* dev = NULL;

    if (!copy_user_string(source, kernel_source, sizeof(kernel_source)) ||
        !copy_user_path_resolved(target, kernel_target, sizeof(kernel_target))) {
        return -1;
    }

    dev = resolve_mount_device_spec(kernel_source);
    if (!dev) {
        (void)block_rescan();
        dev = resolve_mount_device_spec(kernel_source);
    }
    if (!dev) {
        return -1;
    }

    return vfs_mount_dev(kernel_target, dev) ? 0 : -1;
}

int64_t sys_dev_rescan(void) {
    return (int64_t)block_rescan();
}

int64_t sys_ioctl(int fd, uint64_t request, void* arg) {
    process_t* proc = process_current();
    process_fd_t scratch;
    process_fd_t* src = NULL;

    if (!proc || !process_fd_source(proc, fd, &scratch, &src) || !src->is_console) {
        return -1;
    }

    switch (request) {
        case KIWI_IOCTL_TIOCGWINSZ: {
            kiwi_winsize_t ws;
            uint32_t rows = 0;
            uint32_t cols = 0;

            if (!arg || !validate_user_buffer((uint64_t)(uintptr_t)arg, sizeof(ws), true)) {
                return -1;
            }

            console_get_size(&rows, &cols);
            memset(&ws, 0, sizeof(ws));
            ws.ws_row = (uint16_t)rows;
            ws.ws_col = (uint16_t)cols;
            memcpy(arg, &ws, sizeof(ws));
            return 0;
        }
        case KIWI_IOCTL_FIONREAD: {
            int available = 0;

            if (!arg || !validate_user_buffer((uint64_t)(uintptr_t)arg, sizeof(available), true)) {
                return -1;
            }

            if (proc) {
                if (proc->stdin_pending_len > proc->stdin_pending_pos) {
                    available += (int)(proc->stdin_pending_len - proc->stdin_pending_pos);
                }
                available += (int)keyboard_pending_count();
            }

            memcpy(arg, &available, sizeof(available));
            return 0;
        }
        case KIWI_IOCTL_TCGETS: {
            kiwi_termios_t tio;
            process_t* proc = process_current();

            if (!arg || !validate_user_buffer((uint64_t)(uintptr_t)arg, sizeof(tio), true)) {
                return -1;
            }

            memset(&tio, 0, sizeof(tio));
            tio.c_lflag = (proc && proc->tty_raw_mode) ? 0u : (0x00000001u | 0x00000002u | 0x00000004u);
            tio.c_oflag = 0x00000001u;
            tio.c_cflag = 0x00000030u;
            memcpy(arg, &tio, sizeof(tio));
            return 0;
        }
        case KIWI_IOCTL_TCSETS:
        case KIWI_IOCTL_TCSETSW:
        case KIWI_IOCTL_TCSETSF: {
            kiwi_termios_t tio;
            process_t* proc = process_current();

            if (!arg || !validate_user_buffer((uint64_t)(uintptr_t)arg, sizeof(kiwi_termios_t), false)) {
                return -1;
            }
            memcpy(&tio, arg, sizeof(tio));
            if (proc) {
                proc->tty_raw_mode = ((tio.c_lflag & 0x00000002u) == 0u) ? 1u : 0u;
            }
            return 0;
        }
        default:
            return -1;
    }
}

int64_t sys_console_input(const char* prefix,
                          const char* text,
                          uint64_t text_len,
                          uint64_t cursor_pos,
                          uint64_t show_cursor) {
    char kernel_prefix[SYSCALL_PATH_MAX + 32u];
    char kernel_text[SYSCALL_PATH_MAX];

    if (!prefix || !text || text_len >= sizeof(kernel_text) || cursor_pos > text_len) {
        return -1;
    }

    if (!copy_user_string(prefix, kernel_prefix, sizeof(kernel_prefix))) {
        return -1;
    }

    if (!validate_user_buffer((uint64_t)(uintptr_t)text, text_len, false)) {
        return -1;
    }

    memcpy(kernel_text, text, (size_t)text_len);
    kernel_text[text_len] = '\0';
    console_set_input_line(kernel_prefix,
                           kernel_text,
                           (uint32_t)text_len,
                           (uint32_t)cursor_pos,
                           show_cursor != 0);
    return 0;
}

int64_t sys_console_clear(void) {
    console_clear();
    return 0;
}

int64_t sys_seek(int fd, int64_t offset, int whence) {
    process_t* proc = process_current();
    process_fd_t* entry = NULL;
    uint64_t base = 0;
    uint64_t new_offset = 0;

    if (!proc) {
        return -1;
    }

    entry = process_fd_lookup(proc, fd);
    if (!entry || !entry->vnode) {
        return -1;
    }

    switch (whence) {
        case KIWI_SEEK_SET:
            base = 0;
            break;
        case KIWI_SEEK_CUR:
            base = entry->offset;
            break;
        case KIWI_SEEK_END:
            base = entry->vnode->size;
            break;
        default:
            return -1;
    }

    if (!add_signed_offset(base, offset, &new_offset) || new_offset > INT64_MAX) {
        return -1;
    }

    process_fd_sync_offset(proc, entry, new_offset);
    return (int64_t)new_offset;
}

int64_t sys_yield(syscall_frame_t* frame) {
    process_t* proc = process_current();
    if (!proc || !frame) {
        return 0;
    }

    scheduler_save_syscall_context(proc, frame, 0);
    scheduler_add(proc);
    scheduler_switch();
    __builtin_unreachable();
}

int64_t syscall_dispatch(syscall_frame_t* frame) {
    if (!frame) {
        return -1;
    }

    switch (frame->syscall_num) {
        case KIWI_SYS_EXIT:
            sys_exit((int)frame->rdi);
            __builtin_unreachable();
        case KIWI_SYS_WRITE:
            return sys_write((int)frame->rdi, (const void*)frame->rsi, frame->rdx);
        case KIWI_SYS_READ:
            return sys_read((int)frame->rdi, (void*)frame->rsi, frame->rdx);
        case KIWI_SYS_OPEN:
            return sys_open((const char*)frame->rdi, (int)frame->rsi);
        case KIWI_SYS_CLOSE:
            return sys_close((int)frame->rdi);
        case KIWI_SYS_BRK:
            return sys_brk(frame->rdi);
        case KIWI_SYS_GETPID:
            return sys_getpid();
        case KIWI_SYS_EXEC:
            return sys_exec((const char*)frame->rdi);
        case KIWI_SYS_SPAWN:
            return sys_spawn((const char*)frame->rdi);
        case KIWI_SYS_EXEC_ARGV:
            return sys_exec_argv((const char*)frame->rdi, frame->rsi, (const char* const*)frame->rdx);
        case KIWI_SYS_SPAWN_ARGV:
            return sys_spawn_argv((const char*)frame->rdi, frame->rsi, (const char* const*)frame->rdx);
        case KIWI_SYS_WAITPID:
            return sys_waitpid((int)frame->rdi, (int*)frame->rsi);
        case KIWI_SYS_STAT:
            return sys_stat((const char*)frame->rdi, (kiwi_stat_t*)frame->rsi);
        case KIWI_SYS_READDIR:
            return sys_readdir((const char*)frame->rdi, frame->rsi, (kiwi_dirent_t*)frame->rdx);
        case KIWI_SYS_SEEK:
            return sys_seek((int)frame->rdi, (int64_t)frame->rsi, (int)frame->rdx);
        case KIWI_SYS_YIELD:
            return sys_yield(frame);
        case KIWI_SYS_MKDIR:
            return sys_mkdir((const char*)frame->rdi, (uint32_t)frame->rsi);
        case KIWI_SYS_UNLINK:
            return sys_unlink((const char*)frame->rdi);
        case KIWI_SYS_CONSOLE_INPUT:
            return sys_console_input((const char*)frame->rdi,
                                     (const char*)frame->rsi,
                                     frame->rdx,
                                     frame->r10,
                                     frame->r8);
        case KIWI_SYS_CONSOLE_CLEAR:
            return sys_console_clear();
        case KIWI_SYS_MOUNT:
            return sys_mount((const char*)frame->rdi, (const char*)frame->rsi);
        case KIWI_SYS_DEV_RESCAN:
            return sys_dev_rescan();
        case KIWI_SYS_IOCTL:
            return sys_ioctl((int)frame->rdi, frame->rsi, (void*)frame->rdx);
        case KIWI_SYS_CHDIR:
            return sys_chdir((const char*)frame->rdi);
        case KIWI_SYS_GETCWD:
            return sys_getcwd((char*)frame->rdi, frame->rsi);
        case KIWI_SYS_RENAME:
            return sys_rename((const char*)frame->rdi, (const char*)frame->rsi);
        case KIWI_SYS_TRUNCATE:
            return sys_truncate((const char*)frame->rdi, frame->rsi);
        case KIWI_SYS_FTRUNCATE:
            return sys_ftruncate((int)frame->rdi, frame->rsi);
        case KIWI_SYS_FSTAT:
            return sys_fstat((int)frame->rdi, (kiwi_stat_t*)frame->rsi);
        case KIWI_SYS_ACCESS:
            return sys_access((const char*)frame->rdi, (int)frame->rsi);
        case KIWI_SYS_DUP:
            return sys_dup((int)frame->rdi);
        case KIWI_SYS_DUP2:
            return sys_dup2((int)frame->rdi, (int)frame->rsi);
        case KIWI_SYS_FCNTL:
            return sys_fcntl((int)frame->rdi, (int)frame->rsi, frame->rdx);
        case KIWI_SYS_PIPE:
            return sys_pipe((int*)frame->rdi);
        case KIWI_SYS_CLOCK_GETTIME:
            return sys_clock_gettime((int)frame->rdi, (kiwi_timespec_t*)frame->rsi);
        case KIWI_SYS_POWEROFF:
            return sys_poweroff();
        case KIWI_SYS_REBOOT:
            return sys_reboot();
        default:
            return -1;
    }
}
