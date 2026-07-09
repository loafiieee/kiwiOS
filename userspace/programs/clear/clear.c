#include <stdint.h>
#include "kiwi_syscall.h"

/* Clear the screen and home the cursor using the ANSI sequences the KiwiOS
 * framebuffer console already understands. */
int main(void) {
    static const char seq[] = "\x1b[H\x1b[2J";
    sys_write(1, seq, (uint64_t)(sizeof(seq) - 1u));
    return 0;
}
