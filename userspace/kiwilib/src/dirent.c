#include <stddef.h>
#include "dirent.h"
#include "errno.h"
#include "kiwi_syscall.h"
#include "stdlib.h"
#include "string.h"

#define DIRENT_PATH_MAX 256u

struct DIR {
    char path[DIRENT_PATH_MAX];
    uint64_t index;
    struct dirent current;
};

DIR* opendir(const char* path) {
    kiwi_stat_t st;
    DIR* dir;
    size_t len;

    if (!path || sys_stat(path, &st) != 0 || st.type != KIWI_VNODE_DIR) {
        errno = ENOENT;
        return NULL;
    }

    len = strlen(path);
    if (len >= DIRENT_PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    dir = (DIR*)malloc(sizeof(DIR));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(dir->path, path, len + 1u);
    dir->index = 0;
    memset(&dir->current, 0, sizeof(dir->current));
    return dir;
}

struct dirent* readdir(DIR* dirp) {
    kiwi_dirent_t ent;
    int64_t ret;

    if (!dirp) {
        errno = EINVAL;
        return NULL;
    }

    ret = sys_readdir(dirp->path, dirp->index, &ent);
    if (ret <= 0) {
        return NULL;
    }

    dirp->index++;
    dirp->current.d_ino = ent.ino;
    dirp->current.d_type = ent.type == KIWI_VNODE_DIR ? DT_DIR :
                           ent.type == KIWI_VNODE_FILE ? DT_REG : DT_UNKNOWN;
    memcpy(dirp->current.d_name, ent.name, sizeof(dirp->current.d_name));
    return &dirp->current;
}

void rewinddir(DIR* dirp) {
    if (dirp) {
        dirp->index = 0;
    }
}

long telldir(DIR* dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    return (long)dirp->index;
}

void seekdir(DIR* dirp, long loc) {
    if (!dirp || loc < 0) {
        errno = EINVAL;
        return;
    }
    dirp->index = (uint64_t)loc;
}

int dirfd(DIR* dirp) {
    (void)dirp;
    errno = ENOTSUP;
    return -1;
}

int closedir(DIR* dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    free(dirp);
    return 0;
}

int alphasort(const struct dirent** a, const struct dirent** b) {
    if (!a || !b || !*a || !*b) {
        return 0;
    }
    return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char* dirp,
            struct dirent*** namelist,
            int (*filter)(const struct dirent*),
            int (*compar)(const struct dirent**, const struct dirent**)) {
    DIR* dir = NULL;
    struct dirent** list = NULL;
    size_t count = 0;
    size_t cap = 0;

    if (!dirp || !namelist) {
        errno = EINVAL;
        return -1;
    }

    dir = opendir(dirp);
    if (!dir) {
        return -1;
    }

    for (;;) {
        struct dirent* ent = readdir(dir);
        struct dirent* copy = NULL;
        struct dirent** next = NULL;

        if (!ent) {
            break;
        }
        if (filter && !filter(ent)) {
            continue;
        }

        copy = (struct dirent*)malloc(sizeof(*copy));
        if (!copy) {
            errno = ENOMEM;
            goto fail;
        }
        memcpy(copy, ent, sizeof(*copy));

        if (count == cap) {
            size_t next_cap = cap ? cap * 2u : 8u;
            next = (struct dirent**)realloc(list, next_cap * sizeof(*list));
            if (!next) {
                free(copy);
                errno = ENOMEM;
                goto fail;
            }
            list = next;
            cap = next_cap;
        }
        list[count++] = copy;
    }

    (void)closedir(dir);

    if (compar) {
        for (size_t i = 0; i + 1u < count; i++) {
            for (size_t j = i + 1u; j < count; j++) {
                if (compar((const struct dirent**)&list[i], (const struct dirent**)&list[j]) > 0) {
                    struct dirent* tmp = list[i];
                    list[i] = list[j];
                    list[j] = tmp;
                }
            }
        }
    }

    *namelist = list;
    return (int)count;

fail:
    for (size_t i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
    (void)closedir(dir);
    return -1;
}
