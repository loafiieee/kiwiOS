#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/boot.h"
#include "core/console.h"
#include "font8x16_tandy2k.h"
#include "libc/string.h"

// ================= Framebuffer helpers =================
struct limine_framebuffer *console_primary_framebuffer(void) {
    struct limine_framebuffer_response *resp = boot_framebuffer_response();
    if (!resp || resp->framebuffer_count < 1) return NULL;
    return resp->framebuffers[0];
}

// ================= Multi-output (HDMI/DP/etc.) framebuffer support =================
// We mirror (duplicate) text to all framebuffers Limine exposes.

#define MAX_OUTPUTS 8
#define GLYPH_W 8
#define GLYPH_H 16

static struct limine_framebuffer *g_fbs[MAX_OUTPUTS];
static uint32_t g_fb_count = 0;

// Text layout bounds shared by all outputs (min width/height across displays)
static uint32_t g_text_w_px = 0;  // usable width in pixels (min across outputs)
static uint32_t g_text_h_px = 0;  // usable height in pixels (min across outputs)

// Forward declarations for helpers used during display initialization.
static void update_layout_from_bounds(void);
static void reset_scrollback(void);
static void clear_outputs(void);
static void render_visible(void);
static void redraw_input_cursor_cell(bool inverted);
static void console_cursor_set_enabled(bool enabled);
static void console_cursor_sync_to_text_cursor(void);

static bool framebuffer_usable(const struct limine_framebuffer *fb) {
    return fb &&
           fb->address != 0 &&
           fb->bpp == 32 &&
           fb->width > 0 &&
           fb->height > 0 &&
           fb->pitch >= fb->width * 4u;
}

// Call this once early in kmain(), after Limine is ready.
static void display_init(void) {
    struct limine_framebuffer_response *resp = boot_framebuffer_response();
    if (!resp || resp->framebuffer_count == 0) {
        // Nothing to draw on; halt.
        boot_hcf();
    }

    g_fb_count = (resp->framebuffer_count > MAX_OUTPUTS)
               ? MAX_OUTPUTS : (uint32_t)resp->framebuffer_count;

    // Collect outputs and compute shared usable region (min width/height)
    g_text_w_px = 0xFFFFFFFFu;
    g_text_h_px = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < g_fb_count; i++) {
        g_fbs[i] = resp->framebuffers[i];

        // We assume 32-bpp linear RGB (Limine default GOP/VBE).
        if (!framebuffer_usable(g_fbs[i])) {
            // Disable this output
            for (uint32_t j = i + 1; j < g_fb_count; j++) g_fbs[j-1] = g_fbs[j];
            g_fb_count--;
            i--;
            continue;
        }

        if (g_fbs[i]->width  < g_text_w_px) g_text_w_px = (uint32_t)g_fbs[i]->width;
        if (g_fbs[i]->height < g_text_h_px) g_text_h_px = (uint32_t)g_fbs[i]->height;
    }

    if (g_fb_count == 0) {
        boot_hcf();
    }

    // Round down to glyph grid so wrapping/scrolling is identical on all displays.
    g_text_w_px = (g_text_w_px / GLYPH_W) * GLYPH_W;
    g_text_h_px = (g_text_h_px / GLYPH_H) * GLYPH_H;

    update_layout_from_bounds();
    reset_scrollback();
    clear_outputs();
    render_visible();
}

void console_init(void) {
    display_init();
}

// ================= Text renderer (mirrored to all outputs) =================
// Fast scrolling backed by a scrollback buffer and an 8x16 Tandy 2000 font.

static const uint32_t DEFAULT_FG = 0x00C0C0C0; // light gray
static const uint32_t DEFAULT_BG = 0x00000000; // black

static uint32_t fg_color = 0x00C0C0C0;
static uint32_t bg_color = 0x00000000;

