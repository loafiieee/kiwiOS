#ifndef KIWILIB_SYS_STATVFS_H
#define KIWILIB_SYS_STATVFS_H

#include "sys/types.h"

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

#define ST_RDONLY 0x0001
#define ST_NOSUID 0x0002

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

int statvfs(const char* path, struct statvfs* buf);
int fstatvfs(int fd, struct statvfs* buf);

#endif // KIWILIB_SYS_STATVFS_H
