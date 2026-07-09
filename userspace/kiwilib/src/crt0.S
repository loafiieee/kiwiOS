.intel_syntax noprefix
.global _start
.extern main
.extern environ
.extern setprogname

.section .text
_start:
    xor rbp, rbp
    mov r12, [rsp]
    lea r13, [rsp + 8]
    lea r14, [r13 + r12 * 8 + 8]
    mov [rip + environ], r14
    and rsp, -16
    test r12, r12
    jz .call_main
    mov rdi, [r13]
    call setprogname
.call_main:
    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    call main
    mov edi, eax
    mov eax, 0
    syscall
    ud2
