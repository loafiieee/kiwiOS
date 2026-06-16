#ifndef ARCH_X86_SYSCALL_H
#define ARCH_X86_SYSCALL_H

#include <stdint.h>

extern uint64_t kernel_rsp_current;
extern uint64_t user_rsp_tmp;

void syscall_init(void);

#endif // ARCH_X86_SYSCALL_H
