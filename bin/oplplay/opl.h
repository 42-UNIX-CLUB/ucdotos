// SPDX-License-Identifier: GPL-2.0-or-later
/* Our own OPL2 FM synthesizer (the YM3812 in AdLib and Sound Blaster cards). */
#ifndef OPL_H
#define OPL_H
#define OPL_RATE 48000     /* rendered straight at the device rate; the chip ran at 49716 Hz */
#define OPL_CHANNELS 9
void opl_reset(void);
void opl_write(int reg, int val);                 /* a register write, as on the chip */
void opl_pan(int channel, int left, int right);   /* 0 to 256 each: stereo placement (the chip is mono) */
void opl_render(short *stereo, int frames);       /* add nothing: overwrite with frames of 16-bit stereo */
int opl_busy(void);                               /* a voice is still sounding (or releasing) */
#endif