void console_set_colors(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void console_get_colors(uint32_t *fg, uint32_t *bg) {
    if (fg) *fg = fg_color;
    if (bg) *bg = bg_color;
}

// Integer text scale (1=normal, 2=double, ...)
static uint32_t g_scale = 1;
static inline uint32_t CELL_W(void){ return GLYPH_W * g_scale; }
static inline uint32_t CELL_H(void){ return GLYPH_H * g_scale; }

static inline void fill_row_span(uint8_t *row_base, uint32_t pixels, uint32_t color) {
    uint32_t *p = (uint32_t *)row_base;
    for (uint32_t x = 0; x < pixels; x++) p[x] = color;
}

// Basic ANSI color palette (0-7 normal, 8-15 bright)
static const uint32_t ansi_palette[16] = {
    0x00000000, // 0 black
    0x00AA0000, // 1 red
    0x0000AA00, // 2 green
    0x00AA5500, // 3 yellow/brown
    0x000000AA, // 4 blue
    0x00AA00AA, // 5 magenta
    0x0000AAAA, // 6 cyan
    0x00AAAAAA, // 7 light gray
    0x00555555, // 8 dark gray
    0x00FF5555, // 9 bright red
    0x0055FF55, // 10 bright green
    0x00FFFF55, // 11 bright yellow
    0x005555FF, // 12 bright blue
    0x00FF55FF, // 13 bright magenta
    0x0055FFFF, // 14 bright cyan
    0x00FFFFFF  // 15 white
};

static void ansi_reset_state(void);

// Scrollback buffer ------------------------------------------------------

#define MAX_COLS          512
#define SCROLLBACK_LINES 1024

struct cell { char ch; uint32_t fg; uint32_t bg; };
static struct cell g_buffer[SCROLLBACK_LINES][MAX_COLS];

static uint32_t g_cols = 0;               // columns in the visible area
static uint32_t g_rows = 0;               // rows in the visible area
static uint32_t g_head = 0;               // logical line 0 -> g_buffer[g_head]
static uint32_t g_line_count = 0;         // number of valid lines in buffer
static uint32_t g_view_offset = 0;        // how many lines up from the newest view is
static uint32_t g_cursor_line = 0;        // logical cursor line
static uint32_t g_cursor_col = 0;         // cursor column within the newest line
static uint32_t g_saved_cursor_line = 0;
static uint32_t g_saved_cursor_col = 0;
static bool g_input_cursor_enabled = false;
static bool g_input_cursor_visible = false;
static uint32_t g_input_cursor_line = 0;
static uint32_t g_input_cursor_col = 0;
static uint32_t g_input_cursor_ticks = 0;

#define CONSOLE_CURSOR_BLINK_TICKS 25u

static inline uint32_t wrap_line(uint32_t logical) {
    return (g_head + logical) % SCROLLBACK_LINES;
}

static void clear_line(uint32_t logical_line) {
    uint32_t idx = wrap_line(logical_line);
    for (uint32_t x = 0; x < g_cols && x < MAX_COLS; x++) {
        g_buffer[idx][x].ch = ' ';
        g_buffer[idx][x].fg = fg_color;
        g_buffer[idx][x].bg = bg_color;
    }
}

static void reset_scrollback(void) {
    ansi_reset_state();

    g_head = 0;
    g_line_count = 1;
    g_view_offset = 0;
    g_cursor_line = 0;
    g_cursor_col = 0;
    g_saved_cursor_line = 0;
    g_saved_cursor_col = 0;
    g_input_cursor_enabled = false;
    g_input_cursor_visible = false;
    g_input_cursor_line = 0;
    g_input_cursor_col = 0;
    g_input_cursor_ticks = 0;
    clear_line(0);
}

static void update_layout_from_bounds(void) {
    if (CELL_W() == 0 || CELL_H() == 0) return;

    g_cols = g_text_w_px / CELL_W();
    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    if (g_cols == 0) g_cols = 1;
    g_text_w_px = g_cols * CELL_W();

    g_rows = g_text_h_px / CELL_H();
    if (g_rows == 0) g_rows = 1;
    g_text_h_px = g_rows * CELL_H();
}

static uint32_t max_view_offset(void) {
    if (g_line_count <= g_rows) return 0;
    return g_line_count - g_rows;
}

static uint32_t view_start_line(void) {
    uint32_t max_off = max_view_offset();
    if (g_view_offset > max_off) g_view_offset = max_off;
    if (g_line_count <= g_rows) return 0;
    return g_line_count - g_rows - g_view_offset;
}

static void ensure_line_exists(uint32_t logical_line) {
    while (logical_line >= g_line_count) {
        if (g_line_count < SCROLLBACK_LINES) {
            clear_line(g_line_count);
            g_line_count++;
            continue;
        }

        g_head = (g_head + 1) % SCROLLBACK_LINES;
        clear_line(g_line_count - 1);
        if (g_cursor_line > 0) {
            g_cursor_line--;
        }
        if (g_saved_cursor_line > 0) {
            g_saved_cursor_line--;
        }
        if (logical_line > 0) {
            logical_line--;
        }
    }
}

static void clear_outputs(void) {
    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        if (!framebuffer_usable(out)) continue;
        uint8_t *base = (uint8_t *)(uintptr_t)out->address;
        size_t pitch = (size_t)out->pitch;
        for (uint32_t y = 0; y < out->height; y++) {
            uint8_t *row = base + (size_t)y * pitch;
            fill_row_span(row, (uint32_t)out->width, bg_color);
        }
    }
}

