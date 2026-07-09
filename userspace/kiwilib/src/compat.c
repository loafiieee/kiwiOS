#include <stddef.h>
#include "ctype.h"
#include "dirent.h"
#include "errno.h"
#include "fcntl.h"
#include "fnmatch.h"
#include "glob.h"
#include "grp.h"
#include "kiwi_syscall.h"
#include "langinfo.h"
#include "libgen.h"
#include "libintl.h"
#include "locale.h"
#include "poll.h"
#include "pwd.h"
#include "regex.h"
#include "signal.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "strings.h"
#include "sys/resource.h"
#include "sys/select.h"
#include "sys/stat.h"
#include "sys/time.h"
#include "sys/times.h"
#include "sys/utsname.h"
#include "time.h"
#include "unistd.h"

static sighandler_t g_signal_handlers[NSIG];
static sigset_t g_signal_mask;

static char* g_root_group_members[] = { "root", NULL };
static struct passwd g_root_passwd = {
    "root",
    "x",
    0,
    0,
    "KiwiOS root",
    "/home",
    "/bin/sh",
};
static struct group g_root_group = {
    "root",
    "x",
    0,
    g_root_group_members,
};
static int g_passwd_iter_done;

static struct lconv g_lconv = {
    ".",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    127,
    127,
    127,
    127,
    127,
    127,
    127,
    127,
};

char* setlocale(int category, const char* locale) {
    (void)category;
    (void)locale;
    return "C";
}

struct lconv* localeconv(void) {
    return &g_lconv;
}

char* nl_langinfo(nl_item item) {
    static char* const days[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    };
    static char* const abdays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    };
    static char* const months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    static char* const abmonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    switch (item) {
        case CODESET:
            return "ASCII";
        case D_T_FMT:
            return "%Y-%m-%d %H:%M:%S";
        case D_FMT:
            return "%Y-%m-%d";
        case T_FMT:
            return "%H:%M:%S";
        case RADIXCHAR:
            return ".";
        case THOUSEP:
            return "";
        case AM_STR:
            return "AM";
        case PM_STR:
            return "PM";
        case YESEXPR:
            return "^[yY]";
        case NOEXPR:
            return "^[nN]";
        default:
            if (item >= DAY_1 && item <= DAY_7) {
                return days[item - DAY_1];
            }
            if (item >= ABDAY_1 && item <= ABDAY_7) {
                return abdays[item - ABDAY_1];
            }
            if (item >= MON_1 && item <= MON_12) {
                return months[item - MON_1];
            }
            if (item >= ABMON_1 && item <= ABMON_12) {
                return abmonths[item - ABMON_1];
            }
            return "";
    }
}

char* gettext(const char* msgid) {
    return (char*)(msgid ? msgid : "");
}

char* dgettext(const char* domainname, const char* msgid) {
    (void)domainname;
    return gettext(msgid);
}

char* dcgettext(const char* domainname, const char* msgid, int category) {
    (void)domainname;
    (void)category;
    return gettext(msgid);
}

char* ngettext(const char* msgid1, const char* msgid2, unsigned long n) {
    return gettext(n == 1 ? msgid1 : msgid2);
}

char* textdomain(const char* domainname) {
    (void)domainname;
    return "messages";
}

char* bindtextdomain(const char* domainname, const char* dirname) {
    (void)domainname;
    return (char*)(dirname ? dirname : "");
}

time_t time(time_t* tloc) {
    struct timespec ts;
    time_t value = 0;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return (time_t)-1;
    }

    value = ts.tv_sec;
    if (tloc) {
        *tloc = value;
    }
    return value;
}

int gettimeofday(struct timeval* tv, void* tz) {
    struct timespec ts;

    (void)tz;
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000L;
    return 0;
}

int getitimer(int which, struct itimerval* curr_value) {
    if (!curr_value || (which != ITIMER_REAL && which != ITIMER_VIRTUAL && which != ITIMER_PROF)) {
        errno = EINVAL;
        return -1;
    }
    memset(curr_value, 0, sizeof(*curr_value));
    return 0;
}

int setitimer(int which, const struct itimerval* new_value, struct itimerval* old_value) {
    if (!new_value || (which != ITIMER_REAL && which != ITIMER_VIRTUAL && which != ITIMER_PROF)) {
        errno = EINVAL;
        return -1;
    }
    if (old_value) {
        memset(old_value, 0, sizeof(*old_value));
    }
    return 0;
}

clock_t clock(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (clock_t)-1;
    }

    return (clock_t)(ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / 1000L);
}

