// SPDX-License-Identifier: GPL-2.0-or-later
/* Doom's sound effects on UC.OS: the game path of DECISIONS.md 20. Doom writes straight
 * to its own /dev/dsp stream, with a small buffer (6 x 2 KiB, about 64 ms at 48 kHz),
 * and never waits: once per frame (I_UpdateSound) it asks how much room there is,
 * mixes exactly that much and writes it without blocking. If the device is full it
 * simply skips; a frame is never delayed by sound.
 * Mixing: up to 8 channels of the WAD's 8-bit sounds (DMX format, usually 11025 Hz),
 * resampled straight to the device rate with linear interpolation in the same pass,
 * with Doom's volume and stereo separation, in integers. No sample is converted twice.
 * Music: see the end of the file. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"

/* Variables the shared sound code expects when FEATURE_SOUND is on. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
char *timidity_cfg_path = "";
void I_InitTimidityConfig(void) {}

#define CHANNELS 16
#define MAX_FRAMES 4096
static struct {
  const byte *data;   /* 8-bit unsigned samples */
  unsigned len;       /* in samples */
  unsigned pos, step; /* 16.16 */
  int vl, vr;         /* 0 to 254 */
  boolean on;
} ch[CHANNELS];
static int dsp = -1, rate = 48000;
static int acc[2 * MAX_FRAMES];
static short out[2 * MAX_FRAMES];
static boolean prefix;

static void volumes(int c, int vol, int sep) {
  /* As chocolate-doom: sep 0 is full left, 254 full right, 128 the middle. */
  ch[c].vl = ((254 - sep) * vol) / 127;
  ch[c].vr = (sep * vol) / 127;
}

static boolean snd_init(boolean use_sfx_prefix) {
  prefix = use_sfx_prefix;
  dsp = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
  if (dsp < 0) return false;
  int frag = 6 << 16 | 11;            /* 6 x 2 KiB: about 64 ms of sound queued at most */
  ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
  ioctl(dsp, SNDCTL_DSP_SPEED, &rate);
  memset(ch, 0, sizeof ch);
  return true;
}
static void snd_shutdown(void) { if (dsp >= 0) close(dsp); dsp = -1; }

static int snd_lump(sfxinfo_t *sfx) {
  char name[9];
  if (sfx->link) sfx = sfx->link;
  snprintf(name, sizeof name, prefix ? "ds%s" : "%s", DEH_String(sfx->name));
  return W_GetNumForName(name);
}

static int snd_start(sfxinfo_t *sfx, int c, int vol, int sep) {
  if (c < 0 || c >= CHANNELS || dsp < 0) return -1;
  int lump = sfx->lumpnum;
  unsigned size = (unsigned)W_LumpLength((unsigned)lump);
  const byte *d = W_CacheLumpNum(lump, PU_STATIC);
  /* DMX: format (3), rate, sample count, 16 padding bytes, samples, 16 padding bytes. */
  if (size < 8 || d[0] != 3 || d[1] != 0) return -1;
  unsigned srate = (unsigned)(d[2] | d[3] << 8), n = (unsigned)(d[4] | d[5] << 8 | d[6] << 16 | (unsigned)d[7] << 24);
  if (n > size - 8) n = size - 8;
  if (n <= 32 || !srate) return -1;
  ch[c].data = d + 8 + 16;
  ch[c].len = n - 32;
  ch[c].pos = 0;
  ch[c].step = (unsigned)(((unsigned long long)srate << 16) / (unsigned)rate);
  volumes(c, vol, sep);
  ch[c].on = true;
  return c;
}
static void snd_stop(int c) { if (c >= 0 && c < CHANNELS) ch[c].on = false; }
static boolean snd_playing(int c) { return c >= 0 && c < CHANNELS && ch[c].on; }
static void snd_params(int c, int vol, int sep) { if (c >= 0 && c < CHANNELS) volumes(c, vol, sep); }
static void snd_cache(sfxinfo_t *sounds, int num) { (void)sounds; (void)num; } /* loaded on first use */

