.intel_syntax noprefix
.global _start
.extern main

.section .text
_start:
    xor rbp, rbp
    xor rdi, rdi
    xor rsi, rsi
    call main
    mov edi, eax
    mov eax, 0
    syscall
    ud2
