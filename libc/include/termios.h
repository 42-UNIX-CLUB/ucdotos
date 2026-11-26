// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _TERMIOS_H
#define _TERMIOS_H
#include <stddef_extra.h>
#include <sys/ioctl.h>
/* struct termios, NCCS, V* indices and the common flags are in abi.h (Linux values).
 * The kernel tty implements ICANON, ECHO*, ISIG, ICRNL, OPOST/ONLCR, VMIN/VTIME;
 * the other flags are accepted and stored. */
typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16
/* c_iflag */
#define IGNBRK 0x1
#define BRKINT 0x2
#define IGNPAR 0x4
#define PARMRK 0x8
#define INPCK 0x10
#define ISTRIP 0x20
#define INLCR 0x40
#define IGNCR 0x80
#define IUCLC 0x200
#define IXON 0x400
#define IXANY 0x800
#define IXOFF 0x1000
#define IMAXBEL 0x2000
#define IUTF8 0x4000
/* c_oflag */
#define OLCUC 0x2
#define OCRNL 0x8
#define ONOCR 0x10
#define ONLRET 0x20
/* c_cflag */
#define CSIZE 0x30
#define CS5 0x0
#define CS6 0x10
#define CS7 0x20
#define CS8 0x30
#define CSTOPB 0x40
#define CREAD 0x80
#define PARENB 0x100
#define PARODD 0x200
#define HUPCL 0x400
#define CLOCAL 0x800
/* c_lflag */
#define NOFLSH 0x80
#define TOSTOP 0x100
#define ECHOCTL 0x200
#define ECHOPRT 0x400
#define ECHOKE 0x800
/* tcsetattr actions, tcflush queues */
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2
#define TCOOFF 0
#define TCOON 1
#define TCIOFF 2
#define TCION 3
/* speeds: a pty has no baud rate; these exist so programs compile */
#define B0 0
#define B50 1
#define B75 2
#define B110 3
#define B134 4
#define B150 5
#define B200 6
#define B300 7
#define B600 8
#define B1200 9
#define B1800 10
#define B2400 11
#define B4800 12
#define B9600 13
#define B19200 14
#define B38400 15
#define B57600 0x1001
#define B115200 0x1002
int tcgetattr(int, struct termios *);
int tcsetattr(int, int, const struct termios *);
void cfmakeraw(struct termios *);
speed_t cfgetispeed(const struct termios *);
speed_t cfgetospeed(const struct termios *);
int cfsetispeed(struct termios *, speed_t);
int cfsetospeed(struct termios *, speed_t);
int cfsetspeed(struct termios *, speed_t);
int tcflush(int, int);
int tcdrain(int);
int tcflow(int, int);
int tcsendbreak(int, int);
pid_t tcgetpgrp(int);
int tcsetpgrp(int, pid_t);
pid_t tcgetsid(int);
#endif
