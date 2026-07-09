#include "arch/x86/rtc.h"

#include <stdint.h>
#include "arch/x86/io.h"

#define CMOS_INDEX 0x70u
#define CMOS_DATA  0x71u
#define CMOS_NMI_DISABLE 0x80u

#define RTC_REG_SECONDS 0x00u
#define RTC_REG_MINUTES 0x02u
#define RTC_REG_HOURS   0x04u
#define RTC_REG_DAY     0x07u
#define RTC_REG_MONTH   0x08u
#define RTC_REG_YEAR    0x09u
#define RTC_REG_STATUS_A 0x0Au
#define RTC_REG_STATUS_B 0x0Bu
#define RTC_REG_CENTURY_DEFAULT 0x32u

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, CMOS_NMI_DISABLE | reg);
    return inb(CMOS_DATA);
}

static bool rtc_update_in_progress(void) {
    return (cmos_read(RTC_REG_STATUS_A) & 0x80u) != 0;
}

static bool rtc_wait_stable(void) {
    for (uint32_t i = 0; i < 100000u; i++) {
        if (!rtc_update_in_progress()) {
            return true;
        }
    }
    return false;
}

static uint8_t bcd_to_bin(uint8_t value) {
    return (uint8_t)((value & 0x0Fu) + ((value >> 4) * 10u));
}

static bool is_leap_year(uint32_t year) {
    if ((year % 4u) != 0u) return false;
    if ((year % 100u) != 0u) return true;
    return (year % 400u) == 0u;
}

static uint8_t days_in_month(uint32_t year, uint8_t month) {
    static const uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 2u && is_leap_year(year)) {
        return 29u;
    }
    return days[month - 1u];
}

static bool rtc_values_valid(uint32_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
    if (year < 1970u || month < 1u || month > 12u || day < 1u || hour > 23u || minute > 59u || second > 60u) {
        return false;
    }
    return day <= days_in_month(year, month);
}

static uint64_t unix_seconds_from_ymdhms(uint32_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
    uint64_t days = 0;

    for (uint32_t y = 1970u; y < year; y++) {
        days += is_leap_year(y) ? 366u : 365u;
    }
    for (uint8_t m = 1u; m < month; m++) {
        days += days_in_month(year, m);
    }

    days += (uint64_t)(day - 1u);
    return (((days * 24u + hour) * 60u + minute) * 60u) + second;
}

bool rtc_read_unix_time_with_century(uint8_t century_register, uint64_t* out_seconds) {
    uint8_t second = 0;
    uint8_t minute = 0;
    uint8_t hour = 0;
    uint8_t day = 0;
    uint8_t month = 0;
    uint8_t year = 0;
    uint8_t century = 0;
    uint8_t status_b = 0;
    uint8_t hour_raw = 0;
    uint32_t full_year = 0;
    bool pm = false;

    if (!out_seconds || !rtc_wait_stable()) {
        return false;
    }
    if (century_register == 0u) {
        century_register = RTC_REG_CENTURY_DEFAULT;
    }

    for (uint32_t attempt = 0; attempt < 4u; attempt++) {
        uint8_t second2 = 0;
        uint8_t minute2 = 0;
        uint8_t hour2 = 0;
        uint8_t day2 = 0;
        uint8_t month2 = 0;
        uint8_t year2 = 0;
        uint8_t century2 = 0;

        second = cmos_read(RTC_REG_SECONDS);
        minute = cmos_read(RTC_REG_MINUTES);
        hour = cmos_read(RTC_REG_HOURS);
        day = cmos_read(RTC_REG_DAY);
        month = cmos_read(RTC_REG_MONTH);
        year = cmos_read(RTC_REG_YEAR);
        century = cmos_read(century_register);
        status_b = cmos_read(RTC_REG_STATUS_B);

        if (!rtc_wait_stable()) {
            return false;
        }

        second2 = cmos_read(RTC_REG_SECONDS);
        minute2 = cmos_read(RTC_REG_MINUTES);
        hour2 = cmos_read(RTC_REG_HOURS);
        day2 = cmos_read(RTC_REG_DAY);
        month2 = cmos_read(RTC_REG_MONTH);
        year2 = cmos_read(RTC_REG_YEAR);
        century2 = cmos_read(century_register);

        if (second == second2 && minute == minute2 && hour == hour2 && day == day2 && month == month2 && year == year2 && century == century2) {
            break;
        }
        if (attempt == 3u) {
            return false;
        }
    }

    hour_raw = hour;
    pm = ((status_b & 0x02u) == 0u) && ((hour_raw & 0x80u) != 0u);
    hour &= 0x7Fu;

    if ((status_b & 0x04u) == 0u) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour = bcd_to_bin(hour);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    if ((status_b & 0x02u) == 0u) {
        if (pm && hour < 12u) {
            hour += 12u;
        } else if (!pm && hour == 12u) {
            hour = 0u;
        }
    }

    if (century >= 19u && century <= 99u) {
        full_year = ((uint32_t)century * 100u) + year;
    } else {
        full_year = (year < 70u) ? (2000u + year) : (1900u + year);
    }

    if (!rtc_values_valid(full_year, month, day, hour, minute, second)) {
        return false;
    }

    *out_seconds = unix_seconds_from_ymdhms(full_year, month, day, hour, minute, second);
    return true;
}
bool rtc_read_unix_time(uint64_t* out_seconds) {
    return rtc_read_unix_time_with_century(RTC_REG_CENTURY_DEFAULT, out_seconds);
}