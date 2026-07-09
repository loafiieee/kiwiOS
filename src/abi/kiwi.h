#ifndef ABI_KIWI_H
#define ABI_KIWI_H

#include <stdint.h>

enum {
    KIWI_SYS_EXIT   = 0,
    KIWI_SYS_WRITE  = 1,
    KIWI_SYS_READ   = 2,
    KIWI_SYS_OPEN   = 3,
    KIWI_SYS_CLOSE  = 4,
    KIWI_SYS_BRK    = 5,
    KIWI_SYS_GETPID = 6,
    KIWI_SYS_EXEC   = 7,
    KIWI_SYS_SPAWN  = 8,
    KIWI_SYS_WAITPID = 9,
    KIWI_SYS_STAT   = 10,
    KIWI_SYS_SEEK   = 11,
    KIWI_SYS_YIELD  = 12,
    KIWI_SYS_MKDIR  = 13,
    KIWI_SYS_UNLINK = 14,
    KIWI_SYS_READDIR = 15,
    KIWI_SYS_CONSOLE_INPUT = 16,
    KIWI_SYS_CONSOLE_CLEAR = 17,
    KIWI_SYS_MOUNT = 18,
    KIWI_SYS_DEV_RESCAN = 19,
    KIWI_SYS_EXEC_ARGV = 20,
    KIWI_SYS_SPAWN_ARGV = 21,
    KIWI_SYS_IOCTL = 22,
    KIWI_SYS_CHDIR = 23,
    KIWI_SYS_GETCWD = 24,
    KIWI_SYS_RENAME = 25,
    KIWI_SYS_TRUNCATE = 26,
    KIWI_SYS_FTRUNCATE = 27,
    KIWI_SYS_FSTAT = 28,
    KIWI_SYS_ACCESS = 29,
    KIWI_SYS_DUP = 30,
    KIWI_SYS_DUP2 = 31,
    KIWI_SYS_FCNTL = 32,
    KIWI_SYS_PIPE = 33,
    KIWI_SYS_CLOCK_GETTIME = 34,
    KIWI_SYS_POWEROFF = 35,
    KIWI_SYS_REBOOT = 36,
};

#define KIWI_IOCTL_TCGETS      0x5401u
#define KIWI_IOCTL_TCSETS      0x5402u
#define KIWI_IOCTL_TCSETSW     0x5403u
#define KIWI_IOCTL_TCSETSF     0x5404u
#define KIWI_IOCTL_TIOCGWINSZ  0x5413u
#define KIWI_IOCTL_FIONREAD    0x541Bu

#define KIWI_NCCS 32u

enum {
    KIWI_O_RDONLY = 0,
    KIWI_O_WRONLY = 1,
    KIWI_O_RDWR   = 2,
};

#define KIWI_O_ACCMODE 0x3
#define KIWI_O_CREAT   0x100
#define KIWI_O_TRUNC   0x200
#define KIWI_O_APPEND  0x400

#define KIWI_F_DUPFD   0
#define KIWI_F_GETFD   1
#define KIWI_F_SETFD   2
#define KIWI_F_GETFL   3
#define KIWI_F_SETFL   4
#define KIWI_FD_CLOEXEC 1

enum {
    KIWI_SEEK_SET = 0,
    KIWI_SEEK_CUR = 1,
    KIWI_SEEK_END = 2,
};

enum {
    KIWI_CLOCK_REALTIME = 0,
    KIWI_CLOCK_MONOTONIC = 1,
};

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} kiwi_timespec_t;

enum {
    KIWI_VNODE_NONE = 0,
    KIWI_VNODE_FILE = 1,
    KIWI_VNODE_DIR  = 2,
    KIWI_VNODE_PIPE = 3,
};

#define KIWI_DIRENT_NAME_MAX 256u

typedef struct {
    uint32_t type;
    uint32_t ino;
    uint64_t size;
    uint32_t mode;
    uint32_t link_count;
    uint64_t mtime;
    uint64_t ctime;
} kiwi_stat_t;

typedef struct {
    uint32_t ino;
    uint32_t type;
    char name[KIWI_DIRENT_NAME_MAX];
} kiwi_dirent_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} kiwi_winsize_t;

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[KIWI_NCCS];
} kiwi_termios_t;

#endif // ABI_KIWI_H
