// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gfx.h - small 2D drawing library for 32-bit RGBA surfaces.
 *
 * Pixel format: one unsigned int per pixel whose bytes in little-endian
 * memory are R, G, B, A (HTML canvas ImageData order), i.e. the integer
 * value is 0xAABBGGRR.
 *
 * Every function respects the surface clip rectangle and never writes
 * outside the surface. Blending is gamma-naive source-over with exact
 * rounding: (a*c + (255-a)*d + 127) / 255 per channel.
 */
#ifndef UCOS_GFX_H
#define UCOS_GFX_H

#include "ttf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_RGBA(r, g, b, a) \
    ((unsigned int)(((unsigned)(r) & 255u) | (((unsigned)(g) & 255u) << 8) | \
                    (((unsigned)(b) & 255u) << 16) | (((unsigned)(a) & 255u) << 24)))
#define GFX_RGB(r, g, b) GFX_RGBA(r, g, b, 255)
/* 0xRRGGBB hex value (as written in CSS) to an opaque pixel. */
#define GFX_HEX(rgb) GFX_RGB(((rgb) >> 16) & 255u, ((rgb) >> 8) & 255u, (rgb) & 255u)
/* Same with an explicit alpha 0..255. */
#define GFX_HEXA(rgb, a) GFX_RGBA(((rgb) >> 16) & 255u, ((rgb) >> 8) & 255u, (rgb) & 255u, (a))

#define GFX_R(c) ((unsigned)(c) & 255u)
#define GFX_G(c) (((unsigned)(c) >> 8) & 255u)
#define GFX_B(c) (((unsigned)(c) >> 16) & 255u)
#define GFX_A(c) (((unsigned)(c) >> 24) & 255u)

static inline unsigned int gfx_hex(unsigned int rgb) { return GFX_HEX(rgb); }

typedef struct {
    unsigned int *px;
    int w, h, stride;                       /* stride in pixels */
    int clip_x0, clip_y0, clip_x1, clip_y1; /* x1/y1 exclusive */
} gfx_surface;

void gfx_init(gfx_surface *s, unsigned int *px, int w, int h, int stride);
/* Set the clip to (x,y,w,h) intersected with the surface bounds. */
void gfx_set_clip(gfx_surface *s, int x, int y, int w, int h);
void gfx_reset_clip(gfx_surface *s);
/* Make `sub` a view of the (x,y,w,h) part of `s` (clipped to s's bounds),
 * sharing pixels. Its clip is s's clip translated and intersected. */
void gfx_sub(gfx_surface *sub, const gfx_surface *s, int x, int y, int w, int h);

void gfx_clear(gfx_surface *s, unsigned int color);  /* fill the clip rect */
void gfx_fill_rect(gfx_surface *s, int x, int y, int w, int h, unsigned int color);
void gfx_blend_rect(gfx_surface *s, int x, int y, int w, int h, unsigned int color);
void gfx_round_rect_fill(gfx_surface *s, int x, int y, int w, int h, int radius, unsigned int color);
void gfx_round_rect_stroke(gfx_surface *s, int x, int y, int w, int h, int radius,
                           int thickness, unsigned int color);
/* 1-pixel Bresenham line including both end points (blends if alpha < 255). */
void gfx_line(gfx_surface *s, int x0, int y0, int x1, int y1, unsigned int color);
/* Anti-aliased 1-pixel line (Xiaolin Wu) between pixel centres. */
void gfx_line_aa(gfx_surface *s, float x0, float y0, float x1, float y1, unsigned int color);
/* Disc of radius r centred on the pixel corner (cx,cy): spans [cx-r, cx+r). */
void gfx_circle_fill(gfx_surface *s, int cx, int cy, int r, unsigned int color);

void gfx_blit(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h);
/* Source-over with straight (non-premultiplied) source alpha. */
void gfx_blit_alpha(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h);

/* Coverage-blend a glyph; colour alpha multiplies the coverage. */
void gfx_glyph(gfx_surface *s, int pen_x, int baseline_y, const ttf_glyph *g, unsigned int color);
/* Draw UTF-8 text; returns the pen x after the text. len -1 = NUL-terminated. */
int gfx_text(gfx_surface *s, ttf_cache *c, int x, int baseline_y, const char *utf8, int len,
             unsigned int color);
int gfx_text_width(ttf_cache *c, const char *utf8, int len);
/* Decode one code point and advance *p (never past end). Returns U+FFFD for
 * malformed, overlong, surrogate or out-of-range sequences (consuming one
 * byte). */
unsigned gfx_utf8_next(const char **p, const char *end);

/* Blend helper exposed for callers writing their own loops: blends colour c
 * (its alpha byte ignored) over d with weight a in 0..255. */
static inline unsigned int gfx_blend_px(unsigned int d, unsigned int c, unsigned int a)
{
    unsigned int ia = 255u - a, s = c | 0xFF000000u;
    unsigned int rb = (s & 0x00FF00FFu) * a + (d & 0x00FF00FFu) * ia + 0x00800080u;
    unsigned int ga = ((s >> 8) & 0x00FF00FFu) * a + ((d >> 8) & 0x00FF00FFu) * ia + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    ga = (ga + ((ga >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
    return rb | ga;
}

#ifdef __cplusplus
}
#endif
#endif