// Font blitting ----------------------------------------------------------
static void draw_char_scaled(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font8x16_tandy2k[(uint8_t)c];

    for (uint32_t i = 0; i < g_fb_count; i++) {
        struct limine_framebuffer *out = g_fbs[i];
        if (!framebuffer_usable(out)) continue;
        if (x + CELL_W() > out->width || y + CELL_H() > out->height) continue;

        uint8_t  *base   = (uint8_t *)(uintptr_t)out->address;
        size_t    pitch  = (size_t)out->pitch;

        // Fill glyph background box
        for (uint32_t ry = 0; ry < CELL_H(); ry++) {
            uint8_t *row = base + (size_t)(y + ry) * pitch + (size_t)x * 4;
            fill_row_span(row, CELL_W(), bg);
        }

        // Plot foreground pixels scaled up
        for (int src_row = 0; src_row < GLYPH_H; src_row++) {
            uint8_t bits = glyph[src_row];
            for (int src_col = 0; src_col < GLYPH_W; src_col++) {
                if (bits & 1) {
                    for (uint32_t dy = 0; dy < g_scale; dy++) {
                        uint8_t *row = base
                                     + (size_t)(y + (uint32_t)src_row * g_scale + dy) * pitch
                                     + (size_t)(x + (uint32_t)src_col * g_scale) * 4;
                        uint32_t *p = (uint32_t *)row;
                        for (uint32_t dx = 0; dx < g_scale; dx++) p[dx] = fg;
                    }
                }
                bits >>= 1;
            }
        }
    }
}

static void draw_cell(uint32_t view_row, uint32_t col, const struct cell *c) {
    draw_char_scaled(col * CELL_W(), view_row * CELL_H(), c->ch, c->fg, c->bg);
}

static void draw_blank_cell(uint32_t view_row, uint32_t col) {
    struct cell blank = { ' ', fg_color, bg_color };
    draw_cell(view_row, col, &blank);
}

static void render_line_to_row(uint32_t logical_line, uint32_t view_row) {
    uint32_t idx = wrap_line(logical_line);
    const struct cell *line = g_buffer[idx];
    for (uint32_t col = 0; col < g_cols; col++) {
        draw_cell(view_row, col, &line[col]);
    }
}

static void render_visible(void) {
    uint32_t start = view_start_line();
    for (uint32_t row = 0; row < g_rows; row++) {
        uint32_t logical = start + row;
        if (logical < g_line_count) {
            render_line_to_row(logical, row);
        } else {
            for (uint32_t col = 0; col < g_cols; col++) draw_blank_cell(row, col);
        }
    }
}

static void new_line(void) {
    if (g_cursor_line + 1u < g_line_count) {
        g_cursor_line++;
        g_cursor_col = 0;
        if (g_view_offset == 0) {
            render_visible();
        }
        return;
    }

    ensure_line_exists(g_line_count);
    g_cursor_line = g_line_count - 1u;
    g_cursor_col = 0;

    if (g_view_offset == 0) {
        /* Repaint the visible area from the RAM cell buffer instead of doing a
         * VRAM->VRAM scroll. Reading back video memory is extremely slow on real
         * hardware and VMware (the framebuffer is uncached/write-combining), so
         * the old memmove-based scroll stalled for seconds per line. render_visible()
         * only writes to VRAM, matching the fast Page Up/Down repaint path. */
        render_visible();
    } else {
        (void)view_start_line();
    }
}

