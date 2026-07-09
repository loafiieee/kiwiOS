#ifndef CORE_TIME_H
#define CORE_TIME_H

#include <stdint.h>

typedef uint64_t (*time_highres_reader_t)(void);

#define KIWI_TIMER_DEFAULT_HZ 100u

void time_init(uint32_t tick_hz);
void time_set_realtime_unix(uint64_t unix_seconds);
void time_set_highres_reader(time_highres_reader_t reader);
void time_timer_tick(void);
uint64_t time_ticks(void);
uint32_t time_tick_hz(void);
uint64_t time_monotonic_ns(void);
uint64_t time_realtime_ns(void);

#endif // CORE_TIME_H
