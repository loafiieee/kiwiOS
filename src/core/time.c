#include "core/time.h"

#include <stdint.h>

static volatile uint64_t g_ticks = 0;
static volatile uint64_t g_monotonic_ns = 0;
static volatile uint64_t g_realtime_base_ns = 0;
static volatile uint8_t g_realtime_valid = 0;
static volatile uint64_t g_highres_offset_ns = 0;
static time_highres_reader_t g_highres_reader = 0;
static uint32_t g_tick_hz = KIWI_TIMER_DEFAULT_HZ;
static uint64_t g_ns_per_tick = 1000000000ull / KIWI_TIMER_DEFAULT_HZ;

void time_init(uint32_t tick_hz) {
    if (tick_hz == 0) {
        tick_hz = KIWI_TIMER_DEFAULT_HZ;
    }

    g_ticks = 0;
    g_monotonic_ns = 0;
    g_realtime_base_ns = 0;
    g_realtime_valid = 0;
    g_highres_offset_ns = 0;
    g_highres_reader = 0;
    g_tick_hz = tick_hz;
    g_ns_per_tick = 1000000000ull / tick_hz;
}

void time_set_realtime_unix(uint64_t unix_seconds) {
    uint64_t epoch_ns = unix_seconds * 1000000000ull;

    g_realtime_base_ns = epoch_ns - time_monotonic_ns();
    g_realtime_valid = 1;
}

void time_set_highres_reader(time_highres_reader_t reader) {
    if (!reader) {
        g_highres_reader = 0;
        g_highres_offset_ns = 0;
        return;
    }

    g_highres_offset_ns = g_monotonic_ns - reader();
    g_highres_reader = reader;
}

void time_timer_tick(void) {
    g_ticks++;
    g_monotonic_ns += g_ns_per_tick;
}

uint64_t time_ticks(void) {
    return g_ticks;
}

uint32_t time_tick_hz(void) {
    return g_tick_hz;
}

uint64_t time_monotonic_ns(void) {
    time_highres_reader_t reader = g_highres_reader;

    if (reader) {
        return g_highres_offset_ns + reader();
    }
    return g_monotonic_ns;
}

uint64_t time_realtime_ns(void) {
    if (!g_realtime_valid) {
        return 0;
    }
    return g_realtime_base_ns + time_monotonic_ns();
}