void console_page_up(void) {
    uint32_t max_off = max_view_offset();
    if (max_off == 0) return;

    uint32_t step = (g_rows > 1) ? (g_rows - 1) : 1;
    if (g_view_offset + step > max_off) step = max_off - g_view_offset;
    g_view_offset += step;
    render_visible();
}

void console_page_down(void) {
    if (g_view_offset == 0) return;

    uint32_t step = (g_rows > 1) ? (g_rows - 1) : 1;
    if (step > g_view_offset) step = g_view_offset;
    g_view_offset -= step;
    render_visible();
}

// Public: allow shell to change scale
void console_set_scale(uint32_t new_scale) {
    if (new_scale == 0) new_scale = 1;
    if (new_scale > 16) new_scale = 9;
    if (new_scale == g_scale) return;

    g_scale = new_scale;

    update_layout_from_bounds();
    clear_outputs();
    reset_scrollback();
    render_visible();
}

void console_reset_scrollback(void) { reset_scrollback(); }

void console_clear_outputs(void) { clear_outputs(); }

void console_render_visible(void) { render_visible(); }

void console_clear(void) {
    reset_scrollback();
    clear_outputs();
    render_visible();
}

void console_get_size(uint32_t *rows, uint32_t *cols) {
    if (rows) {
        *rows = g_rows;
    }
    if (cols) {
        *cols = g_cols;
    }
}

void console_set_input_line(const char *prefix,
                            const char *text,
                            uint32_t text_len,
                            uint32_t cursor_pos,
                            bool show_cursor) {
    uint32_t logical_line = 0;
    uint32_t idx = 0;
    uint32_t start = 0;
    uint32_t view_row = 0;
    uint32_t col = 0;
    uint32_t cursor_col = 0;

    if (g_line_count == 0) {
        reset_scrollback();
    }

    if (cursor_pos > text_len) {
        cursor_pos = text_len;
    }

    if (g_input_cursor_visible) {
        redraw_input_cursor_cell(false);
        g_input_cursor_visible = false;
    }

    logical_line = g_line_count - 1;
    idx = wrap_line(logical_line);

    for (col = 0; col < g_cols; col++) {
        g_buffer[idx][col].ch = ' ';
        g_buffer[idx][col].fg = fg_color;
        g_buffer[idx][col].bg = bg_color;
    }

    col = 0;
    if (prefix) {
        while (*prefix && col < g_cols) {
            g_buffer[idx][col].ch = *prefix++;
            g_buffer[idx][col].fg = fg_color;
            g_buffer[idx][col].bg = bg_color;
            col++;
        }
    }

    cursor_col = col + cursor_pos;
    for (uint32_t i = 0; i < text_len && col < g_cols; i++, col++) {
        g_buffer[idx][col].ch = text[i];
        g_buffer[idx][col].fg = fg_color;
        g_buffer[idx][col].bg = bg_color;
    }

    g_cursor_line = logical_line;
    g_cursor_col = col;
    g_input_cursor_enabled = show_cursor && cursor_col < g_cols;
    g_input_cursor_line = logical_line;
    g_input_cursor_col = cursor_col;
    g_input_cursor_ticks = 0;

    start = view_start_line();
    if (logical_line < start || logical_line >= start + g_rows) {
        return;
    }

    view_row = logical_line - start;
    render_line_to_row(logical_line, view_row);

    if (g_input_cursor_enabled) {
        redraw_input_cursor_cell(true);
        g_input_cursor_visible = true;
    }
}

void console_timer_tick(void) {
    if (!g_input_cursor_enabled) {
        return;
    }

    g_input_cursor_ticks++;
    if (g_input_cursor_ticks < CONSOLE_CURSOR_BLINK_TICKS) {
        return;
    }

    g_input_cursor_ticks = 0;
    g_input_cursor_visible = !g_input_cursor_visible;
    redraw_input_cursor_cell(g_input_cursor_visible);
}

static void console_cursor_set_enabled(bool enabled) {
    if (g_input_cursor_visible) {
        redraw_input_cursor_cell(false);
        g_input_cursor_visible = false;
    }

    g_input_cursor_enabled = enabled;
    g_input_cursor_line = g_cursor_line;
    g_input_cursor_col = g_cursor_col;
    g_input_cursor_ticks = 0;

    if (enabled) {
        redraw_input_cursor_cell(true);
        g_input_cursor_visible = true;
    }
}

