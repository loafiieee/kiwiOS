#ifndef KIWILIB_SYS_IOCTL_H
#define KIWILIB_SYS_IOCTL_H

#include <stdint.h>
#include "abi/kiwi.h"

#define TCGETS      KIWI_IOCTL_TCGETS
#define TCSETS      KIWI_IOCTL_TCSETS
#define TCSETSW     KIWI_IOCTL_TCSETSW
#define TCSETSF     KIWI_IOCTL_TCSETSF
#define TIOCGWINSZ  KIWI_IOCTL_TIOCGWINSZ
#define TIOCGETA    TCGETS
#define TIOCSETA    TCSETS
#define TIOCSETAW   TCSETSW
#define TIOCSETAF   TCSETSF
#define TIOCSWINSZ  0x5414
#define FIONREAD    KIWI_IOCTL_FIONREAD
#define TIOCOUTQ    0x5411
#define TIOCLINUX   0x541C

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

int ioctl(int fd, unsigned long request, ...);

#endif // KIWILIB_SYS_IOCTL_H
