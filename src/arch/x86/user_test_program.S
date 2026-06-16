.intel_syntax noprefix

.section .rodata

.global user_test_program_start
.global user_test_program_end

user_test_program_start:
    mov eax, 1
    mov edi, 1
    lea rsi, [rip + user_test_msg]
    mov edx, user_test_msg_end - user_test_msg
    syscall

    xor edi, edi
    mov eax, 0
    syscall

    ud2

user_test_msg:
    .ascii "Hello from userspace!\n"
user_test_msg_end:

user_test_program_end:
