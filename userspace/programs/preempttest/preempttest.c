#include <stdint.h>
#include <stdio.h>
#include "kiwi_syscall.h"

int main(void) {
    int status = 0;
    int64_t a = 0;
    int64_t b = 0;
    char path_a[256];
    char path_b[256];

    puts("preempttest: spawning two CPU-bound workers");

    if (kiwi_resolve_program_path("preempt_a", path_a, sizeof(path_a)) != 0) {
        puts("preempttest: FAIL resolve preempt_a");
        return 1;
    }

    if (kiwi_resolve_program_path("preempt_b", path_b, sizeof(path_b)) != 0) {
        puts("preempttest: FAIL resolve preempt_b");
        return 2;
    }

    a = sys_spawn(path_a);
    if (a < 0) {
        puts("preempttest: FAIL spawn preempt_a");
        return 3;
    }

    b = sys_spawn(path_b);
    if (b < 0) {
        puts("preempttest: FAIL spawn preempt_b");
        return 4;
    }

    if (sys_waitpid((int)a, &status) < 0) {
        puts("preempttest: FAIL waitpid preempt_a");
        return 5;
    }

    if (sys_waitpid((int)b, &status) < 0) {
        puts("preempttest: FAIL waitpid preempt_b");
        return 6;
    }

    puts("preempttest: done");
    return 0;
}
