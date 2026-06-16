#ifndef KIWILIB_KIWI_SYSCALL_H
#define KIWILIB_KIWI_SYSCALL_H

#include <stdint.h>
#include "abi/kiwi.h"

static inline int64_t kiwi_syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret = num;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t kiwi_syscall5(uint64_t num,
                                    uint64_t arg1,
                                    uint64_t arg2,
                                    uint64_t arg3,
                                    uint64_t arg4,
                                    uint64_t arg5) {
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8 __asm__("r8") = arg5;
    uint64_t ret = num;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t kiwi_syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    uint64_t ret = num;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"(arg1), "S"(arg2)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_write(int fd, const void* buf, uint64_t len) {
    return kiwi_syscall3(KIWI_SYS_WRITE, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)buf, len);
}

static inline void sys_exit(int code) {
    uint64_t ret = KIWI_SYS_EXIT;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uint32_t)code)
        : "rcx", "r11", "cc", "memory");
    __builtin_unreachable();
}

static inline int64_t sys_read(int fd, void* buf, uint64_t len) {
    return kiwi_syscall3(KIWI_SYS_READ, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_open(const char* path, int flags) {
    uint64_t ret = KIWI_SYS_OPEN;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uintptr_t)path), "S"((uint64_t)(uint32_t)flags)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_close(int fd) {
    uint64_t ret = KIWI_SYS_CLOSE;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uint32_t)fd)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_brk(uint64_t addr) {
    uint64_t ret = KIWI_SYS_BRK;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"(addr)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_getpid(void) {
    uint64_t ret = KIWI_SYS_GETPID;
    asm volatile(
        "syscall"
        : "+a"(ret)
        :
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_exec(const char* path) {
    uint64_t ret = KIWI_SYS_EXEC;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uintptr_t)path)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_spawn(const char* path) {
    uint64_t ret = KIWI_SYS_SPAWN;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uintptr_t)path)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_waitpid(int pid, int* status) {
    return kiwi_syscall2(KIWI_SYS_WAITPID,
                         (uint64_t)(uint32_t)pid,
                         (uint64_t)(uintptr_t)status);
}

static inline int64_t sys_stat(const char* path, kiwi_stat_t* out) {
    uint64_t ret = KIWI_SYS_STAT;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uintptr_t)path), "S"((uint64_t)(uintptr_t)out)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_readdir(const char* path, uint64_t index, kiwi_dirent_t* out) {
    return kiwi_syscall3(KIWI_SYS_READDIR,
                         (uint64_t)(uintptr_t)path,
                         index,
                         (uint64_t)(uintptr_t)out);
}

static inline int64_t sys_seek(int fd, int64_t offset, int whence) {
    return kiwi_syscall3(KIWI_SYS_SEEK, (uint64_t)(uint32_t)fd, (uint64_t)offset, (uint64_t)(uint32_t)whence);
}

static inline int64_t sys_yield(void) {
    uint64_t ret = KIWI_SYS_YIELD;
    asm volatile(
        "syscall"
        : "+a"(ret)
        :
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_mkdir(const char* path, uint32_t mode) {
    return kiwi_syscall3(KIWI_SYS_MKDIR,
                         (uint64_t)(uintptr_t)path,
                         (uint64_t)mode,
                         0);
}

static inline int64_t sys_unlink(const char* path) {
    uint64_t ret = KIWI_SYS_UNLINK;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uintptr_t)path)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline int64_t sys_mount(const char* source, const char* target) {
    return kiwi_syscall2(KIWI_SYS_MOUNT,
                         (uint64_t)(uintptr_t)source,
                         (uint64_t)(uintptr_t)target);
}

static inline int64_t sys_dev_rescan(void) {
    uint64_t ret = KIWI_SYS_DEV_RESCAN;
    asm volatile(
        "syscall"
        : "+a"(ret)
        :
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

static inline uint64_t kiwi_cstr_len(const char* s) {
    uint64_t len = 0;
    if (!s) {
        return 0;
    }
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static inline int kiwi_path_has_slash(const char* s) {
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

static inline int kiwi_copy_cstr(const char* src, char* dst, uint64_t cap) {
    uint64_t i = 0;
    if (!src || !dst || cap == 0) {
        return -1;
    }
    while (src[i] != '\0') {
        if (i + 1 >= cap) {
            return -1;
        }
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 0;
}

static inline int kiwi_resolve_program_path(const char* cmd, char* out, uint64_t out_size) {
    kiwi_stat_t st;
    const char* root_prefix = "/";
    const char* bin_prefix = "/bin/";
    uint64_t cmd_len = 0;
    uint64_t prefix_len = 0;

    if (!cmd || !*cmd || !out || out_size < 2) {
        return -1;
    }

    if (cmd[0] == '/' || kiwi_path_has_slash(cmd)) {
        if (kiwi_copy_cstr(cmd, out, out_size) != 0) {
            return -1;
        }
        return (sys_stat(out, &st) == 0 && st.type == KIWI_VNODE_FILE) ? 0 : -1;
    }

    cmd_len = kiwi_cstr_len(cmd);

    prefix_len = kiwi_cstr_len(bin_prefix);
    if (prefix_len + cmd_len + 1 <= out_size) {
        kiwi_copy_cstr(bin_prefix, out, out_size);
        kiwi_copy_cstr(cmd, out + prefix_len, out_size - prefix_len);
        if (sys_stat(out, &st) == 0 && st.type == KIWI_VNODE_FILE) {
            return 0;
        }
    }

    prefix_len = kiwi_cstr_len(root_prefix);
    if (prefix_len + cmd_len + 1 <= out_size) {
        kiwi_copy_cstr(root_prefix, out, out_size);
        kiwi_copy_cstr(cmd, out + prefix_len, out_size - prefix_len);
        if (sys_stat(out, &st) == 0 && st.type == KIWI_VNODE_FILE) {
            return 0;
        }
    }

    return -1;
}

static inline int64_t sys_console_input(const char* prefix,
                                        const char* text,
                                        uint64_t text_len,
                                        uint64_t cursor_pos,
                                        int show_cursor) {
    return kiwi_syscall5(KIWI_SYS_CONSOLE_INPUT,
                         (uint64_t)(uintptr_t)prefix,
                         (uint64_t)(uintptr_t)text,
                         text_len,
                         cursor_pos,
                         (uint64_t)(uint32_t)show_cursor);
}

static inline int64_t sys_console_clear(void) {
    uint64_t ret = KIWI_SYS_CONSOLE_CLEAR;
    asm volatile(
        "syscall"
        : "+a"(ret)
        :
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
}

#endif // KIWILIB_KIWI_SYSCALL_H
