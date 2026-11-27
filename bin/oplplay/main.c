// SPDX-License-Identifier: GPL-2.0-or-later
/* oplplay: play Doom music (MUS) or a MIDI file on our OPL2 synthesizer, with the
 * instruments of a GENMIDI lump, through the sound server (DECISIONS.md 21).
 *   oplplay [-l] [-v VOLUME] [-c] (-g GENMIDI | -w WAD) SONG
 *   oplplay ... -o FILE [-s SECONDS] SONG     render to a file instead (raw 16-bit stereo, 48 kHz)
 * -l loops the song. -v is 0 to 127 (default 100). -c reads commands from stdin, one per
 * line: pause, resume, volume N, stop (Doom drives its music this way).
 * Playing, it renders only about 60 ms ahead of real time, so pause and stop are heard
 * soon (plus audiod's own buffer, about 340 ms). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <math.h>
#include <time.h>
#include "opl.h"

#define AHEAD_FRAMES (OPL_RATE * 60 / 1000)

/* ---- GENMIDI: 175 instruments (128 melodic, 47 percussion for notes 35 to 81) ---- */
struct gm_op { unsigned char tremolo, attack, sustain, waveform, scale, level; };
struct gm_voice { struct gm_op mod; unsigned char feedback; struct gm_op car; unsigned char unused; short base_note; };
struct gm_instr { unsigned short flags; unsigned char fine_tuning, fixed_note; struct gm_voice voice[2]; };
#define GM_FIXED 1
#define GM_DOUBLE 4
static struct gm_instr instr[175];

static unsigned char *read_file(const char *path, long *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  long cap = 65536, n = 0, r;
  unsigned char *b = malloc((size_t)cap);
  while (b && (r = read(fd, b + n, (size_t)(cap - n))) > 0) {
    n += r;
    if (n == cap) { cap *= 2; b = realloc(b, (size_t)cap); }
  }
  close(fd);
  *len = n;
  return b;
}
static int load_genmidi(const unsigned char *d, long len) {
  if (len < 8 + 175 * 36 || memcmp(d, "#OPL_II#", 8)) return -1;
  for (int i = 0; i < 175; i++) {
    const unsigned char *p = d + 8 + i * 36;
    struct gm_instr *g = &instr[i];
    g->flags = (unsigned short)(p[0] | p[1] << 8);
    g->fine_tuning = p[2];
    g->fixed_note = p[3];
    for (int v = 0; v < 2; v++) {
      const unsigned char *q = p + 4 + v * 16;
      struct gm_voice *x = &g->voice[v];
      x->mod = (struct gm_op){q[0], q[1], q[2], q[3], q[4], q[5]};
      x->feedback = q[6];
      x->car = (struct gm_op){q[7], q[8], q[9], q[10], q[11], q[12]};
      x->base_note = (short)(q[14] | q[15] << 8);
    }
  }
  return 0;
}
static int genmidi_from_wad(const char *path) {
  long len;
  unsigned char *w = read_file(path, &len);
  if (!w || len < 12) return -1;
  int n = (int)(w[4] | w[5] << 8 | w[6] << 16 | (unsigned)w[7] << 24);
  long dir = (long)(w[8] | w[9] << 8 | w[10] << 16 | (unsigned)w[11] << 24);
  for (int i = 0; i < n && dir + i * 16 + 16 <= len; i++) {
    const unsigned char *e = w + dir + i * 16;
    if (strncmp((const char *)e + 8, "GENMIDI", 8)) continue;
    long pos = (long)(e[0] | e[1] << 8 | e[2] << 16 | (unsigned)e[3] << 24), size = (long)(e[4] | e[5] << 8 | e[6] << 16 | (unsigned)e[7] << 24);
    int r = pos + size <= len ? load_genmidi(w + pos, size) : -1;
    free(w);
    return r;
  }
  free(w);
  return -1;
}

