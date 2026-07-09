#ifndef CORE_PROCESS_H
#define CORE_PROCESS_H

#include <stdbool.h>
#include <stdint.h>
#include "memory/vmm.h"

#define PROC_MAX 64
#define PROC_NAME_MAX 64
#define PROC_MAX_FDS 16
#define PROC_CWD_MAX 256

struct process_pipe;
struct vnode;

typedef struct {
    uint8_t used;
    uint8_t is_console;
    uint8_t console_fd;
    uint8_t is_pipe;
    uint8_t pipe_read;
    uint8_t pipe_write;
    uint8_t fd_flags;
    uint8_t reserved[2];
    uint32_t flags;
    uint32_t description_id;
    struct vnode* vnode;
    struct process_pipe* pipe;
    uint64_t offset;
} process_fd_t;

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE,
} proc_state_t;

typedef struct {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} process_context_t;

typedef struct {
    uint64_t rsp;
    uint64_t rip;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t cr3;
    uint64_t rflags;
} process_kernel_context_t;

typedef enum {
    PROC_RESUME_USER = 0,
    PROC_RESUME_KERNEL,
} process_resume_kind_t;

typedef struct process {
    uint32_t pid;
    uint32_t ppid;
    uint32_t wait_target_pid;
    uint32_t exec_replaced_by_pid;
    char name[PROC_NAME_MAX];
    char cwd[PROC_CWD_MAX];
    proc_state_t state;
    int32_t exit_code;
    process_resume_kind_t resume_kind;

    page_table_t* page_table;

    process_context_t context;
    process_kernel_context_t kernel_context;

    uint64_t kernel_stack_phys;
    uint64_t kernel_stack_top;

    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t ticks_remaining;
    uint8_t stdin_pending[4];
    uint8_t stdin_pending_len;
    uint8_t stdin_pending_pos;
    uint8_t tty_raw_mode;
    uint8_t reserved0[1];

    process_fd_t fds[PROC_MAX_FDS];

    struct process* next;
} process_t;

process_t* process_create(const char* name);
void process_destroy(process_t* proc);
process_t* process_current(void);
process_t* process_by_pid(uint32_t pid);
process_t* process_first_child(uint32_t ppid, bool prefer_zombie);
void process_set_name(process_t* proc, const char* name);
void process_close_files(process_t* proc);
void process_reap_zombies(void);
bool process_has_blocked(void);
bool process_map_user_stack(process_t* proc, uint64_t stack_top, uint32_t page_count);
__attribute__((noreturn)) void process_enter(process_t* proc);
void process_set_current(process_t* proc);

#endif // CORE_PROCESS_H