/* Once per frame: fill exactly the room the device has, without waiting. */
static void snd_update(void) {
  if (dsp < 0) return;
  struct audio_buf_info sp;
  if (ioctl(dsp, SNDCTL_DSP_GETOSPACE, &sp) < 0) return;
  int n = sp.bytes / 4;
  if (n > MAX_FRAMES) n = MAX_FRAMES;
  if (n < 256) return;                /* less than ~5 ms of room: next frame */
  memset(acc, 0, sizeof acc[0] * 2 * (size_t)n);
  for (int c = 0; c < CHANNELS; c++) {
    if (!ch[c].on) continue;
    const byte *d = ch[c].data;
    unsigned pos = ch[c].pos, step = ch[c].step, last = ch[c].len - 1;
    int vl = ch[c].vl, vr = ch[c].vr, *a = acc;
    for (int i = 0; i < n; i++, a += 2) {
      unsigned k = pos >> 16;
      if (k >= last) { ch[c].on = false; break; }
      int f = (int)(pos & 0xffff) >> 1;
      int s0 = (d[k] - 128) << 8, s1 = (d[k + 1] - 128) << 8;
      int s = s0 + (((s1 - s0) * f) >> 15);
      a[0] += (s * vl) >> 8;          /* vl up to 254: about full scale */
      a[1] += (s * vr) >> 8;
      pos += step;
    }
    ch[c].pos = pos;
  }
  for (int i = 0; i < 2 * n; i++) out[i] = (short)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
  write(dsp, out, (size_t)n * 4);
}

static snddevice_t devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};
sound_module_t DG_sound_module = {
  devices, sizeof devices / sizeof devices[0],
  snd_init, snd_shutdown, snd_lump, snd_update, snd_params, snd_start, snd_stop, snd_playing, snd_cache,
};

/* Music (DECISIONS.md 21): Doom does not synthesize it. PlaySong writes the song lump
 * and the WAD's GENMIDI instruments to /tmp/doom and starts /bin/oplplay, our OPL2
 * synthesizer, which plays through the sound server (audiod). Doom controls it through
 * a pipe on its stdin: "pause", "resume", "volume N", "stop". */
struct song { const void *data; int len; };
static pid_t player = -1;
static int control = -1;           /* write end of the player's stdin */
static int music_volume = 100;     /* 0 to 127 */
static int have_genmidi;

static void tell(const char *cmd) {
  if (control >= 0) write(control, cmd, strlen(cmd));
}
static void mus_stop(void) {
  if (player > 0) {
    tell("stop\n");
    close(control);
    control = -1;
    kill(player, SIGTERM);            /* in case it is not reading: it must not linger */
    int st;
    waitpid(player, &st, 0);
  }
  player = -1;
}
static int save(const char *path, const void *data, int len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  int ok = write(fd, data, (size_t)len) == len;
  close(fd);
  return ok ? 0 : -1;
}
static boolean mus_init(void) {
  struct stat st;
  if (stat("/bin/oplplay", &st) < 0) return false;
  int lump = W_CheckNumForName("GENMIDI");
  if (lump < 0) return false;
  mkdir("/tmp/doom", 0755);
  have_genmidi = save("/tmp/doom/genmidi.lmp", W_CacheLumpNum(lump, PU_STATIC), W_LumpLength((unsigned)lump)) == 0;
  return have_genmidi;
}
static void mus_shutdown(void) { mus_stop(); }
static void mus_volume(int v) {
  char cmd[32];
  music_volume = v > 127 ? 127 : v < 0 ? 0 : v;   /* Doom passes 0 to 127: its slider (0 to 15) times 8 */
  snprintf(cmd, sizeof cmd, "volume %d\n", music_volume);
  tell(cmd);
}
static void mus_pause(void) { tell("pause\n"); }
static void mus_resume(void) { tell("resume\n"); }
static void *mus_register(void *data, int len) {
  static struct song s;
  s.data = data;
  s.len = len;
  return &s;
}
static void mus_unregister(void *h) { (void)h; }
static void mus_play(void *h, boolean loop) {
  struct song *s = h;
  mus_stop();
  if (!have_genmidi || !s || !s->data || save("/tmp/doom/song.lmp", s->data, s->len) < 0) return;
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0) return;
  char vol[8];
  snprintf(vol, sizeof vol, "%d", music_volume);
  char *argv[] = {"oplplay", "-c", "-v", vol, "-g", "/tmp/doom/genmidi.lmp", loop ? "-l" : "-c", "/tmp/doom/song.lmp", NULL};
  int fdmap[3] = {p[0], -1, -1};
  player = spawn("/bin/oplplay", argv, NULL, fdmap);
  close(p[0]);
  if (player < 0) { close(p[1]); return; }
  control = p[1];
}
static boolean mus_playing(void) {
  if (player <= 0) return false;
  int st;
  if (waitpid(player, &st, WNOHANG) == player) { player = -1; close(control); control = -1; return false; }
  return true;
}
static void mus_poll(void) { mus_playing(); } /* reaps a player that finished a song played once */
music_module_t DG_music_module = {
  devices, sizeof devices / sizeof devices[0],
  mus_init, mus_shutdown, mus_volume, mus_pause, mus_resume, mus_register, mus_unregister, mus_play, mus_stop, mus_playing, mus_poll,
};
