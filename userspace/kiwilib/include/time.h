#ifndef KIWILIB_TIME_H
#define KIWILIB_TIME_H

#include "sys/types.h"

#define CLOCKS_PER_SEC 1000000L

typedef long clock_t;
typedef int clockid_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define TIME_UTC        1
#define TIMER_ABSTIME   1

time_t time(time_t* tloc);
clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm* timeptr);
struct tm* localtime(const time_t* timer);
struct tm* gmtime(const time_t* timer);
char* asctime(const struct tm* timeptr);
char* ctime(const time_t* timer);
size_t strftime(char* s, size_t max, const char* format, const struct tm* tm);
char* strptime(const char* s, const char* format, struct tm* tm);
int clock_gettime(clockid_t clk_id, struct timespec* tp);
int clock_getres(clockid_t clk_id, struct timespec* tp);
int clock_nanosleep(clockid_t clk_id, int flags, const struct timespec* req, struct timespec* rem);
int nanosleep(const struct timespec* req, struct timespec* rem);
int timespec_get(struct timespec* ts, int base);

#endif // KIWILIB_TIME_H
