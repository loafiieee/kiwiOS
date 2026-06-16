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
};

enum {
    KIWI_O_RDONLY = 0,
    KIWI_O_WRONLY = 1,
    KIWI_O_RDWR   = 2,
};

#define KIWI_O_ACCMODE 0x3
#define KIWI_O_CREAT   0x100
#define KIWI_O_TRUNC   0x200
#define KIWI_O_APPEND  0x400

enum {
    KIWI_SEEK_SET = 0,
    KIWI_SEEK_CUR = 1,
    KIWI_SEEK_END = 2,
};

enum {
    KIWI_VNODE_NONE = 0,
    KIWI_VNODE_FILE = 1,
    KIWI_VNODE_DIR  = 2,
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

#endif // ABI_KIWI_H
