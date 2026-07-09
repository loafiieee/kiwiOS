#include "kiwi_syscall.h"
#include <stdio.h>

int main(void) {
    if (sys_poweroff() < 0) {
        puts("shutdown: ACPI S5 unavailable or failed");
        return 1;
    }

    puts("shutdown: ACPI S5 returned; hardware did not power off");
    return 1;
}
