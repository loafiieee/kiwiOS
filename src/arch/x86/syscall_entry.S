.intel_syntax noprefix

.section .bss
.align 8

.global kernel_rsp_current
kernel_rsp_current:
    .quad 0

.global user_rsp_tmp
user_rsp_tmp:
    .quad 0

.section .text

.global syscall_entry
.extern syscall_dispatch
.type syscall_entry, @function

syscall_entry:
    mov qword ptr [rip + user_rsp_tmp], rsp
    mov rsp, qword ptr [rip + kernel_rsp_current]
    test rsp, rsp
    jz .Lfatal

    /* Push fields in reverse order so RSP points at syscall_num first. */
    push qword ptr [rip + user_rsp_tmp]
    push r11
    push rcx
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax

    mov rdi, rsp
    call syscall_dispatch

    mov rdi, qword ptr [rsp + 8]
    mov rsi, qword ptr [rsp + 16]
    mov rdx, qword ptr [rsp + 24]
    mov r10, qword ptr [rsp + 32]
    mov r8,  qword ptr [rsp + 40]
    mov r9,  qword ptr [rsp + 48]
    mov r15, qword ptr [rsp + 56]
    mov r14, qword ptr [rsp + 64]
    mov r13, qword ptr [rsp + 72]
    mov r12, qword ptr [rsp + 80]
    mov rbx, qword ptr [rsp + 88]
    mov rbp, qword ptr [rsp + 96]
    mov rcx, qword ptr [rsp + 104]
    mov r11, qword ptr [rsp + 112]
    mov rsp, qword ptr [rsp + 120]
    sysretq

.Lfatal:
    cli
.Lhalt:
    hlt
    jmp .Lhalt

.size syscall_entry, . - syscall_entry
