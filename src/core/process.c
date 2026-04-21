#include "core/process.h"
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/syscall.h"
#include "arch/x86/tss.h"
#include "libc/string.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "vfs/vfs.h"

#define PROC_KERNEL_STACK_PAGES 2u
#define PTE_GET_ADDR(entry) ((entry) & 0x000FFFFFFFFFF000ULL)

static process_t g_procs[PROC_MAX];
static process_t* g_current = NULL;
static uint32_t g_next_pid = 1;

static void copy_name(char dst[PROC_NAME_MAX], const char* src) {
    uint32_t i = 0;
    if (!src) {
        src = "proc";
    }

    while (src[i] && i < (PROC_NAME_MAX - 1)) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void destroy_pt_level(uint64_t* table, int level) {
    if (!table || level <= 0) {
        return;
    }

    for (uint32_t i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & PAGE_PRESENT)) {
            continue;
        }

        uint64_t phys = PTE_GET_ADDR(entry);
        if (phys == 0) {
            table[i] = 0;
            continue;
        }

        if (level == 1) {
            pmm_free((void*)(uintptr_t)phys);
        } else {
            uint64_t* child = (uint64_t*)phys_to_virt(phys);
            destroy_pt_level(child, level - 1);
            pmm_free((void*)(uintptr_t)phys);
        }

        table[i] = 0;
    }
}

process_t* process_create(const char* name) {
    process_t* proc = NULL;

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (g_procs[i].state == PROC_UNUSED) {
            proc = &g_procs[i];
            break;
        }
    }

    if (!proc) {
        return NULL;
    }

    memset(proc, 0, sizeof(*proc));
    proc->pid = g_next_pid++;
    proc->ppid = g_current ? g_current->pid : 0;
    copy_name(proc->name, name);
    proc->state = PROC_READY;
    proc->exit_code = 0;

    proc->page_table = vmm_create_page_table();
    if (!proc->page_table) {
        memset(proc, 0, sizeof(*proc));
        return NULL;
    }

    proc->kernel_stack_phys = (uint64_t)(uintptr_t)pmm_alloc_pages(PROC_KERNEL_STACK_PAGES);
    if (!proc->kernel_stack_phys) {
        if (proc->page_table->pml4_phys) {
            pmm_free((void*)proc->page_table->pml4_phys);
        }
        kfree(proc->page_table);
        memset(proc, 0, sizeof(*proc));
        return NULL;
    }

    void* kernel_stack_virt = phys_to_virt(proc->kernel_stack_phys);
    memset(kernel_stack_virt, 0, PROC_KERNEL_STACK_PAGES * PAGE_SIZE);
    proc->kernel_stack_top = (uint64_t)(uintptr_t)kernel_stack_virt + (PROC_KERNEL_STACK_PAGES * PAGE_SIZE);

    return proc;
}

void process_set_name(process_t* proc, const char* name) {
    if (!proc) {
        return;
    }
    copy_name(proc->name, name);
}

void process_close_files(process_t* proc) {
    if (!proc) {
        return;
    }

    for (uint32_t i = 0; i < PROC_MAX_FDS; i++) {
        if (proc->fds[i].used && proc->fds[i].vnode) {
            vfs_vnode_put(proc->fds[i].vnode);
        }
        memset(&proc->fds[i], 0, sizeof(proc->fds[i]));
    }
}

void process_destroy(process_t* proc) {
    if (!proc || proc->state == PROC_UNUSED) {
        return;
    }

    if (g_current == proc) {
        g_current = NULL;
    }

    if (proc->page_table) {
        for (uint32_t pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
            uint64_t entry = proc->page_table->pml4_virt[pml4_idx];
            if (!(entry & PAGE_PRESENT)) {
                continue;
            }

            uint64_t pdpt_phys = PTE_GET_ADDR(entry);
            if (pdpt_phys != 0) {
                uint64_t* pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
                destroy_pt_level(pdpt, 3);
                pmm_free((void*)(uintptr_t)pdpt_phys);
            }

            proc->page_table->pml4_virt[pml4_idx] = 0;
        }

        if (proc->page_table->pml4_phys) {
            pmm_free((void*)proc->page_table->pml4_phys);
        }
        kfree(proc->page_table);
    }

    process_close_files(proc);

    if (proc->kernel_stack_phys) {
        pmm_free_pages((void*)(uintptr_t)proc->kernel_stack_phys, PROC_KERNEL_STACK_PAGES);
    }

    memset(proc, 0, sizeof(*proc));
}

process_t* process_current(void) {
    return g_current;
}

process_t* process_by_pid(uint32_t pid) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (g_procs[i].state != PROC_UNUSED && g_procs[i].pid == pid) {
            return &g_procs[i];
        }
    }

    return NULL;
}

void process_set_current(process_t* proc) {
    g_current = proc;
}

bool process_map_user_stack(process_t* proc, uint64_t stack_top, uint32_t page_count) {
    if (!proc || !proc->page_table || page_count == 0) {
        return false;
    }

    uint64_t stack_phys = (uint64_t)(uintptr_t)pmm_alloc_pages(page_count);
    if (!stack_phys) {
        return false;
    }

    void* stack_virt = phys_to_virt(stack_phys);
    memset(stack_virt, 0, (size_t)page_count * PAGE_SIZE);

    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t virt = stack_top - ((uint64_t)(page_count - i) * PAGE_SIZE);
        uint64_t phys = stack_phys + ((uint64_t)i * PAGE_SIZE);
        if (!vmm_map_page(proc->page_table, virt, phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITE)) {
            for (uint32_t j = 0; j < i; j++) {
                uint64_t undo_virt = stack_top - ((uint64_t)(page_count - j) * PAGE_SIZE);
                vmm_unmap_page(proc->page_table, undo_virt);
            }
            pmm_free_pages((void*)(uintptr_t)stack_phys, page_count);
            return false;
        }
    }

    return true;
}

__attribute__((noreturn)) void process_enter(process_t* proc) {
    if (!proc || !proc->page_table) {
        asm volatile("cli");
        for (;;) {
            asm volatile("hlt");
        }
    }

    proc->state = PROC_RUNNING;
    process_set_current(proc);
    kernel_rsp_current = proc->kernel_stack_top;
    tss_set_kernel_stack(proc->kernel_stack_top);

    asm volatile("cli");
    vmm_switch_page_table(proc->page_table);

    asm volatile(
        "pushq $0x1B\n"
        "pushq %[user_rsp]\n"
        "pushq %[user_rflags]\n"
        "pushq $0x23\n"
        "pushq %[entry]\n"
        "iretq\n"
        :
        : [user_rsp] "r"(proc->saved_rsp),
          [user_rflags] "r"(proc->saved_rflags),
          [entry] "r"(proc->saved_rip)
        : "memory"
    );

    __builtin_unreachable();
}