/* ---- the song, as timed events ---- */
enum { E_OFF, E_ON, E_BEND, E_CTRL, E_PROG };
struct ev { unsigned frame, seq; unsigned char type, ch, a, b; };
static struct ev *evs;
static int nev, cap_ev;
static unsigned song_frames;
static void add(unsigned frame, int type, int ch, int a, int b) {
  if (nev == cap_ev) { cap_ev = cap_ev ? cap_ev * 2 : 1024; evs = realloc(evs, sizeof *evs * (size_t)cap_ev); }
  evs[nev] = (struct ev){frame, (unsigned)nev, (unsigned char)type, (unsigned char)ch, (unsigned char)a, (unsigned char)b};
  nev++;
}
/* MUS (Doom's own format): 140 ticks per second, channel 15 is percussion. */
static int load_mus(const unsigned char *d, long len) {
  if (len < 16 || memcmp(d, "MUS\x1a", 4)) return -1;
  long pos = d[6] | d[7] << 8;
  unsigned long tick = 0;
  int lastvol[16];
  for (int i = 0; i < 16; i++) lastvol[i] = 100;
  static const int ctrl_map[10] = {-1, 0, 1, 7, 10, 11, 91, 93, 64, 67}; /* MUS controller -> MIDI */
  while (pos < len) {
    int b = d[pos++], last = b & 0x80, type = (b >> 4) & 7, ch = b & 15;
    if (ch == 15) ch = 9; else if (ch >= 9) ch++;
    unsigned frame = (unsigned)(tick * OPL_RATE / 140);
    switch (type) {
    case 0: add(frame, E_OFF, ch, d[pos++] & 127, 0); break;
    case 1: { int n = d[pos++]; if (n & 0x80) lastvol[ch] = d[pos++] & 127; add(frame, E_ON, ch, n & 127, lastvol[ch]); break; }
    case 2: add(frame, E_BEND, ch, d[pos++], 0); break;                    /* 0 to 255, 128 = none */
    case 3: { int c = d[pos++]; if (c == 10 || c == 11) add(frame, E_CTRL, ch, 123, 0); else if (c == 14) add(frame, E_CTRL, ch, 121, 0); break; }
    case 4: { int c = d[pos++], v = d[pos++] & 127; if (c == 0) add(frame, E_PROG, ch, v, 0); else if (c < 10 && ctrl_map[c] >= 0) add(frame, E_CTRL, ch, ctrl_map[c], v); break; }
    case 6: song_frames = frame; return 0;
    default: break;
    }
    if (last) {
      unsigned long delay = 0;
      int x;
      do { if (pos >= len) break; x = d[pos++]; delay = delay << 7 | (unsigned long)(x & 127); } while (x & 0x80);
      tick += delay;
    }
  }
  song_frames = (unsigned)(tick * OPL_RATE / 140);
  return 0;
}
/* Standard MIDI file, format 0 or 1: all tracks merged, with the tempo map. */
static unsigned long vlq(const unsigned char *d, long *p, long end) {
  unsigned long v = 0;
  int x;
  do { if (*p >= end) return v; x = d[(*p)++]; v = v << 7 | (unsigned long)(x & 127); } while (x & 0x80);
  return v;
}
static int ev_cmp(const void *a, const void *b) {
  const struct ev *x = a, *y = b;
  return x->frame < y->frame ? -1 : x->frame > y->frame ? 1 : x->seq < y->seq ? -1 : x->seq > y->seq; /* stable: file order */
}
static int load_midi(const unsigned char *d, long len) {
  if (len < 14 || memcmp(d, "MThd", 4)) return -1;
  int ntracks = d[10] << 8 | d[11], division = d[12] << 8 | d[13];
  if (division & 0x8000 || !division) return -1;      /* SMPTE timing: not supported */
  /* First pass: the tempo map (tick -> microseconds per quarter note). */
  struct { unsigned long tick, usq; } tempo[256];
  int nt = 1;
  tempo[0].tick = 0; tempo[0].usq = 500000;
  for (int pass = 0; pass < 2; pass++) {
    long p = 8 + (long)(d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]);
    for (int t = 0; t < ntracks && p + 8 <= len; t++) {
      long tlen = (long)((unsigned)d[p + 4] << 24 | d[p + 5] << 16 | d[p + 6] << 8 | d[p + 7]);
      long q = p + 8, end = p + 8 + tlen < len ? p + 8 + tlen : len;
      unsigned long tick = 0;
      int status = 0;
      while (q < end) {
        tick += vlq(d, &q, end);
        if (q >= end) break;
        int b = d[q];
        if (b & 0x80) { status = b; q++; }
        if (status == 0xff) {
          int type = d[q++];
          long l = (long)vlq(d, &q, end);
          if (pass == 0 && type == 0x51 && l == 3 && nt < 256) {
            tempo[nt].tick = tick; tempo[nt].usq = (unsigned long)(d[q] << 16 | d[q + 1] << 8 | d[q + 2]); nt++;
          }
          q += l;
          status = 0;
          continue;
        }
        if (status == 0xf0 || status == 0xf7) { q += (long)vlq(d, &q, end); status = 0; continue; }
        int hi = status & 0xf0, ch = status & 15, a = d[q++], c = 0;
        if (hi != 0xc0 && hi != 0xd0) c = d[q++];
        if (pass == 0) continue;
        /* tick -> frames through the tempo map */
        double us = 0;
        unsigned long tk = 0, usq = 500000;
        for (int i = 0; i < nt && tempo[i].tick <= tick; i++) { us += (double)(tempo[i].tick - tk) * usq / division; tk = tempo[i].tick; usq = tempo[i].usq; }
        us += (double)(tick - tk) * usq / division;
        unsigned frame = (unsigned)(us * OPL_RATE / 1e6);
        if (frame > song_frames) song_frames = frame;
        if (hi == 0x90 && c) add(frame, E_ON, ch, a, c);
        else if (hi == 0x80 || hi == 0x90) add(frame, E_OFF, ch, a, 0);
        else if (hi == 0xb0) add(frame, E_CTRL, ch, a, c);
        else if (hi == 0xc0) add(frame, E_PROG, ch, a, 0);
        else if (hi == 0xe0) add(frame, E_BEND, ch, (c << 7 | a) >> 6, 0); /* to 0..255 */
      }
      p = end;
    }
    if (pass == 0) {                                  /* tempo events from all tracks, in order */
      for (int i = 1; i < nt; i++) for (int j = i; j > 1 && tempo[j].tick < tempo[j - 1].tick; j--) {
        unsigned long a = tempo[j].tick, b2 = tempo[j].usq;
        tempo[j] = tempo[j - 1]; tempo[j - 1].tick = a; tempo[j - 1].usq = b2;
      }
    }
  }
  qsort(evs, (size_t)nev, sizeof *evs, ev_cmp);
  return 0;
}

