#include "arch/x86/tss.h"
#include <stddef.h>
#include <stdint.h>
#include "libc/string.h"

tss_t tss;

#define TSS_IST_STACK_BYTES 8192u

static uint8_t g_double_fault_stack[TSS_IST_STACK_BYTES] __attribute__((aligned(16)));

void tss_init(void) {
    memset(&tss, 0, sizeof(tss_t));
    tss.ist[0] = (uint64_t)(uintptr_t)(g_double_fault_stack + sizeof(g_double_fault_stack));
    tss.iopb_offset = sizeof(tss_t);
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}
