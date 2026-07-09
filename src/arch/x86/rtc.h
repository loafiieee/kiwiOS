#ifndef ARCH_X86_RTC_H
#define ARCH_X86_RTC_H

#include <stdbool.h>
#include <stdint.h>

bool rtc_read_unix_time(uint64_t* out_seconds);
bool rtc_read_unix_time_with_century(uint8_t century_register, uint64_t* out_seconds);

#endif // ARCH_X86_RTC_H