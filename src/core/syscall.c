#include <stddef.h>
#include <stdint.h>
#include "core/syscall.h"
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
#define USER_HEAP_MAX (KXE_USER_STACK_TOP - ((uint64_t)KXE_USER_STACK_PAGES * PAGE_SIZE))

static process_fd_t* process_fd_lookup(process_t* proc, int fd) {
    if (!proc || fd < FD_FIRST_USER || fd >= PROC_MAX_FDS) {
        return NULL;
    }

    if (!proc->fds[fd].used || !proc->fds[fd].vnode) {
        return NULL;
    }

    return &proc->fds[fd];
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
        proc->fds[i].vnode = vnode;
        proc->fds[i].offset = offset;
        return (int)i;
    }

    return -1;
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

static void stdin_queue_escape(process_t* proc, char code) {
    if (!proc) {
        return;
    }

    proc->stdin_pending[0] = 0x1Bu;
    proc->stdin_pending[1] = '[';
    proc->stdin_pending[2] = (uint8_t)code;
    proc->stdin_pending_len = 3u;
    proc->stdin_pending_pos = 0u;
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

    if (fd == FD_STDOUT || fd == FD_STDERR) {
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

    entry = process_fd_lookup(proc, fd);
    if (!entry || !entry->vnode || !entry->vnode->ops || !entry->vnode->ops->write || !fd_can_write(entry)) {
        return -1;
    }

    nwritten = entry->vnode->ops->write(entry->vnode, entry->offset, buf, len);
    if (nwritten > 0) {
        entry->offset += (uint64_t)nwritten;
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

    if (fd == FD_STDIN) {
        char* out = (char*)buf;
        uint64_t i = 0;

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
                console_page_up();
                continue;
            }
            if (ch == KEY_PAGE_DOWN) {
                console_page_down();
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

            out[i] = (char)ch;
            if (out[i] == '\n') {
                return (int64_t)(i + 1u);
            }
            i++;
        }
        return (int64_t)len;
    }

    entry = process_fd_lookup(proc, fd);
    if (!entry || !entry->vnode || !entry->vnode->ops || !entry->vnode->ops->read || !fd_can_read(entry)) {
        return -1;
    }

    nread = entry->vnode->ops->read(entry->vnode, entry->offset, buf, len);
    if (nread > 0) {
        entry->offset += (uint64_t)nread;
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

    if (!copy_user_string(path, kernel_path, sizeof(kernel_path))) {
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

    entry = process_fd_lookup(proc, fd);
    if (!entry) {
        return -1;
    }

    vfs_vnode_put(entry->vnode);
    memset(entry, 0, sizeof(*entry));
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
            parent->wait_target_pid == proc->pid) {
            scheduler_add(parent);
        }

        print(NULL, "\n[sys_exit] pid=");
        print_u32(NULL, proc->pid);
        print(NULL, " exited with code ");
        print_u64(NULL, (uint64_t)(uint32_t)code);
        print(NULL, "\n");
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

int64_t sys_exec(const char* path) {
    process_t* current = process_current();
    process_t* parent = NULL;
    process_t* next = NULL;
    char kernel_path[SYSCALL_PATH_MAX];

    if (!current) {
        return -1;
    }

    if (!copy_user_string(path, kernel_path, sizeof(kernel_path))) {
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
        parent->wait_target_pid == current->pid) {
        parent->wait_target_pid = next->pid;
    }

    current->state = PROC_ZOMBIE;
    process_close_files(current);
    scheduler_add(next);
    scheduler_switch();
    __builtin_unreachable();
}

int64_t sys_spawn(const char* path) {
    process_t* current = process_current();
    process_t* child = NULL;
    char kernel_path[SYSCALL_PATH_MAX];

    if (!current) {
        return -1;
    }

    if (!copy_user_string(path, kernel_path, sizeof(kernel_path))) {
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

int64_t sys_waitpid(int pid, int* status) {
    process_t* current = process_current();
    process_t* child = NULL;
    int target_pid = pid;

    if (!current || pid <= 0) {
        return -1;
    }

    if (status &&
        !validate_user_buffer((uint64_t)(uintptr_t)status, sizeof(*status), true)) {
        return -1;
    }

    for (;;) {
        if (current->wait_target_pid != 0) {
            target_pid = (int)current->wait_target_pid;
        }

        child = process_child_lookup(current, (uint32_t)target_pid);
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
            if (status) {
                *status = child->exit_code;
            }
            current->wait_target_pid = 0;
            process_destroy(child);
            return pid;
        }

        current->state = PROC_BLOCKED;
        current->wait_target_pid = (uint32_t)target_pid;
        if (scheduler_checkpoint_process_kernel(current) == 0) {
            scheduler_switch();
            __builtin_unreachable();
        }
    }
}

int64_t sys_stat(const char* path, kiwi_stat_t* out) {
    char kernel_path[SYSCALL_PATH_MAX];
    vnode_t* vn = NULL;
    vfs_stat_t st;
    kiwi_stat_t user_st;

    if (!out) {
        return -1;
    }

    if (!validate_user_buffer((uint64_t)(uintptr_t)out, sizeof(*out), true)) {
        return -1;
    }

    if (!copy_user_string(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    if (!vfs_resolve(kernel_path, &vn) || !vn) {
        return -1;
    }

    if (!vn->ops || !vn->ops->stat || !vn->ops->stat(vn, &st)) {
        vfs_vnode_put(vn);
        return -1;
    }

    memset(&user_st, 0, sizeof(user_st));
    user_st.type = (uint32_t)st.type;
    user_st.ino = st.ino;
    user_st.size = st.size;
    user_st.mode = st.mode;
    user_st.link_count = st.link_count;
    user_st.mtime = st.mtime;
    user_st.ctime = st.ctime;

    memcpy(out, &user_st, sizeof(user_st));
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

    if (!copy_user_string(path, kernel_path, sizeof(kernel_path))) {
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

    if (!copy_user_string(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    return vfs_mkdir(kernel_path, mode) ? 0 : -1;
}

int64_t sys_unlink(const char* path) {
    char kernel_path[SYSCALL_PATH_MAX];

    if (!copy_user_string(path, kernel_path, sizeof(kernel_path))) {
        return -1;
    }

    return vfs_unlink(kernel_path) ? 0 : -1;
}

int64_t sys_mount(const char* source, const char* target) {
    char kernel_source[SYSCALL_PATH_MAX];
    char kernel_target[SYSCALL_PATH_MAX];
    block_device_t* dev = NULL;

    if (!copy_user_string(source, kernel_source, sizeof(kernel_source)) ||
        !copy_user_string(target, kernel_target, sizeof(kernel_target))) {
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

    entry->offset = new_offset;
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
        default:
            return -1;
    }
}
