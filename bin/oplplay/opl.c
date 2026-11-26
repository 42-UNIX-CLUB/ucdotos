// SPDX-License-Identifier: GPL-2.0-or-later
/* An OPL2 FM synthesizer, written for UC.OS from the chip's documented behaviour
 * (DECISIONS.md 21). Like the chip it works in the log domain: an operator's output is
 * exp(logsin(phase) + attenuation), so volume, envelope and key scaling are additions,
 * and two 256-entry tables do the rest. Close to the chip, not cycle-exact.
 *
 * Per channel: two operators (modulator and carrier), FM or additive connection,
 * modulator feedback. Per operator: frequency multiple, four waveforms, total level,
 * key scale level and rate, an envelope (attack, decay, sustain, release; sustained or
 * percussive), tremolo and vibrato. Not built: rhythm mode, the timers, the CSM mode.
 *
 * Cost: a channel whose carrier has finished its release (and whose modulator is silent
 * in additive mode) is skipped, so silence is free. Rendered at 48 kHz directly: the
 * phase and envelope steps are scaled from the chip's 49716 Hz. */
#include <math.h>
#include <string.h>
#include "opl.h"

enum { ATTACK, DECAY, SUSTAIN, RELEASE, OFF };
struct op {
  unsigned phase, inc;            /* 32-bit phase: the top 10 bits index the wave */
  int env, state;                 /* env: attenuation 0 (loud) to 511, 0.1875 dB per step */
  unsigned env_acc;               /* 16.16 fraction of envelope steps */
  int ar, dr, sl, rr, tl, ksl_att, kso;
  int am, vib, egt, ksr, mult2, wave, kslbits;
  int out, prev;                  /* last two outputs, for feedback */
};
struct chan {
  struct op op[2];                /* modulator, carrier */
  int fnum, block, key, fb, cnt;  /* cnt 0: FM, 1: additive */
  int pl, pr;
};
static struct chan chans[OPL_CHANNELS];
static int wse, dam, dvb;
static unsigned short logsin[256], exptab[256];
static int tables_ready;
static unsigned lfo_count;
static int trem, vibmul;          /* current tremolo attenuation; vibrato as 16.16 multiplier delta */

