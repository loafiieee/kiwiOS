#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include <stdint.h>
#include "arch/x86/idt.h"
#include "core/process.h"
#include "core/syscall.h"

extern int scheduler_checkpoint_asm(process_kernel_context_t* ctx)
    __attribute__((returns_twice));

#define scheduler_checkpoint_process_kernel(proc) \
    ((proc) ? ((proc)->resume_kind = PROC_RESUME_KERNEL, \
               scheduler_checkpoint_asm(&(proc)->kernel_context)) : 1)

void scheduler_add(process_t* proc);
void scheduler_run(process_t* proc);
void scheduler_save_syscall_context(process_t* proc,
                                    const syscall_frame_t* frame,
                                    int64_t return_value);
void scheduler_timer_tick(const interrupt_frame_t* frame);
__attribute__((noreturn)) void scheduler_switch(void);

#endif // CORE_SCHEDULER_H
