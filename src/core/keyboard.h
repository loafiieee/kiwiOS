#ifndef CORE_KEYBOARD_H
#define CORE_KEYBOARD_H

#include "core/process.h"

int keyboard_getchar(void);
int keyboard_getchar_nonblocking(void);
void keyboard_enqueue_waiter(process_t* proc);
void keyboard_interrupt_handler(void);
void wait_for_key(void);

enum {
    KEY_ARROW_UP   = -16,
    KEY_ARROW_DOWN = -17,
    KEY_ARROW_LEFT = -18,
    KEY_ARROW_RIGHT = -19,
    KEY_PAGE_UP = -20,
    KEY_PAGE_DOWN = -21,
};

#endif // CORE_KEYBOARD_H
