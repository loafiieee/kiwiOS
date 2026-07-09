#ifndef KIWILIB_SYS_TIME_H
#define KIWILIB_SYS_TIME_H

#include "sys/types.h"

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

int gettimeofday(struct timeval* tv, void* tz);
int getitimer(int which, struct itimerval* curr_value);
int setitimer(int which, const struct itimerval* new_value, struct itimerval* old_value);
int utimes(const char* path, const struct timeval times[2]);

#endif // KIWILIB_SYS_TIME_H
