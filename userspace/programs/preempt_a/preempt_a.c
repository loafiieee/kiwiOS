#include <stdint.h>
#include "kiwi_syscall.h"

static void write_line(const char* s) {
    uint64_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    (void)sys_write(1, s, len);
}

static void busy_delay(void) {
    volatile uint64_t sink = 0;

    for (uint64_t outer = 0; outer < 24; outer++) {
        for (uint64_t inner = 0; inner < 3000000ull; inner++) {
            sink += (outer ^ inner);
        }
    }

    if (sink == 0) {
        write_line("");
    }
}

int main(void) {
    write_line("preempt_a: step 1\n");
    busy_delay();
    write_line("preempt_a: step 2\n");
    busy_delay();
    write_line("preempt_a: step 3\n");
    return 0;
}
