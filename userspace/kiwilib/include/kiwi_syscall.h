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

static inline int64_t sys_stat(const char* path, kiwi_stat_t* out) {
    uint64_t ret = KIWI_SYS_STAT;
    asm volatile(
        "syscall"
        : "+a"(ret)
        : "D"((uint64_t)(uintptr_t)path), "S"((uint64_t)(uintptr_t)out)
        : "rcx", "r11", "cc", "memory");
    return (int64_t)ret;
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

#endif // KIWILIB_KIWI_SYSCALL_H