static void console_cursor_sync_to_text_cursor(void) {
    if (!g_input_cursor_enabled) {
        return;
    }

    if (g_input_cursor_visible) {
        redraw_input_cursor_cell(false);
        g_input_cursor_visible = false;
    }

    g_input_cursor_line = g_cursor_line;
    g_input_cursor_col = g_cursor_col;
    g_input_cursor_ticks = 0;
    redraw_input_cursor_cell(true);
    g_input_cursor_visible = true;
}

static void redraw_input_cursor_cell(bool inverted) {
    uint32_t start = 0;
    uint32_t view_row = 0;
    uint32_t idx = 0;
    struct cell cell = { ' ', fg_color, bg_color };

    if (!g_input_cursor_enabled || g_input_cursor_col >= g_cols) {
        return;
    }

    start = view_start_line();
    if (g_input_cursor_line < start || g_input_cursor_line >= start + g_rows) {
        return;
    }

    idx = wrap_line(g_input_cursor_line);
    cell = g_buffer[idx][g_input_cursor_col];
    if (inverted) {
        uint32_t tmp = cell.fg;
        cell.fg = cell.bg;
        cell.bg = tmp;
    }

    view_row = g_input_cursor_line - start;
    draw_cell(view_row, g_input_cursor_col, &cell);
}

// --- Required exports (same names as your existing code) ---

void scroll_up(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    new_line();
}

void draw_char(struct limine_framebuffer *fb /*unused*/,
               uint32_t x, uint32_t y,
               char c, uint32_t fg, uint32_t bg) {
    (void)fb;
    draw_char_scaled(x, y, c, fg, bg);
}

// ANSI escape parsing state for simple color control
static enum { ANSI_NORMAL, ANSI_ESC, ANSI_CSI } ansi_state = ANSI_NORMAL;
static uint32_t ansi_params[8];
static uint32_t ansi_param_count = 0;
static bool ansi_private = false;

static void ansi_reset_state(void) {
    ansi_state = ANSI_NORMAL;
    ansi_param_count = 0;
    ansi_private = false;
    for (uint32_t i = 0; i < 8; i++) ansi_params[i] = 0;
}

static void apply_sgr_params(void) {
    if (ansi_param_count == 0) {
        fg_color = DEFAULT_FG;
        bg_color = DEFAULT_BG;
        return;
    }

    for (uint32_t i = 0; i < ansi_param_count; i++) {
        uint32_t p = ansi_params[i];
        if (p == 0) {
            fg_color = DEFAULT_FG;
            bg_color = DEFAULT_BG;
        } else if (p == 1) {
            fg_color = ansi_palette[15];
        } else if (p == 7 || p == 27) {
            uint32_t tmp = fg_color;
            fg_color = bg_color;
            bg_color = tmp;
        } else if (p == 39) {
            fg_color = DEFAULT_FG;
        } else if (p == 49) {
            bg_color = DEFAULT_BG;
        } else if (p >= 30 && p <= 37) {
            fg_color = ansi_palette[p - 30];
        } else if (p >= 90 && p <= 97) {
            fg_color = ansi_palette[(p - 90) + 8];
        } else if (p >= 40 && p <= 47) {
            bg_color = ansi_palette[p - 40];
        } else if (p >= 100 && p <= 107) {
            bg_color = ansi_palette[(p - 100) + 8];
        }
    }
}

static uint32_t ansi_param_or(uint32_t index, uint32_t fallback) {
    if (index >= ansi_param_count || ansi_params[index] == 0) {
        return fallback;
    }
    return ansi_params[index];
}

static void render_line_if_visible(uint32_t logical_line) {
    uint32_t start = view_start_line();
    if (logical_line >= start && logical_line < start + g_rows) {
        render_line_to_row(logical_line, logical_line - start);
    }
}

static void blank_cell(uint32_t logical_line, uint32_t col) {
    uint32_t idx = wrap_line(logical_line);
    if (col >= g_cols) {
        return;
    }

    g_buffer[idx][col].ch = ' ';
    g_buffer[idx][col].fg = fg_color;
    g_buffer[idx][col].bg = bg_color;
}

