#include <stdint.h>
#include "arch/x86/io.h"

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u

#define EFER_SCE   (1ull << 0)

#define RFLAGS_TF  (1ull << 8)
#define RFLAGS_IF  (1ull << 9)
#define RFLAGS_DF  (1ull << 10)

#define KERNEL_CS_SELECTOR 0x08u
#define SYSRET_BASE        0x10u

extern void syscall_entry(void);

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    /*
     * STAR[47:32] gives the kernel CS selector loaded by SYSCALL.
     * STAR[63:48] is the SYSRET base. SYSRET derives:
     *   user SS = base + 8  -> 0x18
     *   user CS = base + 16 -> 0x20
     */
    uint64_t star = 0;
    star |= ((uint64_t)KERNEL_CS_SELECTOR << 32);
    star |= ((uint64_t)SYSRET_BASE << 48);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_TF | RFLAGS_IF | RFLAGS_DF);
}
