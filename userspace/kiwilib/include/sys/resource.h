#ifndef KIWILIB_SYS_RESOURCE_H
#define KIWILIB_SYS_RESOURCE_H

#include "sys/time.h"
#include "sys/types.h"

#define RLIM_INFINITY ((rlim_t)-1)

#define RLIMIT_CPU    0
#define RLIMIT_FSIZE  1
#define RLIMIT_DATA   2
#define RLIMIT_STACK  3
#define RLIMIT_CORE   4
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
};

int getrlimit(int resource, struct rlimit* rlim);
int setrlimit(int resource, const struct rlimit* rlim);
int getrusage(int who, struct rusage* usage);

#endif // KIWILIB_SYS_RESOURCE_H