/* ---- the voices: 9 OPL channels shared by the 16 MIDI channels ---- */
static struct { int used, ch, note, key, pitch_note, age, vel, second; const struct gm_instr *in; } voice[OPL_CHANNELS];
static struct { int prog, vol, pan, bend, expr; } midi[16];
static int master = 100, age_counter;
static const int op_off[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

/* Loudness 0..127 -> 0..127, roughly as the ear hears volume. */
static int curve(int v) { return (int)(sqrt(v / 127.0) * 127 + 0.5); }
static int level(int op_level, int scale) { int l = op_level & 63; return 63 - ((63 - l) * scale) / 127; }
static void voice_levels(int vi) {
  int c = voice[vi].ch;
  const struct gm_voice *g = &voice[vi].in->voice[voice[vi].second];
  int scale = curve(voice[vi].vel) * curve(midi[c].vol) / 127 * curve(midi[c].expr) / 127 * curve(master) / 127;
  int o = op_off[vi];
  opl_write(0x40 + o + 3, (g->car.scale & 0xc0) | level(g->car.level, scale));
  opl_write(0x40 + o, (g->mod.scale & 0xc0) | ((g->feedback & 1) ? level(g->mod.level, scale) : (g->mod.level & 63)));
  int pan = midi[c].pan;
  double a = (pan > 127 ? 127 : pan) / 128.0 * M_PI / 2;    /* 64, MIDI's middle, is exactly the centre */
  opl_pan(vi, (int)(256 * cos(a) + 0.5), (int)(256 * sin(a) + 0.5));
}
static void voice_freq(int vi) {
  int c = voice[vi].ch;
  double note = voice[vi].pitch_note + (midi[c].bend - 128) / 64.0;  /* bend: +-2 semitones */
  if (voice[vi].second) note += ((voice[vi].in->fine_tuning / 2) - 64) / 32.0;
  double hz = 440 * pow(2, (note - 69) / 12);
  int block = 0;
  double fnum = hz * (1 << 20) / 49716;
  while (fnum >= 1024 && block < 7) { fnum /= 2; block++; }
  int f = (int)(fnum + 0.5);
  if (f > 1023) f = 1023;
  opl_write(0xa0 + vi, f & 0xff);
  opl_write(0xb0 + vi, (voice[vi].key ? 0x20 : 0) | block << 2 | f >> 8);
}
static void voice_off(int vi) { voice[vi].key = 0; voice_freq(vi); voice[vi].used = 0; }
static int voice_take(void) {
  int best = 0;
  for (int i = 0; i < OPL_CHANNELS; i++) if (!voice[i].used) return i;
  for (int i = 1; i < OPL_CHANNELS; i++) if (voice[i].age < voice[best].age) best = i;  /* steal the oldest */
  voice_off(best);
  return best;
}
static void voice_program(int vi) {
  const struct gm_voice *g = &voice[vi].in->voice[voice[vi].second];
  int o = op_off[vi];
  opl_write(0x20 + o, g->mod.tremolo); opl_write(0x60 + o, g->mod.attack); opl_write(0x80 + o, g->mod.sustain); opl_write(0xe0 + o, g->mod.waveform);
  opl_write(0x20 + o + 3, g->car.tremolo); opl_write(0x60 + o + 3, g->car.attack); opl_write(0x80 + o + 3, g->car.sustain); opl_write(0xe0 + o + 3, g->car.waveform);
  opl_write(0xc0 + vi, g->feedback);
  voice_levels(vi);
}
static void note_on(int c, int note, int vel) {
  const struct gm_instr *in;
  int pitch = note;
  if (c == 9) { if (note < 35 || note > 81) return; in = &instr[128 + note - 35]; }
  else in = &instr[midi[c].prog & 127];
  int voices = (in->flags & GM_DOUBLE) ? 2 : 1;
  for (int v = 0; v < voices; v++) {
    int vi = voice_take();
    voice[vi].used = 1; voice[vi].ch = c; voice[vi].note = note; voice[vi].vel = vel;
    voice[vi].in = in; voice[vi].second = v; voice[vi].age = ++age_counter;
    voice[vi].pitch_note = (in->flags & GM_FIXED) ? in->fixed_note : pitch + in->voice[v].base_note;
    voice[vi].key = 0;
    voice_freq(vi);                                   /* make sure the previous note is off */
    voice_program(vi);
    voice[vi].key = 1;
    voice_freq(vi);
  }
}
static void note_off(int c, int note) {
  for (int i = 0; i < OPL_CHANNELS; i++) if (voice[i].used && voice[i].ch == c && voice[i].note == note) voice_off(i);
}
static void reset_channels(void) {
  for (int c = 0; c < 16; c++) { midi[c].prog = 0; midi[c].vol = 100; midi[c].pan = 64; midi[c].bend = 128; midi[c].expr = 127; }
}
static void event(const struct ev *e) {
  int c = e->ch;
  switch (e->type) {
  case E_ON: note_on(c, e->a, e->b); break;
  case E_OFF: note_off(c, e->a); break;
  case E_PROG: midi[c].prog = e->a; break;
  case E_BEND: midi[c].bend = e->a; for (int i = 0; i < OPL_CHANNELS; i++) if (voice[i].used && voice[i].ch == c) voice_freq(i); break;
  case E_CTRL:
    if (e->a == 7 || e->a == 10 || e->a == 11) {
      if (e->a == 7) midi[c].vol = e->b; else if (e->a == 10) midi[c].pan = e->b; else midi[c].expr = e->b;
      for (int i = 0; i < OPL_CHANNELS; i++) if (voice[i].used && voice[i].ch == c) voice_levels(i);
    } else if (e->a == 123 || e->a == 120) { for (int i = 0; i < OPL_CHANNELS; i++) if (voice[i].used && voice[i].ch == c) voice_off(i); }
    else if (e->a == 121) { midi[c].vol = 100; midi[c].pan = 64; midi[c].bend = 128; midi[c].expr = 127; }
    break;
  }
}
static void all_off(void) { for (int i = 0; i < OPL_CHANNELS; i++) if (voice[i].used) voice_off(i); }

/* ---- output ---- */
struct audiod_hello { char magic[4]; unsigned rate; unsigned short channels, bits, volume, pad; unsigned total; };
static int connect_audiod(void) {
  int fd = (int)__syscall3(SYS_socket, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0);
  struct { unsigned short family; char path[108]; } a = {1, ""};
  { const char *x = getenv("XDG_RUNTIME_DIR"); snprintf(a.path, sizeof a.path, "%s/audiod.sock", x && *x ? x : "/run"); } /* audiod's socket, in the session (bin/audiod.c) */
  long e = __syscall3(SYS_connect, fd, &a, sizeof a);
  if (e < 0) { fprintf(stderr, "oplplay: cannot reach the sound server (audiod): %s\n", strerror((int)-e)); return -1; }
  struct audiod_hello h = {{'U', 'C', 'A', 'U'}, OPL_RATE, 2, 16, 100, 0, 0};
  write(fd, &h, sizeof h);
  return fd;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int write_all(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n) { long w = write(fd, c, n); if (w <= 0) return -1; c += w; n -= (size_t)w; }
  return 0;
}
/* Commands on stdin, for Doom. Returns 1 to stop. */
static int paused;
static int commands(void) {
  static char line[64];
  static int len;
  char b[64];
  long n = read(0, b, sizeof b);
  if (n <= 0) return n == 0;                        /* the controller went away: stop */
  for (long i = 0; i < n; i++) {
    if (b[i] != '\n') { if (len < (int)sizeof line - 1) line[len++] = b[i]; continue; }
    line[len] = 0;
    len = 0;
    if (!strcmp(line, "pause")) paused = 1;
    else if (!strcmp(line, "resume")) paused = 0;
    else if (!strcmp(line, "stop")) return 1;
    else if (!strncmp(line, "volume ", 7)) {
      master = atoi(line + 7) < 0 ? 0 : atoi(line + 7) > 127 ? 127 : atoi(line + 7);
      for (int v = 0; v < OPL_CHANNELS; v++) if (voice[v].used) voice_levels(v);
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  int loop = 0, control = 0, opt;
  const char *gm = NULL, *wad = NULL, *outfile = NULL;
  double secs = 0;
  while ((opt = getopt(argc, argv, "lcv:g:w:o:s:")) != -1) {
    switch (opt) {
    case 'l': loop = 1; break;
    case 'c': control = 1; break;
    case 'v': master = atoi(optarg) < 0 ? 0 : atoi(optarg) > 127 ? 127 : atoi(optarg); break;
    case 'g': gm = optarg; break;
    case 'w': wad = optarg; break;
    case 'o': outfile = optarg; break;
    case 's': secs = strtod(optarg, NULL); break;
    default: goto usage;
    }
  }
  if (optind != argc - 1 || (!gm && !wad)) {
  usage:
    fprintf(stderr, "usage: oplplay [-l] [-v VOLUME] [-c] (-g GENMIDI | -w WAD) [-o FILE [-s SECONDS]] SONG\n");
    return 2;
  }
  long len;
  unsigned char *g = gm ? read_file(gm, &len) : NULL;
  if (gm ? (!g || load_genmidi(g, len)) : genmidi_from_wad(wad)) { fprintf(stderr, "oplplay: no GENMIDI instruments in %s\n", gm ? gm : wad); return 1; }
  unsigned char *song = read_file(argv[optind], &len);
  if (!song) { fprintf(stderr, "oplplay: %s: %s\n", argv[optind], strerror(errno)); return 1; }
  if (load_mus(song, len) && load_midi(song, len)) { fprintf(stderr, "oplplay: %s: not a MUS or MIDI file\n", argv[optind]); return 1; }
  opl_reset();
  opl_write(0x01, 0x20);                            /* waveform select on */
  reset_channels();

  int out = -1;
  if (outfile) out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  else out = connect_audiod();
  if (out < 0) return 1;
  if (control) fcntl(0, F_SETFL, O_NONBLOCK);
  static short buf[2 * 960];
  unsigned pos = 0, base = 0, produced = 0, limit = outfile && secs > 0 ? (unsigned)(secs * OPL_RATE) : 0;
  int next = 0;
  double t0 = now_ms();
  for (;;) {
    if (!outfile) {                                   /* stay about 60 ms ahead of real time */
      double ahead = produced - (now_ms() - t0) * OPL_RATE / 1000;
      if (paused || ahead > AHEAD_FRAMES) {
        struct pollfd p = {0, POLLIN, 0};
        int wait = paused ? -1 : (int)((ahead - AHEAD_FRAMES / 2) * 1000 / OPL_RATE) + 1;
        if (poll(&p, control ? 1 : 0, wait) > 0 && commands()) break;
        if (paused) { t0 = now_ms() - produced * 1000.0 / OPL_RATE; all_off(); }
        continue;
      }
      if (control) { struct pollfd p = {0, POLLIN, 0}; if (poll(&p, 1, 0) > 0 && commands()) break; }
    }
    while (next < nev && base + evs[next].frame <= pos) event(&evs[next++]);
    if (next >= nev && pos >= base + song_frames && !loop) {
      /* The song is over: let the last notes ring out (or fill the time -s asked for). */
      if (limit ? produced >= limit : !opl_busy() || pos >= base + song_frames + 3 * OPL_RATE) break;
    } else if (next >= nev && pos >= base + song_frames) {
      all_off();
      reset_channels();
      base = pos; next = 0;
      if (!nev) break;
      continue;
    }
    unsigned n = 960;
    if (next < nev && base + evs[next].frame - pos < n) n = base + evs[next].frame - pos;
    if (limit && produced + n > limit) n = limit - produced;
    if (n) {
      opl_render(buf, (int)n);
      if (write_all(out, buf, n * 4)) break;
      pos += n; produced += n;
    }
    if (limit && produced >= limit) break;
  }
  close(out);
  free(song);
  free(g);
  free(evs);
  return 0;
}
