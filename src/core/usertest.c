#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/console.h"
#include "core/process.h"
#include "core/scheduler.h"
#include "core/usertest.h"
#include "libc/string.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#define USER_TEST_CODE_BASE   0x0000000000400000ull
#define USER_TEST_STACK_TOP   0x0000000000800000ull
#define USER_TEST_STACK_PAGES 16u
extern uint8_t user_test_program_start[];
extern uint8_t user_test_program_end[];

void usertest_run(struct limine_framebuffer* fb) {
    size_t program_size = (size_t)(user_test_program_end - user_test_program_start);
    if (program_size == 0 || program_size > PAGE_SIZE) {
        print(fb, "usertest: embedded program size is invalid\n");
        return;
    }

    process_t* proc = process_create("usertest");
    if (!proc) {
        print(fb, "usertest: failed to create process\n");
        return;
    }

    uint64_t code_phys = (uint64_t)(uintptr_t)pmm_alloc();
    if (!code_phys) {
        print(fb, "usertest: failed to allocate code page\n");
        process_destroy(proc);
        return;
    }

    void* code_virt = phys_to_virt(code_phys);
    memset(code_virt, 0, PAGE_SIZE);
    memcpy(code_virt, user_test_program_start, program_size);

    if (!vmm_map_page(proc->page_table, USER_TEST_CODE_BASE, code_phys, PAGE_PRESENT | PAGE_USER)) {
        print(fb, "usertest: failed to map code page\n");
        pmm_free((void*)(uintptr_t)code_phys);
        process_destroy(proc);
        return;
    }

    if (!process_map_user_stack(proc, USER_TEST_STACK_TOP, USER_TEST_STACK_PAGES)) {
        print(fb, "usertest: failed to map user stack\n");
        process_destroy(proc);
        return;
    }

    proc->context.rip = USER_TEST_CODE_BASE;
    proc->context.rsp = USER_TEST_STACK_TOP;
    proc->context.rflags = 0x202;
    proc->brk_base = PAGE_ALIGN_UP(USER_TEST_CODE_BASE + (uint64_t)program_size);
    proc->brk_current = proc->brk_base;

    print(fb, "usertest: scheduling ring 3 test...\n");
    scheduler_run(proc);
}
