// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_SOUNDCARD_H
#define _SYS_SOUNDCARD_H
#include <sys/syscall.h>
/* OSS (/dev/dsp): the ioctls, formats and structs are in abi/abi.h. UC.OS plays one
 * format, 16-bit little-endian stereo, at the rate SNDCTL_DSP_SPEED answers with. */
#endif
