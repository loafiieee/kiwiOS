#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "arch/x86/io.h"
#include "core/console.h"
#include "core/keyboard.h"
#include "core/scheduler.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define KEYBOARD_BUFFER_SIZE 256u

// US QWERTY scancode set 1 -> ASCII mapping
static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, // Left ctrl
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, // Left shift
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 
    0, // Right shift
    '*',
    0, // Left alt
    ' '
};

static const char scancode_to_ascii_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, // Left ctrl
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, // Left shift
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 
    0, // Right shift
    '*',
    0, // Left alt
    ' '
};

static bool shift_pressed = false;
static bool ctrl_pressed  = false;
static bool e0_prefix     = false;
static int g_keybuf[KEYBOARD_BUFFER_SIZE];
static uint32_t g_keybuf_head = 0;
static uint32_t g_keybuf_tail = 0;
static uint32_t g_keybuf_count = 0;
static process_t* g_wait_head = NULL;
static process_t* g_wait_tail = NULL;

// Helpers for scancode press/release
static inline bool is_shift_press(uint8_t s)   { return s == 0x2A || s == 0x36; }
static inline bool is_shift_release(uint8_t s) { return s == 0xAA || s == 0xB6; }
static inline bool is_ctrl_press(uint8_t s)    { return s == 0x1D; }   // Left Ctrl
static inline bool is_ctrl_release(uint8_t s)  { return s == 0x9D; }   // Left Ctrl release

static inline char maybe_ctrlify(char c) {
    if (!ctrl_pressed) return c;
    // Only letters become control chars (A..Z -> 1..26)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return (char)(c & 0x1F);
    }
    return c;
}

static void keybuf_push(int ch) {
    if (g_keybuf_count >= KEYBOARD_BUFFER_SIZE) {
        return;
    }

    g_keybuf[g_keybuf_tail] = ch;
    g_keybuf_tail = (g_keybuf_tail + 1u) % KEYBOARD_BUFFER_SIZE;
    g_keybuf_count++;
}

static int keybuf_pop(void) {
    int ch = -1;
    if (g_keybuf_count == 0) {
        return -1;
    }

    ch = g_keybuf[g_keybuf_head];
    g_keybuf_head = (g_keybuf_head + 1u) % KEYBOARD_BUFFER_SIZE;
    g_keybuf_count--;
    return ch;
}

static process_t* keyboard_wait_pop(void) {
    process_t* proc = g_wait_head;
    if (!proc) {
        return NULL;
    }

    g_wait_head = proc->next;
    if (!g_wait_head) {
        g_wait_tail = NULL;
    }

    proc->next = NULL;
    return proc;
}

static int translate_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        e0_prefix = true;
        return -1;
    }

    if (e0_prefix) {
        e0_prefix = false;
        if (scancode & 0x80) {
            return -1;
        }

        switch (scancode) {
            case 0x49:
                console_page_up();
                return -1;
            case 0x51:
                console_page_down();
                return -1;
            case 0x48:
                return KEY_ARROW_UP;
            case 0x50:
                return KEY_ARROW_DOWN;
            case 0x4B:
                return KEY_ARROW_LEFT;
            case 0x4D:
                return KEY_ARROW_RIGHT;
            default:
                return -1;
        }
    }

    if (is_shift_press(scancode))   { shift_pressed = true;  return -1; }
    if (is_shift_release(scancode)) { shift_pressed = false; return -1; }
    if (is_ctrl_press(scancode))    { ctrl_pressed  = true;  return -1; }
    if (is_ctrl_release(scancode))  { ctrl_pressed  = false; return -1; }

    if (scancode & 0x80) {
        return -1;
    }

    if (scancode < sizeof(scancode_to_ascii)) {
        char c = shift_pressed ? scancode_to_ascii_shift[scancode]
                               : scancode_to_ascii[scancode];
        if (c != 0) {
            return (int)maybe_ctrlify(c);
        }
    }

    return -1;
}

static bool interrupts_enabled(void) {
    uint64_t rflags = 0;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    return (rflags & (1ull << 9)) != 0;
}

void keyboard_enqueue_waiter(process_t* proc) {
    if (!proc) {
        return;
    }

    proc->next = NULL;
    if (g_wait_tail) {
        g_wait_tail->next = proc;
    } else {
        g_wait_head = proc;
    }
    g_wait_tail = proc;
}

void keyboard_interrupt_handler(void) {
    for (;;) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & 0x01)) {
            break;
        }

        int ch = translate_scancode(inb(PS2_DATA_PORT));
        if (ch == -1) {
            continue;
        }

        keybuf_push(ch);

        process_t* proc = keyboard_wait_pop();
        if (proc) {
            scheduler_add(proc);
        }
    }
}

int keyboard_getchar(void) {
    for (;;) {
        int ch = keybuf_pop();
        if (ch >= 0) {
            return ch;
        }

        if (ch == KEY_ARROW_UP || ch == KEY_ARROW_DOWN ||
            ch == KEY_ARROW_LEFT || ch == KEY_ARROW_RIGHT) {
            return ch;
        }

        if (interrupts_enabled()) {
            asm volatile("hlt");
        }
    }
}

int keyboard_getchar_nonblocking(void) {
    return keybuf_pop();
}

void wait_for_key(void) {
    print(NULL, "[Press any key to continue]");
    keyboard_getchar();
    print(NULL, "\n");
}
