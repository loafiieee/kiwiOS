#ifndef KIWILIB_TERMIOS_H
#define KIWILIB_TERMIOS_H

#include "abi/kiwi.h"

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

#define NCCS KIWI_NCCS

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10

#define IGNBRK   0x00000001u
#define BRKINT   0x00000002u
#define IGNPAR   0x00000004u
#define PARMRK   0x00000008u
#define INPCK    0x00000010u
#define ISTRIP   0x00000020u
#define INLCR    0x00000040u
#define IGNCR    0x00000080u
#define ICRNL    0x00000100u
#define IUCLC    0x00000200u
#define IXON     0x00000400u
#define IXANY    0x00000800u
#define IXOFF    0x00001000u
#define IMAXBEL  0x00002000u
#define IUTF8    0x00004000u

#define OPOST    0x00000001u
#define OLCUC    0x00000002u
#define ONLCR    0x00000004u
#define OCRNL    0x00000008u
#define ONOCR    0x00000010u
#define ONLRET   0x00000020u

#define CSIZE    0x00000030u
#define CS5      0x00000000u
#define CS6      0x00000010u
#define CS7      0x00000020u
#define CS8      0x00000030u
#define CSTOPB   0x00000040u
#define CLOCAL   0x00000800u
#define CREAD    0x00000080u
#define PARENB   0x00000100u
#define PARODD   0x00000200u
#define HUPCL    0x00000400u

#define ECHO     0x00000001u
#define ICANON   0x00000002u
#define ISIG     0x00000004u
#define IEXTEN   0x00000008u
#define ECHOE    0x00000010u
#define ECHOK    0x00000020u
#define ECHONL   0x00000040u
#define NOFLSH   0x00000080u
#define TOSTOP   0x00000100u
#define ECHOCTL  0x00000200u
#define ECHOPRT  0x00000400u
#define ECHOKE   0x00000800u
#define FLUSHO   0x00001000u
#define PENDIN   0x00002000u
#define EXTPROC  0x00004000u

#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

#define B0      0u
#define B50     50u
#define B75     75u
#define B110    110u
#define B134    134u
#define B150    150u
#define B200    200u
#define B300    300u
#define B600    600u
#define B1200   1200u
#define B1800   1800u
#define B2400   2400u
#define B4800   4800u
#define B9600   9600u
#define B19200  19200u
#define B38400  38400u
#define B57600  57600u
#define B115200 115200u
#define B230400 230400u

int tcgetattr(int fd, struct termios* termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios* termios_p);
int tcdrain(int fd);
int tcflush(int fd, int queue_selector);
int tcflow(int fd, int action);
speed_t cfgetispeed(const struct termios* termios_p);
speed_t cfgetospeed(const struct termios* termios_p);
int cfsetispeed(struct termios* termios_p, speed_t speed);
int cfsetospeed(struct termios* termios_p, speed_t speed);
int cfsetspeed(struct termios* termios_p, speed_t speed);
void cfmakeraw(struct termios* termios_p);

#endif // KIWILIB_TERMIOS_H
