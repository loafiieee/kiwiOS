.intel_syntax noprefix

.section .text

.global process_enter_context_asm
.type process_enter_context_asm, @function

process_enter_context_asm:
    cli
    mov rsp, rsi
    mov cr3, rdi

    push 0x1B
    push qword ptr [rdx + 8]
    push qword ptr [rdx + 16]
    push 0x23
    push qword ptr [rdx + 0]

    mov rax, qword ptr [rdx + 24]
    mov rbx, qword ptr [rdx + 32]
    mov rbp, qword ptr [rdx + 40]
    mov rdi, qword ptr [rdx + 48]
    mov rsi, qword ptr [rdx + 56]
    mov r8,  qword ptr [rdx + 80]
    mov r9,  qword ptr [rdx + 88]
    mov r10, qword ptr [rdx + 96]
    mov r11, qword ptr [rdx + 104]
    mov r12, qword ptr [rdx + 112]
    mov r13, qword ptr [rdx + 120]
    mov r14, qword ptr [rdx + 128]
    mov r15, qword ptr [rdx + 136]
    mov rcx, qword ptr [rdx + 72]
    mov rdx, qword ptr [rdx + 64]
    iretq

.size process_enter_context_asm, . - process_enter_context_asm

.global scheduler_checkpoint_asm
.type scheduler_checkpoint_asm, @function

scheduler_checkpoint_asm:
    lea rax, [rsp + 8]
    mov qword ptr [rdi + 0], rax
    mov rax, qword ptr [rsp]
    mov qword ptr [rdi + 8], rax
    mov qword ptr [rdi + 16], rbx
    mov qword ptr [rdi + 24], rbp
    mov qword ptr [rdi + 32], r12
    mov qword ptr [rdi + 40], r13
    mov qword ptr [rdi + 48], r14
    mov qword ptr [rdi + 56], r15
    mov rax, cr3
    mov qword ptr [rdi + 64], rax
    pushfq
    pop rax
    mov qword ptr [rdi + 72], rax
    xor eax, eax
    ret

.size scheduler_checkpoint_asm, . - scheduler_checkpoint_asm

.global scheduler_restore_kernel_asm
.type scheduler_restore_kernel_asm, @function

scheduler_restore_kernel_asm:
    mov rax, qword ptr [rdi + 64]
    mov cr3, rax
    mov rbx, qword ptr [rdi + 16]
    mov rbp, qword ptr [rdi + 24]
    mov r12, qword ptr [rdi + 32]
    mov r13, qword ptr [rdi + 40]
    mov r14, qword ptr [rdi + 48]
    mov r15, qword ptr [rdi + 56]
    mov rsp, qword ptr [rdi + 0]
    mov rax, qword ptr [rdi + 72]
    push rax
    popfq
    mov eax, 1
    jmp qword ptr [rdi + 8]

.size scheduler_restore_kernel_asm, . - scheduler_restore_kernel_asm
