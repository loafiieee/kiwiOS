#ifndef KIWILIB_FCNTL_H
#define KIWILIB_FCNTL_H

#include "abi/kiwi.h"
#include "sys/types.h"

#define O_RDONLY KIWI_O_RDONLY
#define O_WRONLY KIWI_O_WRONLY
#define O_RDWR   KIWI_O_RDWR
#define O_ACCMODE KIWI_O_ACCMODE
#define O_CREAT  KIWI_O_CREAT
#define O_TRUNC  KIWI_O_TRUNC
#define O_APPEND KIWI_O_APPEND
#define O_EXCL   0x0800
#define O_NONBLOCK 0x1000
#define O_NOCTTY 0x2000
#define O_CLOEXEC 0x4000
#define O_BINARY 0
#define O_TEXT   0

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x0100
#define AT_REMOVEDIR 0x0200
#define AT_EACCESS 0x0400
#define AT_EMPTY_PATH 0x1000

#define F_DUPFD  KIWI_F_DUPFD
#define F_GETFD  KIWI_F_GETFD
#define F_SETFD  KIWI_F_SETFD
#define F_GETFL  KIWI_F_GETFL
#define F_SETFL  KIWI_F_SETFL
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030

#define FD_CLOEXEC KIWI_FD_CLOEXEC

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

struct flock {
    short l_type;
    short l_whence;
    long l_start;
    long l_len;
    int l_pid;
};

int open(const char* path, int flags, ...);
int openat(int dirfd, const char* path, int flags, ...);
int creat(const char* path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif // KIWILIB_FCNTL_H
