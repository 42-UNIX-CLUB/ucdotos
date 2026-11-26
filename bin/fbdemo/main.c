// SPDX-License-Identifier: GPL-2.0-or-later
/* fbdemo: the smallest program that owns the screen. It draws straight to
 * /dev/fb0 with libgfx and shows the raw input events it reads. It exists to
 * test the path kernel -> framebuffer -> host -> canvas without a window manager. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include "gfx.h"
#include "ttf.h"

struct input_event { long long sec, usec; unsigned short type, code; int value; };

static unsigned char *read_file(const char *path, unsigned *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  long size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  unsigned char *p = malloc(size);
  if (read(fd, p, size) != size) { close(fd); free(p); return 0; }
  close(fd);
  *len = size;
  return p;
}

int main(void) {
  int fb = open("/dev/fb0", O_RDWR), ev = open("/dev/input/event0", O_RDONLY);
  unsigned info[40] = {0};
  if (fb < 0 || ev < 0 || __syscall3(SYS_ioctl, fb, 0x4600, (long)info) < 0) { fprintf(stderr, "fbdemo: no framebuffer\n"); return 1; }
  int w = info[0], h = info[1];
  unsigned *px = malloc(w * h * 4);
  gfx_surface s;
  gfx_init(&s, px, w, h, w);
  unsigned flen = 0;
  unsigned char *fdata = read_file("/usr/share/fonts/FiraCode-Regular.ttf", &flen);
  ttf_font font;
  ttf_cache *big = 0, *small = 0;
  if (fdata && ttf_open(&font, fdata, flen) == 0) { big = ttf_cache_new(&font, 48, 256); small = ttf_cache_new(&font, 18, 512); }

  char line[6][96] = {{0}};
  int mx = w / 2, my = h / 2, nline = 0, frames = 0;
  for (;;) {
    gfx_fill_rect(&s, 0, 0, w, h, GFX_HEX(0x282828));
    gfx_round_rect_fill(&s, 40, 40, w - 80, h - 80, 16, GFX_HEX(0x32302f));
    gfx_round_rect_stroke(&s, 40, 40, w - 80, h - 80, 16, 2, GFX_HEX(0xfe8019));
    gfx_circle_fill(&s, 80, 80, 9, GFX_HEX(0xfb4934));
    gfx_circle_fill(&s, 108, 80, 9, GFX_HEX(0xfabd2f));
    gfx_circle_fill(&s, 136, 80, 9, GFX_HEX(0xb8bb26));
    if (big) {
      gfx_text(&s, big, 72, 170, "UC.OS", -1, GFX_HEX(0xfe8019));
      gfx_text(&s, small, 72, 210, "kernel, libc, rasterizer and this program are C compiled to WebAssembly", -1, GFX_HEX(0xebdbb2));
      char buf[96];
      snprintf(buf, sizeof buf, "framebuffer %dx%d   frame %d   pointer %d,%d", w, h, ++frames, mx, my);
      gfx_text(&s, small, 72, 240, buf, -1, GFX_HEX(0x8ec07c));
      for (int i = 0; i < 6; i++) gfx_text(&s, small, 72, 290 + i * 26, line[i], -1, GFX_HEX(0xa89984));
    }
    gfx_line(&s, mx - 12, my, mx + 12, my, GFX_HEX(0xfabd2f));
    gfx_line(&s, mx, my - 12, mx, my + 12, GFX_HEX(0xfabd2f));
    __syscall4(SYS_pwrite64, fb, (long)px, w * h * 4, 0);

    struct pollfd pf = {ev, POLLIN, 0};
    poll(&pf, 1, -1);
    struct input_event e[32];
    long n = read(ev, e, sizeof e);
    for (long i = 0; i < n / (long)sizeof e[0]; i++) {
      if (e[i].type == 3) { if (e[i].code == 0) mx = e[i].value; else my = e[i].value; continue; }
      if (e[i].type == 0) continue;
      if (e[i].type == 4 && e[i].code == 0x11) { /* screen resized */
        w = (unsigned)e[i].value >> 16; h = e[i].value & 0xffff;
        px = realloc(px, w * h * 4);
        gfx_init(&s, px, w, h, w);
      }
      snprintf(line[nline % 6], sizeof line[0], "event type %d code %d value %d", e[i].type, e[i].code, e[i].value);
      nline++;
      if (e[i].type == 1 && e[i].code == 1 && e[i].value == 1) return 0; /* Escape quits */
    }
  }
}
