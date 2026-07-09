#include "kiwi_syscall.h"
#include <stdio.h>

int main(void) {
    if (sys_reboot() < 0) {
        puts("reboot: ACPI reset unavailable and fallback failed");
        return 1;
    }

    puts("reboot: returned unexpectedly");
    return 1;
}