static void ansi_set_cursor(uint32_t row, uint32_t col) {
    uint32_t start = view_start_line();
    uint32_t target_line = start;

    if (row == 0) {
        row = 1;
    }
    if (col == 0) {
        col = 1;
    }
    if (row > g_rows) {
        row = g_rows;
    }
    if (col > g_cols) {
        col = g_cols;
    }

    target_line = start + row - 1u;
    ensure_line_exists(target_line);
    g_cursor_line = target_line;
    g_cursor_col = col - 1u;
    console_cursor_sync_to_text_cursor();
}

static void ansi_move_cursor(char final) {
    uint32_t amount = ansi_param_or(0, 1);
    uint32_t start = view_start_line();
    uint32_t bottom = start + ((g_rows > 0) ? (g_rows - 1u) : 0);

    switch (final) {
        case 'A':
            if (amount > g_cursor_line) {
                g_cursor_line = 0;
            } else {
                g_cursor_line -= amount;
            }
            if (g_cursor_line < start) {
                g_cursor_line = start;
            }
            break;
        case 'B':
            g_cursor_line += amount;
            if (g_cursor_line > bottom) {
                g_cursor_line = bottom;
            }
            ensure_line_exists(g_cursor_line);
            break;
        case 'C':
            g_cursor_col += amount;
            if (g_cursor_col >= g_cols) {
                g_cursor_col = g_cols - 1u;
            }
            break;
        case 'D':
            if (amount > g_cursor_col) {
                g_cursor_col = 0;
            } else {
                g_cursor_col -= amount;
            }
            break;
    }

    console_cursor_sync_to_text_cursor();
}

static void ansi_erase_line(uint32_t mode) {
    ensure_line_exists(g_cursor_line);

    if (mode == 2) {
        for (uint32_t col = 0; col < g_cols; col++) {
            blank_cell(g_cursor_line, col);
        }
    } else if (mode == 1) {
        for (uint32_t col = 0; col <= g_cursor_col && col < g_cols; col++) {
            blank_cell(g_cursor_line, col);
        }
    } else {
        for (uint32_t col = g_cursor_col; col < g_cols; col++) {
            blank_cell(g_cursor_line, col);
        }
    }

    render_line_if_visible(g_cursor_line);
}

static void ansi_erase_display(uint32_t mode) {
    uint32_t start = view_start_line();
    uint32_t end = start + g_rows;

    if (mode == 2 || mode == 3) {
        reset_scrollback();
        clear_outputs();
        render_visible();
        return;
    }

    for (uint32_t line = start; line < end; line++) {
        ensure_line_exists(line);
        for (uint32_t col = 0; col < g_cols; col++) {
            if (mode == 1) {
                if (line > g_cursor_line || (line == g_cursor_line && col > g_cursor_col)) {
                    continue;
                }
            } else {
                if (line < g_cursor_line || (line == g_cursor_line && col < g_cursor_col)) {
                    continue;
                }
            }
            blank_cell(line, col);
        }
    }

    render_visible();
}

static void ansi_handle_csi(char final) {
    if (ansi_private) {
        if (final == 'h' || final == 'l') {
            for (uint32_t i = 0; i < ansi_param_count; i++) {
                if (ansi_params[i] == 25) {
                    console_cursor_set_enabled(final == 'h');
                }
            }
        }
        // Other private modes, including alternate-screen toggles, are no-ops for now.
        return;
    }

    switch (final) {
        case 'm':
            apply_sgr_params();
            break;
        case 'H':
        case 'f':
            ansi_set_cursor(ansi_param_or(0, 1), ansi_param_or(1, 1));
            break;
        case 'A':
        case 'B':
        case 'C':
        case 'D':
            ansi_move_cursor(final);
            break;
        case 'G':
            ansi_set_cursor((g_cursor_line - view_start_line()) + 1u, ansi_param_or(0, 1));
            break;
        case 'J':
            ansi_erase_display(ansi_param_or(0, 0));
            break;
        case 'K':
            ansi_erase_line(ansi_param_or(0, 0));
            break;
        case 's':
            g_saved_cursor_line = g_cursor_line;
            g_saved_cursor_col = g_cursor_col;
            break;
        case 'u':
            ensure_line_exists(g_saved_cursor_line);
            g_cursor_line = g_saved_cursor_line;
            g_cursor_col = (g_saved_cursor_col < g_cols) ? g_saved_cursor_col : (g_cols - 1u);
            break;
    }
}

