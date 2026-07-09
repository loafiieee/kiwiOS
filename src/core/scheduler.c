#include "core/scheduler.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/syscall.h"
#include "arch/x86/tss.h"
#include "drivers/block/block.h"

#define SCHED_DEFAULT_TIME_SLICE 5u
#define SCHED_IDLE_HOTPLUG_POLL_TICKS 10u

extern void scheduler_restore_kernel_asm(const process_kernel_context_t* ctx)
    __attribute__((noreturn));

static process_t* g_run_queue_head = NULL;
static process_t* g_run_queue_tail = NULL;
static process_kernel_context_t* g_kernel_wait_ctx = NULL;
static bool g_kernel_wait_active = false;
static uint32_t g_idle_hotplug_ticks = 0;

static process_t* scheduler_take_next(void) {
    process_t* next = g_run_queue_head;
    if (!next) {
        return NULL;
    }

    g_run_queue_head = next->next;
    if (!g_run_queue_head) {
        g_run_queue_tail = NULL;
    }

    next->next = NULL;
    return next;
}

static void scheduler_reset_timeslice(process_t* proc) {
    if (!proc) {
        return;
    }

    proc->ticks_remaining = SCHED_DEFAULT_TIME_SLICE;
}

void scheduler_add(process_t* proc) {
    if (!proc || proc->state == PROC_UNUSED || proc->state == PROC_ZOMBIE) {
        return;
    }

    proc->state = PROC_READY;
    proc->next = NULL;

    if (g_run_queue_tail) {
        g_run_queue_tail->next = proc;
    } else {
        g_run_queue_head = proc;
    }
    g_run_queue_tail = proc;
}

void scheduler_save_syscall_context(process_t* proc,
                                    const syscall_frame_t* frame,
                                    int64_t return_value) {
    if (!proc || !frame) {
        return;
    }

    proc->context.rip = frame->user_rip;
    proc->context.rsp = frame->user_rsp;
    proc->context.rflags = frame->user_rflags;
    proc->context.rax = (uint64_t)return_value;
    proc->context.rbx = frame->rbx;
    proc->context.rbp = frame->rbp;
    proc->context.rdi = frame->rdi;
    proc->context.rsi = frame->rsi;
    proc->context.rdx = frame->rdx;
    proc->context.rcx = 0;
    proc->context.r8 = frame->r8;
    proc->context.r9 = frame->r9;
    proc->context.r10 = frame->r10;
    proc->context.r11 = 0;
    proc->context.r12 = frame->r12;
    proc->context.r13 = frame->r13;
    proc->context.r14 = frame->r14;
    proc->context.r15 = frame->r15;
    proc->resume_kind = PROC_RESUME_USER;
}

static void scheduler_save_interrupt_context(process_t* proc,
                                             const interrupt_frame_t* frame) {
    if (!proc || !frame) {
        return;
    }

    proc->context.rip = frame->rip;
    proc->context.rsp = frame->rsp;
    proc->context.rflags = frame->rflags;
    proc->context.rax = frame->rax;
    proc->context.rbx = frame->rbx;
    proc->context.rbp = frame->rbp;
    proc->context.rdi = frame->rdi;
    proc->context.rsi = frame->rsi;
    proc->context.rdx = frame->rdx;
    proc->context.rcx = frame->rcx;
    proc->context.r8 = frame->r8;
    proc->context.r9 = frame->r9;
    proc->context.r10 = frame->r10;
    proc->context.r11 = frame->r11;
    proc->context.r12 = frame->r12;
    proc->context.r13 = frame->r13;
    proc->context.r14 = frame->r14;
    proc->context.r15 = frame->r15;
    proc->resume_kind = PROC_RESUME_USER;
}

void scheduler_run(process_t* proc) {
    process_kernel_context_t wait_ctx;

    if (!proc) {
        return;
    }

    process_reap_zombies();
    scheduler_add(proc);
    g_kernel_wait_active = true;
    g_kernel_wait_ctx = &wait_ctx;

    if (scheduler_checkpoint_asm(&wait_ctx) != 0) {
        g_kernel_wait_ctx = NULL;
        g_kernel_wait_active = false;
        process_reap_zombies();
        return;
    }

    scheduler_switch();
}

void scheduler_timer_tick(const interrupt_frame_t* frame) {
    process_t* current = process_current();

    if (!frame || (uint64_t)(uintptr_t)frame < 0x1000u || !current) {
        return;
    }

    if ((frame->cs & 0x3u) != 0x3u) {
        return;
    }

    if (current->state != PROC_RUNNING || current->resume_kind != PROC_RESUME_USER) {
        return;
    }

    if (current->ticks_remaining > 0) {
        current->ticks_remaining--;
    }

    if (current->ticks_remaining != 0) {
        return;
    }

    scheduler_save_interrupt_context(current, frame);
    scheduler_add(current);
    scheduler_switch();
}

static process_t* scheduler_wait_for_next(void) {
    for (;;) {
        process_t* next = scheduler_take_next();
        if (next) {
            return next;
        }

        if (g_kernel_wait_active && !process_has_blocked()) {
            return NULL;
        }

        g_idle_hotplug_ticks++;
        if (g_idle_hotplug_ticks >= SCHED_IDLE_HOTPLUG_POLL_TICKS) {
            g_idle_hotplug_ticks = 0;
            asm volatile("sti");
            (void)block_poll_hotplug();
            asm volatile("cli");
            continue;
        }

        asm volatile("sti");
        asm volatile("hlt");
        asm volatile("cli");
    }
}

__attribute__((noreturn)) void scheduler_switch(void) {
    process_t* next = scheduler_wait_for_next();
    if (next) {
        scheduler_reset_timeslice(next);
        if (next->resume_kind == PROC_RESUME_KERNEL) {
            next->state = PROC_RUNNING;
            process_set_current(next);
            kernel_rsp_current = next->kernel_stack_top;
            tss_set_kernel_stack(next->kernel_stack_top);
            scheduler_restore_kernel_asm(&next->kernel_context);
        }

        process_enter(next);
    }

    if (g_kernel_wait_active && g_kernel_wait_ctx) {
        g_kernel_wait_active = false;
        process_set_current(NULL);
        kernel_rsp_current = 0;
        tss_set_kernel_stack(0);
        scheduler_restore_kernel_asm(g_kernel_wait_ctx);
    }

    g_kernel_wait_ctx = NULL;
    process_set_current(NULL);
    kernel_rsp_current = 0;
    tss_set_kernel_stack(0);

    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}
