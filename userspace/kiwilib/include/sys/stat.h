#ifndef KIWILIB_SYS_STAT_H
#define KIWILIB_SYS_STAT_H

#include <stdint.h>
#include "sys/types.h"
#include <time.h>

#define S_IFMT  0170000u
#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IFLNK 0120000u
#define S_IFIFO 0010000u
#define S_IFCHR 0020000u
#define S_IFBLK 0060000u
#define S_IFSOCK 0140000u

#define S_IRWXU 0700u
#define S_IRUSR 0400u
#define S_IWUSR 0200u
#define S_IXUSR 0100u
#define S_IRWXG 0070u
#define S_IRGRP 0040u
#define S_IWGRP 0020u
#define S_IXGRP 0010u
#define S_IRWXO 0007u
#define S_IROTH 0004u
#define S_IWOTH 0002u
#define S_IXOTH 0001u

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

struct stat {
    uint32_t st_dev;
    uint32_t st_mode;
    uint32_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    uint64_t st_size;
    uint32_t st_nlink;
    uint32_t st_blksize;
    uint64_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

int stat(const char* path, struct stat* out);
int lstat(const char* path, struct stat* out);
int fstat(int fd, struct stat* out);
int fstatat(int dirfd, const char* path, struct stat* out, int flags);
int mkdir(const char* path, mode_t mode);
int mkdirat(int dirfd, const char* path, mode_t mode);
int chmod(const char* path, mode_t mode);
int fchmod(int fd, mode_t mode);
int fchmodat(int dirfd, const char* path, mode_t mode, int flags);
int futimens(int fd, const struct timespec times[2]);
int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags);
mode_t umask(mode_t mask);

#endif // KIWILIB_SYS_STAT_H
