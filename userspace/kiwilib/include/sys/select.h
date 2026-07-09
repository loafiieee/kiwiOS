#ifndef KIWILIB_SYS_SELECT_H
#define KIWILIB_SYS_SELECT_H

#include <stddef.h>
#include <stdint.h>
#include "sys/time.h"

#define FD_SETSIZE 64

typedef struct {
    uint64_t bits;
} fd_set;

static inline void kiwi_fd_zero(fd_set* set) {
    if (set) {
        set->bits = 0;
    }
}

static inline void kiwi_fd_set(int fd, fd_set* set) {
    if (set && fd >= 0 && fd < FD_SETSIZE) {
        set->bits |= (1ull << fd);
    }
}

static inline void kiwi_fd_clr(int fd, fd_set* set) {
    if (set && fd >= 0 && fd < FD_SETSIZE) {
        set->bits &= ~(1ull << fd);
    }
}

static inline int kiwi_fd_isset(int fd, const fd_set* set) {
    return set && fd >= 0 && fd < FD_SETSIZE && ((set->bits & (1ull << fd)) != 0);
}

#define FD_ZERO(set) kiwi_fd_zero((set))
#define FD_SET(fd, set) kiwi_fd_set((fd), (set))
#define FD_CLR(fd, set) kiwi_fd_clr((fd), (set))
#define FD_ISSET(fd, set) kiwi_fd_isset((fd), (set))

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout);

#endif // KIWILIB_SYS_SELECT_H
