#ifndef KIWILIB_DIRENT_H
#define KIWILIB_DIRENT_H

#include <stdint.h>
#include "abi/kiwi.h"

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

typedef struct DIR DIR;

struct dirent {
    uint32_t d_ino;
    uint8_t d_type;
    char d_name[KIWI_DIRENT_NAME_MAX];
};

DIR* opendir(const char* path);
struct dirent* readdir(DIR* dirp);
void rewinddir(DIR* dirp);
long telldir(DIR* dirp);
void seekdir(DIR* dirp, long loc);
int dirfd(DIR* dirp);
int closedir(DIR* dirp);
int alphasort(const struct dirent** a, const struct dirent** b);
int scandir(const char* dirp,
            struct dirent*** namelist,
            int (*filter)(const struct dirent*),
            int (*compar)(const struct dirent**, const struct dirent**));

#endif // KIWILIB_DIRENT_H