// Draw char at cursor (advances cursor) — mirrored to all outputs
void putc_fb(struct limine_framebuffer *fb /*unused*/, char c) {
    (void)fb;

    if (ansi_state == ANSI_ESC) {
        if (c == '[') {
            ansi_state = ANSI_CSI;
            ansi_param_count = 0;
            ansi_params[0] = 0;
        } else if (c == '7') {
            g_saved_cursor_line = g_cursor_line;
            g_saved_cursor_col = g_cursor_col;
            ansi_reset_state();
        } else if (c == '8') {
            ensure_line_exists(g_saved_cursor_line);
            g_cursor_line = g_saved_cursor_line;
            g_cursor_col = (g_saved_cursor_col < g_cols) ? g_saved_cursor_col : (g_cols - 1u);
            ansi_reset_state();
        } else if (c == 'c') {
            console_clear();
            ansi_reset_state();
        } else {
            ansi_reset_state();
        }
        return;
    } else if (ansi_state == ANSI_CSI) {
        if (c >= '0' && c <= '9') {
            ansi_params[ansi_param_count] = ansi_params[ansi_param_count] * 10 + (uint32_t)(c - '0');
        } else if (c == '?') {
            ansi_private = true;
        } else if (c == ';') {
            if (ansi_param_count + 1 < 8) {
                ansi_param_count++;
                ansi_params[ansi_param_count] = 0;
            }
        } else {
            ansi_param_count++;
            ansi_handle_csi(c);
            ansi_reset_state();
        }
        return;
    }

    if (c == '\x1B') { ansi_state = ANSI_ESC; return; }
    if (c == '\n') { new_line(); return; }
    if (c == '\r') { g_cursor_col = 0; return; }
    if (c == '\t') {
        uint32_t next_tab = (g_cursor_col + 8u) & ~7u;
        while (g_cursor_col < next_tab) {
            putc_fb(NULL, ' ');
        }
        return;
    }

    if (c == '\b') {
        if (g_cursor_col > 0) {
            g_cursor_col--;
            uint32_t logical_line = g_cursor_line;
            g_buffer[wrap_line(logical_line)][g_cursor_col].ch = ' ';
            g_buffer[wrap_line(logical_line)][g_cursor_col].fg = fg_color;
            g_buffer[wrap_line(logical_line)][g_cursor_col].bg = bg_color;

            uint32_t start = view_start_line();
            if (g_view_offset <= max_view_offset() && logical_line >= start && logical_line < start + g_rows) {
                render_line_to_row(logical_line, logical_line - start);
            }
        }
        return;
    }

    if ((uint8_t)c < 0x20u) {
        return;
    }

    if (g_cursor_col >= g_cols) new_line();
    ensure_line_exists(g_cursor_line);

    uint32_t logical_line = g_cursor_line;
    uint32_t idx = wrap_line(logical_line);
    g_buffer[idx][g_cursor_col].ch = c;
    g_buffer[idx][g_cursor_col].fg = fg_color;
    g_buffer[idx][g_cursor_col].bg = bg_color;

    uint32_t start = view_start_line();
    if (logical_line >= start && logical_line < start + g_rows && g_cursor_col < g_cols) {
        draw_cell(logical_line - start, g_cursor_col, &g_buffer[idx][g_cursor_col]);
    }

    g_cursor_col++;
}

// Draw string at cursor
void print(struct limine_framebuffer *fb /*unused*/, const char *s) {
    (void)fb;
    while (*s) putc_fb(NULL, *s++);
}

// Print a 64-bit hex number (unchanged)
void print_hex(struct limine_framebuffer *fb, uint64_t num) {
    print(fb, "0x");
    static const char hex[] = "0123456789ABCDEF";
    char buf[17]; buf[16] = '\0';
    for (int i = 15; i >= 0; i--) { buf[i] = hex[num & 0xF]; num >>= 4; }
    print(fb, buf);
}

void print_u64(struct limine_framebuffer *fb, uint64_t v) {
    char buf[32];
    int i = 0;
    if (v == 0) { putc_fb(fb, '0'); return; }
    while (v > 0) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i--) putc_fb(fb, buf[i]);
}

void print_u32(struct limine_framebuffer *fb, uint32_t v) {
    print_u64(fb, v);
}