/* Register offsets 0x00 to 0x15 -> operator (channel * 2 + carrier), -1 for the gaps. */
static const signed char slot_op[22] = {0, 2, 4, 1, 3, 5, -1, -1, 6, 8, 10, 7, 9, 11, -1, -1, 12, 14, 16, 13, 15, 17};
static const int mult2_tab[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const int kslrom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const int kslshift[4] = {31, 1, 2, 0};

static void tables(void) {
  for (int i = 0; i < 256; i++) {
    logsin[i] = (unsigned short)(-log2(sin((i + 0.5) * M_PI / 512)) * 256 + 0.5);
    exptab[i] = (unsigned short)(4096 * pow(2, -i / 256.0) + 0.5);
  }
  tables_ready = 1;
}
/* The operator at a register offset, and its channel. */
static struct op *op_at(int offset, struct chan **c) {
  if (offset < 0 || offset >= 22 || slot_op[offset] < 0) return 0;
  int k = slot_op[offset];
  *c = &chans[k >> 1];
  return &chans[k >> 1].op[k & 1];
}
/* 16.16 envelope steps per sample for a rate 0 to 63 (the chip's scheme, rescaled to 48 kHz). */
static unsigned rate_inc(int r) {
  if (r < 4) return 0;
  if (r > 63) r = 63;
  int shift = 13 - (r >> 2);
  double steps = (4 + (r & 3)) / 4.0 / (shift >= 0 ? (double)(1 << shift) : 1.0 / (1 << -shift));
  return (unsigned)(steps * 65536 * 49716.0 / OPL_RATE);
}
static unsigned rates[64];
static void update_freq(struct chan *c) {
  int kso = (c->block << 1) | ((c->fnum >> 9) & 1);
  int ksl = (kslrom[c->fnum >> 6] << 2) - ((8 - c->block) << 5);
  if (ksl < 0) ksl = 0;
  for (int i = 0; i < 2; i++) {
    struct op *o = &c->op[i];
    o->inc = (unsigned)(((unsigned long long)((unsigned)c->fnum << c->block) * (unsigned)o->mult2 * 49716ull * 2048ull) / OPL_RATE);
    o->kso = o->ksr ? kso : kso >> 2;
    o->ksl_att = o->kslbits ? ksl >> kslshift[o->kslbits] : 0;
  }
}
static void key_on(struct op *o) { o->state = ATTACK; o->phase = 0; o->env_acc = 0; }
static void key_off(struct op *o) { if (o->state != OFF) o->state = RELEASE; }

void opl_reset(void) {
  if (!tables_ready) tables();
  for (int r = 0; r < 64; r++) rates[r] = rate_inc(r);
  memset(chans, 0, sizeof chans);
  for (int c = 0; c < OPL_CHANNELS; c++) {
    chans[c].pl = chans[c].pr = 181;             /* the middle: 256 / sqrt(2) */
    for (int i = 0; i < 2; i++) { chans[c].op[i].env = 511; chans[c].op[i].state = OFF; chans[c].op[i].mult2 = 1; }
  }
  wse = dam = dvb = 0;
  lfo_count = 0; trem = 0; vibmul = 0;
}
int opl_busy(void) {
  for (int c = 0; c < OPL_CHANNELS; c++)
    if (chans[c].op[1].state != OFF || (chans[c].cnt && chans[c].op[0].state != OFF)) return 1;
  return 0;
}
void opl_pan(int c, int l, int r) { if (c >= 0 && c < OPL_CHANNELS) { chans[c].pl = l; chans[c].pr = r; } }

void opl_write(int reg, int v) {
  reg &= 0xff; v &= 0xff;
  if (reg == 0x01) { wse = v & 0x20; return; }
  if (reg == 0xbd) { dam = v >> 7; dvb = (v >> 6) & 1; return; }
  int hi = reg & 0xe0, lo = reg & 0x1f;
  if (hi >= 0x20 && hi <= 0x80 && hi != 0xa0) {
    struct chan *c;
    struct op *o = op_at(lo, &c);
    if (!o) return;
    switch (hi) {
    case 0x20: o->am = v >> 7; o->vib = (v >> 6) & 1; o->egt = (v >> 5) & 1; o->ksr = (v >> 4) & 1; o->mult2 = mult2_tab[v & 15]; break;
    case 0x40: o->kslbits = v >> 6; o->tl = (v & 63) << 2; break;
    case 0x60: o->ar = v >> 4; o->dr = v & 15; break;
    case 0x80: o->sl = (v >> 4) == 15 ? 31 << 4 : (v >> 4) << 4; o->rr = v & 15; break;
    }
    update_freq(c);
    return;
  }
  if (hi == 0xe0) { struct chan *c; struct op *o = op_at(lo, &c); if (o) o->wave = wse ? (v & 3) : 0; return; }
  if (reg >= 0xa0 && reg <= 0xa8) { struct chan *c = &chans[reg - 0xa0]; c->fnum = (c->fnum & 0x300) | v; update_freq(c); return; }
  if (reg >= 0xb0 && reg <= 0xb8) {
    struct chan *c = &chans[reg - 0xb0];
    int key = (v >> 5) & 1;
    c->fnum = (c->fnum & 0xff) | ((v & 3) << 8);
    c->block = (v >> 2) & 7;
    update_freq(c);
    if (key && !c->key) { key_on(&c->op[0]); key_on(&c->op[1]); }
    if (!key && c->key) { key_off(&c->op[0]); key_off(&c->op[1]); }
    c->key = key;
    return;
  }
  if (reg >= 0xc0 && reg <= 0xc8) { struct chan *c = &chans[reg - 0xc0]; c->fb = (v >> 1) & 7; c->cnt = v & 1; }
}

static inline void envelope(struct op *o) {
  int r;
  switch (o->state) {
  case ATTACK: r = o->ar ? o->ar * 4 + o->kso : 0; break;
  case DECAY: r = o->dr ? o->dr * 4 + o->kso : 0; break;
  case SUSTAIN: return;
  case RELEASE: r = o->rr ? o->rr * 4 + o->kso : 0; break;
  default: return;
  }
  if (r > 63) r = 63;
  if (o->state == ATTACK && r >= 60) { o->env = 0; o->state = DECAY; return; }
  o->env_acc += rates[r];
  int steps = (int)(o->env_acc >> 16);
  if (!steps) return;
  o->env_acc &= 0xffff;
  if (o->state == ATTACK) {
    while (steps-- && o->env > 0) o->env -= (o->env >> 3) + 1;
    if (o->env <= 0) { o->env = 0; o->state = DECAY; }
  } else if (o->state == DECAY) {
    o->env += steps;
    if (o->env >= o->sl) { o->env = o->sl; o->state = o->egt ? SUSTAIN : RELEASE; }
  } else {
    o->env += steps;
    if (o->env >= 511) { o->env = 511; o->state = OFF; }
  }
}
static inline int operator(struct op *o, int mod) {
  int att = o->env + o->tl + o->ksl_att + (o->am ? trem : 0);
  unsigned inc = o->vib ? o->inc + (unsigned)(((long long)o->inc * vibmul) >> 16) : o->inc;
  int idx = (int)((o->phase >> 22) + (unsigned)mod) & 1023;
  o->phase += inc;
  if (att >= 511) return 0;
  int i = idx & 511, neg = idx & 512;
  switch (o->wave) {
  case 1: if (neg) return 0; break;                 /* half sine */
  case 2: neg = 0; break;                           /* absolute sine */
  case 3: if (idx & 256) return 0; neg = 0; break;  /* quarter pulses */
  }
  if (i & 256) i = 511 - i;
  int level = logsin[i] + (att << 3);
  if (level >= 12 << 8) return 0;
  int amp = exptab[level & 255] >> (level >> 8);
  return neg ? -amp : amp;
}

void opl_render(short *out, int frames) {
  for (int f = 0; f < frames; f++) {
    if ((lfo_count++ & 63) == 0) {                  /* the chip's LFOs: tremolo 3.7 Hz, vibrato 6.1 Hz */
      double t = lfo_count / (double)OPL_RATE;
      double tri = 1 - fabs(fmod(t * 3.7, 1.0) * 2 - 1);
      trem = (int)(tri * (dam ? 26 : 5));           /* 4.8 dB or 1 dB, in 0.1875 dB steps */
      vibmul = (int)(sin(t * 6.1 * 2 * M_PI) * (dvb ? 0.0081 : 0.0041) * 65536); /* 14 or 7 cents */
    }
    int l = 0, r = 0;
    for (int ci = 0; ci < OPL_CHANNELS; ci++) {
      struct chan *c = &chans[ci];
      struct op *m = &c->op[0], *k = &c->op[1];
      if (k->state == OFF && (!c->cnt || m->state == OFF)) continue; /* silent: free */
      envelope(m);
      envelope(k);
      int fbin = c->fb ? (m->prev + m->out) >> (9 - c->fb) : 0;
      int mo = operator(m, fbin);
      m->prev = m->out;
      m->out = mo;
      int s = c->cnt ? mo + operator(k, 0) : operator(k, mo);
      l += s * c->pl;
      r += s * c->pr;
    }
    l >>= 6; r >>= 6;                                /* the chip's 4096 per voice, x4, over pan's 256: level with Doom's effects */
    out[2 * f] = (short)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
    out[2 * f + 1] = (short)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
  }
}