clock_t times(struct tms* buf) {
    clock_t now = clock();

    if (buf) {
        memset(buf, 0, sizeof(*buf));
        if (now != (clock_t)-1) {
            buf->tms_utime = now;
        }
    }
    return now;
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

static const char* const g_time_abday[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

static const char* const g_time_day[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static const char* const g_time_abmon[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static const char* const g_time_mon[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

static int64_t floor_div_i64(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;

    if (r != 0 && ((r < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int is_leap_year_i64(int64_t year) {
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

static int month_days(int64_t year, int month) {
    static const int days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };

    if (month == 1 && is_leap_year_i64(year)) {
        return 29;
    }
    return days[month];
}

static int yday_from_ymd(int64_t year, int month, int mday) {
    int yday = mday - 1;

    for (int m = 0; m < month; m++) {
        yday += month_days(year, m);
    }
    return yday;
}

static int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
    unsigned adjusted_month;

    year -= month <= 2u;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);

    adjusted_month = month > 2u ? month - 3u : month + 9u;
    unsigned doy = (153u * adjusted_month + 2u) / 5u + day - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t days, int64_t* year, int* month, int* day) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = mp < 10u ? mp + 3u : mp - 9u;

    y += m <= 2u;
    *year = y;
    *month = (int)m;
    *day = (int)d;
}

static void fill_tm_from_unix(time_t value, struct tm* out) {
    int64_t seconds = value;
    int64_t days = floor_div_i64(seconds, 86400);
    int64_t sod = seconds - days * 86400;
    int64_t year = 1970;
    int month = 1;
    int day = 1;

    civil_from_days(days, &year, &month, &day);
    out->tm_sec = (int)(sod % 60);
    out->tm_min = (int)((sod / 60) % 60);
    out->tm_hour = (int)(sod / 3600);
    out->tm_mday = day;
    out->tm_mon = month - 1;
    out->tm_year = (int)(year - 1900);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) {
        out->tm_wday += 7;
    }
    out->tm_yday = yday_from_ymd(year, out->tm_mon, out->tm_mday);
    out->tm_isdst = 0;
}

time_t mktime(struct tm* timeptr) {
    int64_t year;
    int64_t month;
    int64_t day_seconds;
    int64_t days;
    time_t value;

    if (!timeptr) {
        errno = EINVAL;
        return (time_t)-1;
    }

    year = (int64_t)timeptr->tm_year + 1900;
    month = timeptr->tm_mon;
    year += floor_div_i64(month, 12);
    month %= 12;
    if (month < 0) {
        month += 12;
        year--;
    }

    days = days_from_civil(year, (unsigned)month + 1u, 1u) + (int64_t)timeptr->tm_mday - 1;
    day_seconds = (int64_t)timeptr->tm_hour * 3600 +
                  (int64_t)timeptr->tm_min * 60 +
                  (int64_t)timeptr->tm_sec;
    value = (time_t)(days * 86400 + day_seconds);
    fill_tm_from_unix(value, timeptr);
    return value;
}

static char* append_char_limited(char* out, char* end, char ch) {
    if (out < end) {
        *out = ch;
    }
    return out + 1;
}

static char* append_string_limited(char* out, char* end, const char* value) {
    while (value && *value) {
        out = append_char_limited(out, end, *value++);
    }
    return out;
}

static char* append_dec2_limited(char* out, char* end, int value) {
    if (value < 0) {
        value = 0;
    }
    out = append_char_limited(out, end, (char)('0' + ((value / 10) % 10)));
    out = append_char_limited(out, end, (char)('0' + (value % 10)));
    return out;
}

static char* append_dec2_space_limited(char* out, char* end, int value) {
    if (value < 0) {
        value = 0;
    }
    if (value < 10) {
        out = append_char_limited(out, end, ' ');
    } else {
        out = append_char_limited(out, end, (char)('0' + ((value / 10) % 10)));
    }
    out = append_char_limited(out, end, (char)('0' + (value % 10)));
    return out;
}

static char* append_dec3_limited(char* out, char* end, int value) {
    if (value < 0) {
        value = 0;
    }
    out = append_char_limited(out, end, (char)('0' + ((value / 100) % 10)));
    out = append_char_limited(out, end, (char)('0' + ((value / 10) % 10)));
    out = append_char_limited(out, end, (char)('0' + (value % 10)));
    return out;
}

static char* append_dec4_limited(char* out, char* end, int value) {
    if (value < 0) {
        value = 0;
    }
    out = append_char_limited(out, end, (char)('0' + ((value / 1000) % 10)));
    out = append_char_limited(out, end, (char)('0' + ((value / 100) % 10)));
    out = append_char_limited(out, end, (char)('0' + ((value / 10) % 10)));
    out = append_char_limited(out, end, (char)('0' + (value % 10)));
    return out;
}

struct tm* gmtime(const time_t* timer) {
    static struct tm tm;

    if (!timer) {
        errno = EINVAL;
        return NULL;
    }
    memset(&tm, 0, sizeof(tm));
    fill_tm_from_unix(*timer, &tm);
    return &tm;
}

struct tm* localtime(const time_t* timer) {
    return gmtime(timer);
}

char* asctime(const struct tm* timeptr) {
    static char buf[32];
    int year;

    if (!timeptr) {
        errno = EINVAL;
        return NULL;
    }

    year = timeptr->tm_year + 1900;
    snprintf(buf, sizeof(buf), "%.3s %.3s %2d %02d:%02d:%02d %04d\n",
             (timeptr->tm_wday >= 0 && timeptr->tm_wday < 7) ? g_time_abday[timeptr->tm_wday] : "???",
             (timeptr->tm_mon >= 0 && timeptr->tm_mon < 12) ? g_time_abmon[timeptr->tm_mon] : "???",
             timeptr->tm_mday,
             timeptr->tm_hour,
             timeptr->tm_min,
             timeptr->tm_sec,
             year);
    return buf;
}

char* ctime(const time_t* timer) {
    return asctime(localtime(timer));
}

static char* append_i64_limited(char* out, char* end, int64_t value) {
    char tmp[32];

    snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    return append_string_limited(out, end, tmp);
}

static int tm_year_full(const struct tm* tm) {
    return tm->tm_year + 1900;
}

static int tm_year_mod100(const struct tm* tm) {
    int year = tm_year_full(tm) % 100;

    if (year < 0) {
        year += 100;
    }
    return year;
}

static int tm_hour12(const struct tm* tm) {
    int hour = tm->tm_hour % 12;

    if (hour < 0) {
        hour += 12;
    }
    return hour == 0 ? 12 : hour;
}

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm) {
    char* out = s;
    char* end = s ? s + max - (max ? 1u : 0u) : NULL;

    if (!s || max == 0 || !format || !tm) {
        return 0;
    }

    while (*format) {
        int year = tm_year_full(tm);
        int wday = tm->tm_wday;
        int mon = tm->tm_mon;

        if (*format != '%') {
            out = append_char_limited(out, end, *format++);
            continue;
        }

        format++;
        if (!*format) {
            out = append_char_limited(out, end, '%');
            break;
        }

        switch (*format) {
            case '%':
                out = append_char_limited(out, end, '%');
                break;
            case 'a':
                out = append_string_limited(out, end, (wday >= 0 && wday < 7) ? g_time_abday[wday] : "???");
                break;
            case 'A':
                out = append_string_limited(out, end, (wday >= 0 && wday < 7) ? g_time_day[wday] : "???");
                break;
            case 'b':
            case 'h':
                out = append_string_limited(out, end, (mon >= 0 && mon < 12) ? g_time_abmon[mon] : "???");
                break;
            case 'B':
                out = append_string_limited(out, end, (mon >= 0 && mon < 12) ? g_time_mon[mon] : "???");
                break;
            case 'C':
                out = append_dec2_limited(out, end, year / 100);
                break;
            case 'd':
                out = append_dec2_limited(out, end, tm->tm_mday);
                break;
            case 'D':
                out = append_dec2_limited(out, end, tm->tm_mon + 1);
                out = append_char_limited(out, end, '/');
                out = append_dec2_limited(out, end, tm->tm_mday);
                out = append_char_limited(out, end, '/');
                out = append_dec2_limited(out, end, tm_year_mod100(tm));
                break;
            case 'e':
                out = append_dec2_space_limited(out, end, tm->tm_mday);
                break;
            case 'F':
                out = append_dec4_limited(out, end, year);
                out = append_char_limited(out, end, '-');
                out = append_dec2_limited(out, end, tm->tm_mon + 1);
                out = append_char_limited(out, end, '-');
                out = append_dec2_limited(out, end, tm->tm_mday);
                break;
            case 'H':
                out = append_dec2_limited(out, end, tm->tm_hour);
                break;
            case 'I':
                out = append_dec2_limited(out, end, tm_hour12(tm));
                break;
            case 'j':
                out = append_dec3_limited(out, end, tm->tm_yday + 1);
                break;
            case 'm':
                out = append_dec2_limited(out, end, tm->tm_mon + 1);
                break;
            case 'M':
                out = append_dec2_limited(out, end, tm->tm_min);
                break;
            case 'n':
                out = append_char_limited(out, end, '\n');
                break;
            case 'p':
                out = append_string_limited(out, end, tm->tm_hour < 12 ? "AM" : "PM");
                break;
            case 'r':
                out = append_dec2_limited(out, end, tm_hour12(tm));
                out = append_char_limited(out, end, ':');
                out = append_dec2_limited(out, end, tm->tm_min);
                out = append_char_limited(out, end, ':');
                out = append_dec2_limited(out, end, tm->tm_sec);
                out = append_char_limited(out, end, ' ');
                out = append_string_limited(out, end, tm->tm_hour < 12 ? "AM" : "PM");
                break;
            case 'R':
                out = append_dec2_limited(out, end, tm->tm_hour);
                out = append_char_limited(out, end, ':');
                out = append_dec2_limited(out, end, tm->tm_min);
                break;
            case 's': {
                struct tm tmp = *tm;
                out = append_i64_limited(out, end, (int64_t)mktime(&tmp));
                break;
            }
            case 'S':
                out = append_dec2_limited(out, end, tm->tm_sec);
                break;
            case 't':
                out = append_char_limited(out, end, '\t');
                break;
            case 'T':
            case 'X':
                out = append_dec2_limited(out, end, tm->tm_hour);
                out = append_char_limited(out, end, ':');
                out = append_dec2_limited(out, end, tm->tm_min);
                out = append_char_limited(out, end, ':');
                out = append_dec2_limited(out, end, tm->tm_sec);
                break;
            case 'u':
                out = append_char_limited(out, end, (char)('0' + (tm->tm_wday == 0 ? 7 : tm->tm_wday)));
                break;
            case 'w':
                out = append_char_limited(out, end, (char)('0' + tm->tm_wday));
                break;
            case 'x':
                out = append_dec2_limited(out, end, tm->tm_mon + 1);
                out = append_char_limited(out, end, '/');
                out = append_dec2_limited(out, end, tm->tm_mday);
                out = append_char_limited(out, end, '/');
                out = append_dec2_limited(out, end, tm_year_mod100(tm));
                break;
            case 'y':
                out = append_dec2_limited(out, end, tm_year_mod100(tm));
                break;
            case 'Y':
                out = append_dec4_limited(out, end, year);
                break;
            case 'z':
                out = append_string_limited(out, end, "+0000");
                break;
            case 'Z':
                out = append_string_limited(out, end, "UTC");
                break;
            case 'c':
                out = append_string_limited(out, end, (wday >= 0 && wday < 7) ? g_time_abday[wday] : "???");
                out = append_char_limited(out, end, ' ');
                out = append_string_limited(out, end, (mon >= 0 && mon < 12) ? g_time_abmon[mon] : "???");
                out = append_char_limited(out, end, ' ');
                out = append_dec2_space_limited(out, end, tm->tm_mday);
                out = append_char_limited(out, end, ' ');
                out = append_dec2_limited(out, end, tm->tm_hour);
                out = append_char_limited(out, end, ':');
                out = append_dec2_limited(out, end, tm->tm_min);
                out = append_char_limited(out, end, ':');
                out = append_dec2_limited(out, end, tm->tm_sec);
                out = append_char_limited(out, end, ' ');
                out = append_dec4_limited(out, end, year);
                break;
            default:
                out = append_char_limited(out, end, '%');
                out = append_char_limited(out, end, *format);
                break;
        }
        format++;
    }

    if (out > end) {
        s[0] = '\0';
        return 0;
    }
    *out = '\0';
    return (size_t)(out - s);
}
static const char* strptime_read_number(const char* s, int min_width, int max_width, int min_value, int max_value, int* out) {
    int value = 0;
    int count = 0;

    while (count < max_width && isdigit((unsigned char)s[count])) {
        value = value * 10 + (s[count] - '0');
        count++;
    }
    if (count < min_width || value < min_value || value > max_value) {
        return NULL;
    }
    *out = value;
    return s + count;
}

static const char* strptime_match_word(const char* s, const char* const* words, size_t count, int* out) {
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(words[i]);
        if (strncasecmp(s, words[i], len) == 0) {
            *out = (int)i;
            return s + len;
        }
    }
    return NULL;
}

char* strptime(const char* s, const char* format, struct tm* tm) {
    static const char* const months_short[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    static const char* const months_long[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };

    if (!s || !format || !tm) {
        errno = EINVAL;
        return NULL;
    }

    while (*format) {
        int value = 0;
        const char* next = NULL;

        if (isspace((unsigned char)*format)) {
            while (isspace((unsigned char)*format)) {
                format++;
            }
            while (isspace((unsigned char)*s)) {
                s++;
            }
            continue;
        }

        if (*format != '%') {
            if (*s != *format) {
                return NULL;
            }
            s++;
            format++;
            continue;
        }

        format++;
        switch (*format) {
            case '%':
                if (*s != '%') {
                    return NULL;
                }
                s++;
                break;
            case 'Y':
                next = strptime_read_number(s, 1, 4, 0, 9999, &value);
                if (!next) return NULL;
                tm->tm_year = value - 1900;
                s = next;
                break;
            case 'y':
                next = strptime_read_number(s, 2, 2, 0, 99, &value);
                if (!next) return NULL;
                tm->tm_year = value + (value >= 69 ? 0 : 100);
                s = next;
                break;
            case 'm':
                next = strptime_read_number(s, 1, 2, 1, 12, &value);
                if (!next) return NULL;
                tm->tm_mon = value - 1;
                s = next;
                break;
            case 'b':
            case 'h':
                next = strptime_match_word(s, months_short, 12, &value);
                if (!next) return NULL;
                tm->tm_mon = value;
                s = next;
                break;
            case 'B':
                next = strptime_match_word(s, months_long, 12, &value);
                if (!next) return NULL;
                tm->tm_mon = value;
                s = next;
                break;
            case 'd':
            case 'e':
                next = strptime_read_number(s, 1, 2, 1, 31, &value);
                if (!next) return NULL;
                tm->tm_mday = value;
                s = next;
                break;
            case 'H':
                next = strptime_read_number(s, 1, 2, 0, 23, &value);
                if (!next) return NULL;
                tm->tm_hour = value;
                s = next;
                break;
            case 'M':
                next = strptime_read_number(s, 1, 2, 0, 59, &value);
                if (!next) return NULL;
                tm->tm_min = value;
                s = next;
                break;
            case 'S':
                next = strptime_read_number(s, 1, 2, 0, 60, &value);
                if (!next) return NULL;
                tm->tm_sec = value;
                s = next;
                break;
            case 'F':
                s = strptime(s, "%Y-%m-%d", tm);
                if (!s) return NULL;
                break;
            case 'T':
                s = strptime(s, "%H:%M:%S", tm);
                if (!s) return NULL;
                break;
            case 'n':
            case 't':
                while (isspace((unsigned char)*s)) {
                    s++;
                }
                break;
            default:
                return NULL;
        }
        if (*format) {
            format++;
        }
    }

    return (char*)s;
}

int clock_gettime(clockid_t clk_id, struct timespec* tp) {
    kiwi_timespec_t kt;

    if (!tp || (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC)) {
        errno = EINVAL;
        return -1;
    }

    if (sys_clock_gettime(clk_id == CLOCK_MONOTONIC ? KIWI_CLOCK_MONOTONIC : KIWI_CLOCK_REALTIME, &kt) < 0) {
        errno = EINVAL;
        return -1;
    }

    tp->tv_sec = (time_t)kt.tv_sec;
    tp->tv_nsec = (long)kt.tv_nsec;
    return 0;
}

int clock_getres(clockid_t clk_id, struct timespec* tp) {
    if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    if (tp) {
        tp->tv_sec = 0;
        tp->tv_nsec = 10000000L;
    }
    return 0;
}

static int timespec_before(const struct timespec* a, const struct timespec* b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static void timespec_add(struct timespec* out, const struct timespec* a, const struct timespec* b) {
    out->tv_sec = a->tv_sec + b->tv_sec;
    out->tv_nsec = a->tv_nsec + b->tv_nsec;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec++;
        out->tv_nsec -= 1000000000L;
    }
}

int clock_nanosleep(clockid_t clk_id, int flags, const struct timespec* req, struct timespec* rem) {
    struct timespec target;
    struct timespec now;

    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L ||
        (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) ||
        (flags & ~TIMER_ABSTIME) != 0) {
        errno = EINVAL;
        return EINVAL;
    }

    if (flags & TIMER_ABSTIME) {
        target = *req;
    } else {
        if (clock_gettime(clk_id, &now) != 0) {
            return errno;
        }
        timespec_add(&target, &now, req);
    }

    for (;;) {
        if (clock_gettime(clk_id, &now) != 0) {
            return errno;
        }
        if (!timespec_before(&now, &target)) {
            break;
        }
        sys_yield();
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    int rc = clock_nanosleep(CLOCK_MONOTONIC, 0, req, rem);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

int timespec_get(struct timespec* ts, int base) {
    if (!ts || base != TIME_UTC) {
        return 0;
    }
    return clock_gettime(CLOCK_REALTIME, ts) == 0 ? base : 0;
}

int sigemptyset(sigset_t* set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t* set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = ~0ul;
    return 0;
}

int sigaddset(sigset_t* set, int signum) {
    if (!set || signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set |= 1ul << signum;
    return 0;
}

int sigdelset(sigset_t* set, int signum) {
    if (!set || signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1ul << signum);
    return 0;
}

int sigismember(const sigset_t* set, int signum) {
    if (!set || signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return ((*set & (1ul << signum)) != 0) ? 1 : 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    if (oldset) {
        *oldset = g_signal_mask;
    }
    if (!set) {
        return 0;
    }

    switch (how) {
        case SIG_BLOCK:
            g_signal_mask |= *set;
            return 0;
        case SIG_UNBLOCK:
            g_signal_mask &= ~(*set);
            return 0;
        case SIG_SETMASK:
            g_signal_mask = *set;
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

int sigpending(sigset_t* set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sigsuspend(const sigset_t* mask) {
    sigset_t old = g_signal_mask;

    if (!mask) {
        errno = EINVAL;
        return -1;
    }
    g_signal_mask = *mask;
    g_signal_mask = old;
    errno = EINTR;
    return -1;
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    if (signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }

    if (oldact) {
        oldact->sa_handler = g_signal_handlers[signum] ? g_signal_handlers[signum] : SIG_DFL;
        oldact->sa_mask = 0;
        oldact->sa_flags = 0;
    }
    if (act) {
        g_signal_handlers[signum] = act->sa_handler;
    }
    return 0;
}

sighandler_t signal(int signum, sighandler_t handler) {
    sighandler_t old;

    if (signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return SIG_ERR;
    }

    old = g_signal_handlers[signum] ? g_signal_handlers[signum] : SIG_DFL;
    g_signal_handlers[signum] = handler;
    return old;
}

int siginterrupt(int signum, int flag) {
    (void)flag;
    if (signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int raise(int signum) {
    sighandler_t handler;

    if (signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }

    if ((g_signal_mask & (1ul << signum)) != 0) {
        return 0;
    }

    handler = g_signal_handlers[signum];
    if (handler && handler != SIG_DFL && handler != SIG_IGN) {
        handler(signum);
    }
    return 0;
}

int kill(int pid, int signum) {
    pid_t self = getpid();

    if (signum < 0 || signum >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (pid != 0 && pid != self) {
        errno = ESRCH;
        return -1;
    }
    if (signum == 0) {
        return 0;
    }
    return raise(signum);
}

unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    return 0;
}

int pause(void) {
    errno = EINTR;
    return -1;
}

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout) {
    int ready = 0;

    (void)timeout;
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    if (exceptfds) {
        FD_ZERO(exceptfds);
    }

    for (int fd = 0; fd < nfds; fd++) {
        int selected = (readfds && FD_ISSET(fd, readfds)) ||
                       (writefds && FD_ISSET(fd, writefds));
        int fd_ready = 0;

        if (!selected) {
            continue;
        }
        if (fcntl(fd, F_GETFL) < 0) {
            errno = EBADF;
            return -1;
        }
        if (readfds && FD_ISSET(fd, readfds)) {
            fd_ready = 1;
        }
        if (writefds && FD_ISSET(fd, writefds)) {
            fd_ready = 1;
        }
        if (fd_ready) {
            ready++;
        }
    }

    return ready;
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    int ready = 0;

    (void)timeout;
    if (!fds && nfds != 0) {
        errno = EINVAL;
        return -1;
    }

    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) {
            continue;
        }
        if (fcntl(fds[i].fd, F_GETFL) < 0) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (fds[i].events == 0) {
            fds[i].revents = 0;
        }
        if (fds[i].revents) {
            ready++;
        }
    }
    return ready;
}

uid_t getuid(void) {
    return 0;
}

uid_t geteuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

int getgroups(int size, gid_t list[]) {
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0) {
        return 1;
    }
    if (!list) {
        errno = EINVAL;
        return -1;
    }
    list[0] = 0;
    return 1;
}

int setuid(uid_t uid) {
    if (uid != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int seteuid(uid_t euid) {
    return setuid(euid);
}

int setgid(gid_t gid) {
    if (gid != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int setegid(gid_t egid) {
    return setgid(egid);
}

unsigned int sleep(unsigned int seconds) {
    (void)seconds;
    return 0;
}

int usleep(unsigned int usec) {
    (void)usec;
    return 0;
}

int getpagesize(void) {
    return 4096;
}

long sysconf(int name) {
    switch (name) {
        case _SC_PAGESIZE:
            return 4096;
        case _SC_OPEN_MAX:
            return 16;
        case _SC_CLK_TCK:
            return 100;
        case _SC_ARG_MAX:
            return 4096;
        case _SC_CHILD_MAX:
            return 64;
        default:
            errno = EINVAL;
            return -1;
    }
}

long fpathconf(int fd, int name) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    switch (name) {
        case _PC_PIPE_BUF:
            return 4096;
        default:
            errno = EINVAL;
            return -1;
    }
}

long pathconf(const char* path, int name) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    switch (name) {
        case _PC_PIPE_BUF:
            return 4096;
        default:
            errno = EINVAL;
            return -1;
    }
}

int gethostname(char* name, size_t len) {
    static const char hostname[] = "kiwi";

    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    if (sizeof(hostname) > len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, hostname, sizeof(hostname));
    return 0;
}

int getdtablesize(void) {
    return (int)sysconf(_SC_OPEN_MAX);
}

char* getlogin(void) {
    return "root";
}

int getlogin_r(char* name, size_t size) {
    static const char login[] = "root";

    if (!name || size == 0) {
        errno = EINVAL;
        return EINVAL;
    }
    if (sizeof(login) > size) {
        errno = ERANGE;
        return ERANGE;
    }
    memcpy(name, login, sizeof(login));
    return 0;
}

struct passwd* getpwuid(uid_t uid) {
    return uid == 0 ? &g_root_passwd : NULL;
}

struct passwd* getpwnam(const char* name) {
    return (name && strcmp(name, "root") == 0) ? &g_root_passwd : NULL;
}

struct passwd* getpwent(void) {
    if (g_passwd_iter_done) {
        return NULL;
    }
    g_passwd_iter_done = 1;
    return &g_root_passwd;
}

void setpwent(void) {
    g_passwd_iter_done = 0;
}

void endpwent(void) {
    g_passwd_iter_done = 0;
}

struct group* getgrgid(gid_t gid) {
    return gid == 0 ? &g_root_group : NULL;
}

struct group* getgrnam(const char* name) {
    return (name && strcmp(name, "root") == 0) ? &g_root_group : NULL;
}

int getrlimit(int resource, struct rlimit* rlim) {
    if (!rlim) {
        errno = EINVAL;
        return -1;
    }
    switch (resource) {
        case RLIMIT_NOFILE:
            rlim->rlim_cur = 16;
            rlim->rlim_max = 16;
            return 0;
        case RLIMIT_STACK:
        case RLIMIT_DATA:
        case RLIMIT_AS:
        case RLIMIT_FSIZE:
        case RLIMIT_CORE:
        case RLIMIT_CPU:
            rlim->rlim_cur = RLIM_INFINITY;
            rlim->rlim_max = RLIM_INFINITY;
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

int setrlimit(int resource, const struct rlimit* rlim) {
    struct rlimit current;

    if (!rlim) {
        errno = EINVAL;
        return -1;
    }
    if (getrlimit(resource, &current) != 0) {
        return -1;
    }
    if (rlim->rlim_cur > current.rlim_max || rlim->rlim_max > current.rlim_max) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int getrusage(int who, struct rusage* usage) {
    if (!usage || (who != RUSAGE_SELF && who != RUSAGE_CHILDREN)) {
        errno = EINVAL;
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    return 0;
}

char* basename(char* path) {
    char* slash = NULL;
    size_t len = 0;

    if (!path || !*path) {
        return ".";
    }
    len = strlen(path);
    while (len > 1u && path[len - 1u] == '/') {
        path[--len] = '\0';
    }
    slash = strrchr(path, '/');
    if (!slash) {
        return path;
    }
    if (slash[1] == '\0') {
        return slash;
    }
    return slash + 1;
}

char* dirname(char* path) {
    char* slash = NULL;
    size_t len = 0;

    if (!path || !*path) {
        return ".";
    }
    len = strlen(path);
    while (len > 1u && path[len - 1u] == '/') {
        path[--len] = '\0';
    }
    slash = strrchr(path, '/');
    if (!slash) {
        return ".";
    }
    if (slash == path) {
        path[1] = '\0';
        return path;
    }
    *slash = '\0';
    return path;
}

int uname(struct utsname* buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    strcpy(buf->sysname, "KiwiOS");
    strcpy(buf->nodename, "kiwi");
    strcpy(buf->release, "0.1");
    strcpy(buf->version, "KiwiOS userspace ABI");
    strcpy(buf->machine, "x86_64");
    return 0;
}

static int glob_match_class(const char** pattern, char ch) {
    const char* p = *pattern + 1;
    int negate = 0;
    int matched = 0;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }

    while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            char lo = p[0];
            char hi = p[2];
            if (lo <= ch && ch <= hi) {
                matched = 1;
            }
            p += 3;
        } else {
            if (*p == ch) {
                matched = 1;
            }
            p++;
        }
    }

    if (*p != ']') {
        return -1;
    }

    *pattern = p;
    return negate ? !matched : matched;
}
static int fnmatch_ch_eq(char a, char b, int flags) {
    if ((flags & FNM_CASEFOLD) != 0) {
        return tolower((unsigned char)a) == tolower((unsigned char)b);
    }
    return a == b;
}

static int fnmatch_impl(const char* pattern, const char* string, int flags, int component_start) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') {
                pattern++;
            }
            if (*pattern == '\0') {
                if ((flags & FNM_PATHNAME) != 0 && strchr(string, '/')) {
                    return 0;
                }
                return 1;
            }
            while (*string) {
                if ((flags & FNM_PATHNAME) != 0 && *string == '/') {
                    return 0;
                }
                if (fnmatch_impl(pattern, string, flags, component_start && *string == '/')) {
                    return 1;
                }
                component_start = *string == '/';
                string++;
            }
            return 0;
        }

        if (*string == '\0') {
            return 0;
        }
        if ((flags & FNM_PERIOD) != 0 && component_start && *string == '.' &&
            (*pattern == '?' || *pattern == '*' || *pattern == '[')) {
            return 0;
        }

        if (*pattern == '?') {
            if ((flags & FNM_PATHNAME) != 0 && *string == '/') {
                return 0;
            }
            pattern++;
            component_start = *string == '/';
            string++;
            continue;
        }

        if (*pattern == '[') {
            const char* class_pattern = pattern;
            int matched;
            if ((flags & FNM_PATHNAME) != 0 && *string == '/') {
                return 0;
            }
            matched = glob_match_class(&class_pattern, *string);
            if (matched < 0) {
                if (!fnmatch_ch_eq(*pattern, *string, flags)) {
                    return 0;
                }
            } else {
                pattern = class_pattern;
                if (!matched) {
                    return 0;
                }
            }
            pattern++;
            component_start = *string == '/';
            string++;
            continue;
        }

        if (*pattern == '\\' && (flags & FNM_NOESCAPE) == 0 && pattern[1] != '\0') {
            pattern++;
        }
        if (!fnmatch_ch_eq(*pattern, *string, flags)) {
            return 0;
        }
        pattern++;
        component_start = *string == '/';
        string++;
    }

    return *string == '\0';
}

int fnmatch(const char* pattern, const char* string, int flags) {
    if (!pattern || !string) {
        return FNM_NOMATCH;
    }
    return fnmatch_impl(pattern, string, flags, 1) ? 0 : FNM_NOMATCH;
}

typedef struct {
    char** items;
    size_t count;
    size_t cap;
} glob_list_t;

static int glob_has_magic(const char* pattern) {
    while (pattern && *pattern) {
        if (*pattern == '*' || *pattern == '?' || *pattern == '[') {
            return 1;
        }
        pattern++;
    }
    return 0;
}

static int glob_component_has_magic(const char* component, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (component[i] == '*' || component[i] == '?' || component[i] == '[') {
            return 1;
        }
    }
    return 0;
}

static int glob_list_push(glob_list_t* list, const char* path) {
    char** next = NULL;
    char* copy = NULL;

    if (!list || !path) {
        return GLOB_ABORTED;
    }

    copy = strdup(path);
    if (!copy) {
        return GLOB_NOSPACE;
    }

    if (list->count == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 8u;
        next = (char**)realloc(list->items, next_cap * sizeof(char*));
        if (!next) {
            free(copy);
            return GLOB_NOSPACE;
        }
        list->items = next;
        list->cap = next_cap;
    }

    list->items[list->count++] = copy;
    return 0;
}

static void glob_list_free(glob_list_t* list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int glob_join_path(char* out, size_t out_size, const char* prefix, const char* name) {
    size_t prefix_len = strlen(prefix);
    size_t name_len = strlen(name);

    if (prefix_len == 0) {
        if (name_len + 1u > out_size) {
            return 0;
        }
        memcpy(out, name, name_len + 1u);
        return 1;
    }

    if (strcmp(prefix, "/") == 0) {
        if (1u + name_len + 1u > out_size) {
            return 0;
        }
        out[0] = '/';
        memcpy(out + 1u, name, name_len + 1u);
        return 1;
    }

    if (prefix_len + 1u + name_len + 1u > out_size) {
        return 0;
    }

    memcpy(out, prefix, prefix_len);
    out[prefix_len] = '/';
    memcpy(out + prefix_len + 1u, name, name_len + 1u);
    return 1;
}

static int glob_mark_if_dir(char* path, size_t path_size) {
    struct stat st;
    size_t len;

    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 1;
    }
    len = strlen(path);
    if (len > 0 && path[len - 1u] == '/') {
        return 1;
    }
    if (len + 2u > path_size) {
        return 0;
    }
    path[len] = '/';
    path[len + 1u] = '\0';
    return 1;
}

static int glob_expand_recursive(const char* prefix,
                                 const char* rest,
                                 int flags,
                                 glob_list_t* out) {
    char component[128];
    const char* slash = NULL;
    const char* next = NULL;
    size_t comp_len = 0;
    int has_more = 0;
    int magic = 0;

    while (*rest == '/') {
        rest++;
    }
    if (*rest == '\0') {
        char final_path[512];
        const char* value = *prefix ? prefix : ".";

        if (strlen(value) + 1u > sizeof(final_path)) {
            return GLOB_ABORTED;
        }
        strcpy(final_path, value);
        if ((flags & GLOB_MARK) != 0 && !glob_mark_if_dir(final_path, sizeof(final_path))) {
            return GLOB_ABORTED;
        }
        return glob_list_push(out, final_path);
    }

    slash = strchr(rest, '/');
    comp_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (comp_len == 0 || comp_len >= sizeof(component)) {
        return GLOB_ABORTED;
    }
    memcpy(component, rest, comp_len);
    component[comp_len] = '\0';

    next = slash ? slash + 1 : rest + comp_len;
    while (*next == '/') {
        next++;
    }
    has_more = *next != '\0';
    magic = glob_component_has_magic(component, comp_len);

    if (!magic) {
        char child[512];
        if (!glob_join_path(child, sizeof(child), prefix, component)) {
            return GLOB_ABORTED;
        }
        if (has_more) {
            DIR* dir = opendir(child);
            if (!dir) {
                return 0;
            }
            closedir(dir);
            return glob_expand_recursive(child, next, flags, out);
        }
        if (access(child, F_OK) == 0) {
            if ((flags & GLOB_MARK) != 0 && !glob_mark_if_dir(child, sizeof(child))) {
                return GLOB_ABORTED;
            }
            return glob_list_push(out, child);
        }
        return 0;
    }

    {
        const char* dir_path = *prefix ? prefix : ".";
        DIR* dir = opendir(dir_path);
        int rc = 0;

        if (!dir) {
            return 0;
        }

        for (;;) {
            struct dirent* ent = readdir(dir);
            char child[512];

            if (!ent) {
                break;
            }
            if (ent->d_name[0] == '.' && component[0] != '.') {
                continue;
            }
            if (fnmatch(component, ent->d_name, 0) != 0) {
                continue;
            }
            if (!glob_join_path(child, sizeof(child), prefix, ent->d_name)) {
                closedir(dir);
                return GLOB_ABORTED;
            }
            if (has_more) {
                DIR* child_dir = opendir(child);
                if (!child_dir) {
                    continue;
                }
                closedir(child_dir);
                rc = glob_expand_recursive(child, next, flags, out);
            } else {
                if ((flags & GLOB_MARK) != 0 && !glob_mark_if_dir(child, sizeof(child))) {
                    closedir(dir);
                    return GLOB_ABORTED;
                }
                rc = glob_list_push(out, child);
            }
            if (rc != 0) {
                closedir(dir);
                return rc;
            }
        }
        closedir(dir);
    }

    return 0;
}

static void glob_sort(glob_list_t* list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i + 1u < list->count; i++) {
        for (size_t j = i + 1u; j < list->count; j++) {
            if (strcmp(list->items[i], list->items[j]) > 0) {
                char* tmp = list->items[i];
                list->items[i] = list->items[j];
                list->items[j] = tmp;
            }
        }
    }
}

int glob(const char* pattern, int flags, int (*errfunc)(const char*, int), glob_t* pglob) {
    size_t requested_offs = 0;
    size_t offs = 0;
    size_t old_count = 0;
    glob_list_t matches;
    char** pathv = NULL;
    int rc = 0;

    (void)errfunc;
    memset(&matches, 0, sizeof(matches));
    if (!pattern || !pglob) {
        errno = EINVAL;
        return GLOB_ABORTED;
    }

    requested_offs = pglob->gl_offs;
    if ((flags & GLOB_APPEND) != 0) {
        old_count = pglob->gl_pathc;
        offs = pglob->gl_offs;
    } else {
        memset(pglob, 0, sizeof(*pglob));
        offs = (flags & GLOB_DOOFFS) ? requested_offs : 0;
    }

    if (!glob_has_magic(pattern)) {
        if (access(pattern, F_OK) != 0 && (flags & GLOB_NOCHECK) == 0) {
            return GLOB_NOMATCH;
        }
        rc = glob_list_push(&matches, pattern);
        if (rc != 0) {
            return rc;
        }
    } else {
        const char* rest = pattern;
        char prefix[2] = "";
        if (*rest == '/') {
            prefix[0] = '/';
            prefix[1] = '\0';
            while (*rest == '/') {
                rest++;
            }
        }
        rc = glob_expand_recursive(prefix, rest, flags, &matches);
        if (rc != 0) {
            glob_list_free(&matches);
            return rc;
        }
        if (matches.count == 0) {
            if ((flags & GLOB_NOCHECK) == 0) {
                return GLOB_NOMATCH;
            }
            rc = glob_list_push(&matches, pattern);
            if (rc != 0) {
                return rc;
            }
        } else {
            if ((flags & GLOB_NOSORT) == 0) {
                glob_sort(&matches);
            }
        }
    }

    pathv = (char**)calloc(offs + old_count + matches.count + 1u, sizeof(char*));
    if (!pathv) {
        glob_list_free(&matches);
        return GLOB_NOSPACE;
    }

    if ((flags & GLOB_APPEND) != 0 && pglob->gl_pathv) {
        for (size_t i = 0; i < old_count; i++) {
            pathv[offs + i] = pglob->gl_pathv[offs + i];
        }
        free(pglob->gl_pathv);
    }

    for (size_t i = 0; i < matches.count; i++) {
        pathv[offs + old_count + i] = matches.items[i];
    }
    free(matches.items);

    pglob->gl_pathc = old_count + matches.count;
    pglob->gl_offs = offs;
    pglob->gl_pathv = pathv;
    return 0;
}

void globfree(glob_t* pglob) {
    if (!pglob || !pglob->gl_pathv) {
        return;
    }

    for (size_t i = 0; i < pglob->gl_pathc; i++) {
        free(pglob->gl_pathv[pglob->gl_offs + i]);
    }
    free(pglob->gl_pathv);
    memset(pglob, 0, sizeof(*pglob));
}

typedef struct {
    const char* start;
    const char* end;
    const char* next;
    int type;
    int negate;
    char literal;
} regex_atom_t;

#define REGEX_ATOM_LITERAL 0
#define REGEX_ATOM_DOT     1
#define REGEX_ATOM_CLASS   2

static int regex_fold_char(int ch, int cflags) {
    return (cflags & REG_ICASE) ? tolower((unsigned char)ch) : ch;
}

static int regex_char_equal(int a, int b, int cflags) {
    return regex_fold_char(a, cflags) == regex_fold_char(b, cflags);
}

static int regex_is_line_start(const regex_t* preg, const char* string, const char* text, int eflags) {
    if (text == string) {
        return (eflags & REG_NOTBOL) == 0;
    }
    return (preg->cflags & REG_NEWLINE) && text[-1] == '\n';
}

static int regex_is_line_end(const regex_t* preg, const char* text, int eflags) {
    if (*text == '\0') {
        return (eflags & REG_NOTEOL) == 0;
    }
    return (preg->cflags & REG_NEWLINE) && *text == '\n';
}

static const char* regex_class_next_char(const char* p, const char* end, int* ch) {
    if (p >= end) {
        return NULL;
    }
    if (*p == '\\' && p + 1 < end) {
        *ch = (unsigned char)p[1];
        return p + 2;
    }
    *ch = (unsigned char)*p;
    return p + 1;
}

static int regex_named_class_match(const char* name, size_t len, int ch) {
    unsigned char c = (unsigned char)ch;

    if (len == 5 && strncmp(name, "alnum", len) == 0) return isalnum(c);
    if (len == 5 && strncmp(name, "alpha", len) == 0) return isalpha(c);
    if (len == 5 && strncmp(name, "blank", len) == 0) return c == ' ' || c == '\t';
    if (len == 5 && strncmp(name, "cntrl", len) == 0) return iscntrl(c);
    if (len == 5 && strncmp(name, "digit", len) == 0) return isdigit(c);
    if (len == 5 && strncmp(name, "graph", len) == 0) return isgraph(c);
    if (len == 5 && strncmp(name, "lower", len) == 0) return islower(c);
    if (len == 5 && strncmp(name, "print", len) == 0) return isprint(c);
    if (len == 5 && strncmp(name, "punct", len) == 0) return ispunct(c);
    if (len == 5 && strncmp(name, "space", len) == 0) return isspace(c);
    if (len == 5 && strncmp(name, "upper", len) == 0) return isupper(c);
    if (len == 6 && strncmp(name, "xdigit", len) == 0) return isxdigit(c);
    return 0;
}

static int regex_class_match(const regex_t* preg, const regex_atom_t* atom, int ch) {
    const char* p = atom->start;
    int matched = 0;

    while (p < atom->end) {
        int first = 0;
        const char* next = NULL;

        if (p + 3 < atom->end && p[0] == '[' && p[1] == ':') {
            const char* class_end = p + 2;
            while (class_end + 1 < atom->end && !(class_end[0] == ':' && class_end[1] == ']')) {
                class_end++;
            }
            if (class_end + 1 < atom->end && regex_named_class_match(p + 2, (size_t)(class_end - (p + 2)), ch)) {
                matched = 1;
            }
            if (class_end + 1 < atom->end) {
                p = class_end + 2;
                continue;
            }
        }

        next = regex_class_next_char(p, atom->end, &first);
        if (!next) {
            break;
        }

        if (next < atom->end && *next == '-' && next + 1 < atom->end) {
            int last = 0;
            const char* after_last = regex_class_next_char(next + 1, atom->end, &last);
            int folded_ch = regex_fold_char(ch, preg->cflags);
            int folded_first = regex_fold_char(first, preg->cflags);
            int folded_last = regex_fold_char(last, preg->cflags);

            if (after_last) {
                if (folded_first <= folded_last) {
                    matched |= folded_ch >= folded_first && folded_ch <= folded_last;
                } else {
                    matched |= folded_ch >= folded_last && folded_ch <= folded_first;
                }
                p = after_last;
                continue;
            }
        }

        matched |= regex_char_equal(first, ch, preg->cflags);
        p = next;
    }

    return atom->negate ? !matched : matched;
}

static int regex_parse_atom(const char* pat, regex_atom_t* atom, int* err) {
    const char* p = pat;

    memset(atom, 0, sizeof(*atom));
    if (!p || *p == '\0') {
        return 0;
    }

    if (*p == '\\') {
        if (p[1] == '\0') {
            *err = REG_EESCAPE;
            return -1;
        }
        atom->type = REGEX_ATOM_LITERAL;
        atom->literal = p[1];
        atom->next = p + 2;
        return 1;
    }

    if (*p == '.') {
        atom->type = REGEX_ATOM_DOT;
        atom->next = p + 1;
        return 1;
    }

    if (*p == '[') {
        const char* end = p + 1;
        const char* class_start = NULL;

        atom->type = REGEX_ATOM_CLASS;
        atom->negate = (*end == '^' || *end == '!');
        if (atom->negate) {
            end++;
        }
        class_start = end;
        if (*end == ']') {
            end++;
        }
        while (*end && *end != ']') {
            if (end >= class_start && end[0] == '[' && end[1] == ':') {
                const char* named_end = end + 2;
                while (named_end[0] && named_end[1] && !(named_end[0] == ':' && named_end[1] == ']')) {
                    named_end++;
                }
                if (named_end[0] == ':' && named_end[1] == ']') {
                    end = named_end + 2;
                } else {
                    end++;
                }
            } else if (*end == '\\' && end[1]) {
                end += 2;
            } else {
                end++;
            }
        }
        if (*end != ']') {
            *err = REG_EBRACK;
            return -1;
        }
        atom->start = p + 1 + (atom->negate ? 1 : 0);
        atom->end = end;
        atom->next = end + 1;
        return 1;
    }

    atom->type = REGEX_ATOM_LITERAL;
    atom->literal = *p;
    atom->next = p + 1;
    return 1;
}

static int regex_atom_match(const regex_t* preg, const regex_atom_t* atom, const char* text) {
    if (!text || *text == '\0') {
        return 0;
    }

    switch (atom->type) {
        case REGEX_ATOM_DOT:
            return !((preg->cflags & REG_NEWLINE) && *text == '\n');
        case REGEX_ATOM_CLASS:
            return regex_class_match(preg, atom, (unsigned char)*text);
        default:
            return regex_char_equal(atom->literal, (unsigned char)*text, preg->cflags);
    }
}

static int regex_match_here(const regex_t* preg,
                            const char* pat,
                            const char* string,
                            const char* text,
                            int eflags,
                            const char** out_end) {
    regex_atom_t atom;
    int err = 0;
    int parsed = 0;
    char quant = '\0';

    if (*pat == '\0') {
        *out_end = text;
        return 1;
    }

    if (pat == preg->pattern && *pat == '^') {
        return regex_is_line_start(preg, string, text, eflags) &&
               regex_match_here(preg, pat + 1, string, text, eflags, out_end);
    }

    if (pat[0] == '$' && pat[1] == '\0') {
        if (regex_is_line_end(preg, text, eflags)) {
            *out_end = text;
            return 1;
        }
        return 0;
    }

    parsed = regex_parse_atom(pat, &atom, &err);
    if (parsed <= 0) {
        return 0;
    }

    quant = *atom.next;
    if (quant == '*' || quant == '+' || quant == '?') {
        const char* after = atom.next + 1;
        const char* cur = text;
        size_t count = 0;
        size_t min_count = quant == '+' ? 1u : 0u;
        size_t max_count = quant == '?' ? 1u : (size_t)-1;

        while (count < max_count && regex_atom_match(preg, &atom, cur)) {
            cur++;
            count++;
        }

        while (count >= min_count) {
            if (regex_match_here(preg, after, string, cur, eflags, out_end)) {
                return 1;
            }
            if (count == 0) {
                break;
            }
            cur--;
            count--;
        }
        return 0;
    }

    if (!regex_atom_match(preg, &atom, text)) {
        return 0;
    }

    return regex_match_here(preg, atom.next, string, text + 1, eflags, out_end);
}

static int regex_validate_pattern(const char* pattern, int* err) {
    const char* p = pattern;

    while (*p) {
        regex_atom_t atom;
        int parsed = regex_parse_atom(p, &atom, err);

        if (parsed < 0) {
            return -1;
        }
        if (parsed == 0) {
            break;
        }
        p = atom.next;
        if (*p == '*' || *p == '+' || *p == '?') {
            p++;
        }
    }
    return 0;
}

int regcomp(regex_t* preg, const char* pattern, int cflags) {
    size_t len = 0;
    int err = 0;

    if (!preg || !pattern) {
        return REG_BADPAT;
    }

    len = strlen(pattern);
    if (len >= sizeof(preg->pattern)) {
        preg->errcode = REG_ESPACE;
        return REG_ESPACE;
    }

    if (regex_validate_pattern(pattern, &err) != 0) {
        preg->errcode = err ? err : REG_BADPAT;
        return preg->errcode;
    }

    memcpy(preg->pattern, pattern, len + 1u);
    preg->cflags = cflags;
    preg->errcode = 0;
    return 0;
}

int regexec(const regex_t* preg, const char* string, size_t nmatch, regmatch_t pmatch[], int eflags) {
    const char* start = NULL;

    if (!preg || !string) {
        return REG_NOMATCH;
    }

    for (start = string;; start++) {
        const char* end = NULL;
        if (regex_match_here(preg, preg->pattern, string, start, eflags, &end)) {
            if ((preg->cflags & REG_NOSUB) == 0 && nmatch > 0 && pmatch) {
                pmatch[0].rm_so = (regoff_t)(start - string);
                pmatch[0].rm_eo = (regoff_t)(end - string);
                for (size_t i = 1; i < nmatch; i++) {
                    pmatch[i].rm_so = -1;
                    pmatch[i].rm_eo = -1;
                }
            }
            return 0;
        }
        if (*start == '\0') {
            break;
        }
    }

    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t* preg, char* errbuf, size_t errbuf_size) {
    const char* msg = "bad pattern";
    size_t len = 0;

    if (errcode == 0) {
        msg = "success";
    } else if (errcode == REG_NOMATCH) {
        msg = "no match";
    } else if (errcode == REG_EBRACK) {
        msg = "unmatched bracket";
    } else if (errcode == REG_EESCAPE) {
        msg = "trailing escape";
    } else if (errcode == REG_ESPACE) {
        msg = "pattern too large";
    } else if (preg && preg->errcode == REG_EBRACK) {
        msg = "unmatched bracket";
    } else if (preg && preg->errcode == REG_EESCAPE) {
        msg = "trailing escape";
    } else if (preg && preg->errcode == REG_ESPACE) {
        msg = "pattern too large";
    }

    len = strlen(msg) + 1u;
    if (errbuf && errbuf_size != 0) {
        size_t copy_len = len;
        if (copy_len > errbuf_size) {
            copy_len = errbuf_size;
        }
        memcpy(errbuf, msg, copy_len);
        errbuf[errbuf_size - 1u] = '\0';
    }
    return len;
}

void regfree(regex_t* preg) {
    if (preg) {
        preg->pattern[0] = '\0';
        preg->cflags = 0;
        preg->errcode = 0;
    }
}
