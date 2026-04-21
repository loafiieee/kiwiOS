#ifndef CORE_PROCESS_H
#define CORE_PROCESS_H

#include <stdbool.h>
#include <stdint.h>
#include "memory/vmm.h"

#define PROC_MAX 64
#define PROC_NAME_MAX 64
#define PROC_MAX_FDS 16

struct vnode;

typedef struct {
    uint8_t used;
    uint8_t reserved[7];
    struct vnode* vnode;
    uint64_t offset;
} process_fd_t;

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE,
} proc_state_t;

typedef struct process {
    uint32_t pid;
    uint32_t ppid;
    char name[PROC_NAME_MAX];
    proc_state_t state;
    int32_t exit_code;

    page_table_t* page_table;

    uint64_t saved_rip;
    uint64_t saved_rsp;
    uint64_t saved_rflags;
    uint64_t saved_rax;
    uint64_t saved_rbx;
    uint64_t saved_rcx;
    uint64_t saved_rdx;
    uint64_t saved_rsi;
    uint64_t saved_rdi;
    uint64_t saved_rbp;
    uint64_t saved_r8;
    uint64_t saved_r9;
    uint64_t saved_r10;
    uint64_t saved_r11;
    uint64_t saved_r12;
    uint64_t saved_r13;
    uint64_t saved_r14;
    uint64_t saved_r15;

    uint64_t kernel_stack_phys;
    uint64_t kernel_stack_top;

    uint64_t brk_base;
    uint64_t brk_current;

    process_fd_t fds[PROC_MAX_FDS];

    struct process* next;
} process_t;

process_t* process_create(const char* name);
void process_destroy(process_t* proc);
process_t* process_current(void);
process_t* process_by_pid(uint32_t pid);
void process_set_name(process_t* proc, const char* name);
void process_close_files(process_t* proc);
bool process_map_user_stack(process_t* proc, uint64_t stack_top, uint32_t page_count);
__attribute__((noreturn)) void process_enter(process_t* proc);

/*
 * Temporary bring-up helper until scheduling/context switch code exists.
 * Phase 6 needs a notion of the current process before the scheduler lands.
 */
void process_set_current(process_t* proc);

#endif // CORE_PROCESS_H
