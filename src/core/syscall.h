#ifndef CORE_SYSCALL_H
#define CORE_SYSCALL_H

#include <stdbool.h>
#include <stdint.h>
#include "abi/kiwi.h"

typedef struct {
    uint64_t syscall_num;   // RAX
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t user_rip;      // RCX on SYSCALL entry
    uint64_t user_rflags;   // R11 on SYSCALL entry
    uint64_t user_rsp;      // saved manually by the asm stub
} syscall_frame_t;

int64_t syscall_dispatch(syscall_frame_t* frame);
bool validate_user_buffer(uint64_t addr, uint64_t len, bool needs_write);
bool copy_user_string(const char* user_str, char* kernel_buf, uint64_t kernel_buf_size);
int64_t sys_read(int fd, void* buf, uint64_t len);
int64_t sys_open(const char* path, int flags);
int64_t sys_close(int fd);
int64_t sys_brk(uint64_t addr);
int64_t sys_getpid(void);
int64_t sys_exec(const char* path);
int64_t sys_stat(const char* path, kiwi_stat_t* out);
int64_t sys_seek(int fd, int64_t offset, int whence);
int64_t sys_yield(void);
int64_t sys_write(int fd, const void* buf, uint64_t len);
void sys_exit(int code) __attribute__((noreturn));

#endif // CORE_SYSCALL_H
