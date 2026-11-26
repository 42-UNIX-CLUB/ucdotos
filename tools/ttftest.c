// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ttftest.c - native test tool for libgfx (ttf.c + gfx.c).
 *
 * Build:
 *   cc -O2 -Ilibgfx -o ttftest tools/ttftest.c libgfx/ttf.c libgfx/gfx.c
 * (add -fsanitize=address,undefined -g for the fuzz run)
 *
 * Commands:
 *   ttftest info  FONT...                    table/cmap summary, glyf/CFF/fvar check
 *   ttftest text  FONT OUT.ppm PX...         sample string at several sizes
 *   ttftest box   FONT OUT.ppm PX            box drawing / blocks / powerline
 *   ttftest glyph FONT CODEPOINT PX OUT.pgm  one glyph's raw coverage (+ prints left/top)
 *   ttftest card  FONT OUT.ppm               gfx primitive test card
 *   ttftest fuzz  FONT N SEED                mutate font N times, open + render
 *   ttftest bench FONT                       raster and text drawing speed
 *   ttftest blend                            exhaustive blend formula check
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ttf.h"
#include "gfx.h"

static const char *SAMPLE = "clubs@42lisboa:~/unix_club$ ./intro.sh  {}[]()<>=!&|;  0O il1";

static unsigned char *load_file(const char *path, unsigned *len)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fprintf(stderr, "read fail\n"); exit(1); }
    fclose(fp);
    *len = (unsigned)n;
    return buf;
}

static void open_font(const char *path, ttf_font *f, unsigned char **data)
{
    unsigned len;
    int r;
    *data = load_file(path, &len);
    r = ttf_open(f, *data, len);
    if (r) { fprintf(stderr, "%s: ttf_open failed %d\n", path, r); exit(1); }
}

static void write_ppm(const char *path, const gfx_surface *s)
{
    FILE *fp = fopen(path, "wb");
    int x, y;
    if (!fp) { perror(path); exit(1); }
    fprintf(fp, "P6\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++)
        for (x = 0; x < s->w; x++) {
            unsigned c = s->px[y * s->stride + x];
            unsigned char rgb[3] = { (unsigned char)GFX_R(c), (unsigned char)GFX_G(c), (unsigned char)GFX_B(c) };
            fwrite(rgb, 1, 3, fp);
        }
    fclose(fp);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int has_table(const unsigned char *d, unsigned len, const char *tag)
{
    unsigned n, i;
    if (len < 12) return 0;
    n = (d[4] << 8) | d[5];
    for (i = 0; i < n && 12 + i * 16 + 16 <= len; i++)
        if (!memcmp(d + 12 + i * 16, tag, 4)) return 1;
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    int i;
    for (i = 0; i < argc; i++) {
        ttf_font f;
        unsigned char *d;
        unsigned len;
        int r;
        ttf_metrics m;
        d = load_file(argv[i], &len);
        r = ttf_open(&f, d, len);
        printf("%s: open=%d glyf=%d CFF=%d fvar=%d gvar=%d", argv[i], r, has_table(d, len, "glyf"),
               has_table(d, len, "CFF "), has_table(d, len, "fvar"), has_table(d, len, "gvar"));
        if (r == 0) {
            int ok = 0, fail = 0, g;
            ttf_metrics_px(&f, 16, &m);
            printf(" upem=%d glyphs=%d loca_long=%d cmap_fmt=%d asc=%d desc=%d gap=%d advM=%d\n"
                   "   16px: ascent=%d descent=%d line_height=%d cell=%d\n"
                   "   gid: A=%d U+2500=%d U+2588=%d U+E0B0=%d U+E0B2=%d U+F121(nf)=%d U+1F600=%d\n",
                   f.units_per_em, f.num_glyphs, f.loca_long, f.cmap_format, f.ascender, f.descender,
                   f.line_gap, f.advance_m, m.ascent, m.descent, m.line_height, m.advance,
                   ttf_glyph_index(&f, 'A'), ttf_glyph_index(&f, 0x2500), ttf_glyph_index(&f, 0x2588),
                   ttf_glyph_index(&f, 0xE0B0), ttf_glyph_index(&f, 0xE0B2), ttf_glyph_index(&f, 0xF121),
                   ttf_glyph_index(&f, 0x1F600));
            /* render every glyph in the font at 16 px */
            for (g = 0; g < f.num_glyphs; g++) {
                ttf_glyph gl;
                if (ttf_render(&f, g, 16, &gl) == 0) { ok++; ttf_glyph_free(&gl); }
                else fail++;
            }
            printf("   rendered all glyphs at 16px: ok=%d fail=%d\n", ok, fail);
        } else {
            printf("\n");
        }
        free(d);
    }
    return 0;
}

static int cmd_text(int argc, char **argv)
{
    ttf_font f;
    unsigned char *d;
    int i, W = 0, H = 8, y;
    gfx_surface s;
    unsigned *px;
    open_font(argv[0], &f, &d);
    for (i = 2; i < argc; i++) {
        float p = (float)atof(argv[i]);
        ttf_metrics m;
        ttf_cache *c = ttf_cache_new(&f, p, 256);
        int w = gfx_text_width(c, SAMPLE, -1) + 16;
        ttf_metrics_px(&f, p, &m);
        if (w > W) W = w;
        H += m.line_height + 8;
        ttf_cache_free(c);
    }
    px = malloc((size_t)W * H * 4);
    gfx_init(&s, px, W, H, W);
    gfx_fill_rect(&s, 0, 0, W, H, GFX_HEX(0x282828));
    y = 4;
    for (i = 2; i < argc; i++) {
        float p = (float)atof(argv[i]);
        ttf_metrics m;
        ttf_cache *c = ttf_cache_new(&f, p, 256);
        ttf_metrics_px(&f, p, &m);
        gfx_text(&s, c, 8, y + m.ascent, SAMPLE, -1, GFX_HEX(0xebdbb2));
        printf("px=%s baseline_y=%d ascent=%d line_height=%d\n", argv[i], y + m.ascent, m.ascent, m.line_height);
        y += m.line_height + 8;
        ttf_cache_free(c);
    }
    write_ppm(argv[1], &s);
    free(px);
    free(d);
    return 0;
}

/* Box drawing sample laid out on a fixed character grid. */
static int cmd_box(int argc, char **argv)
{
    static const char *lines[] = {
        "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 \xe2\x94\x82 \xe2\x94\x80 "
        "\xe2\x95\xad\xe2\x94\x80\xe2\x95\xae \xe2\x95\xb0\xe2\x94\x80\xe2\x95\xaf "
        "\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88 "
        "\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93 \xee\x82\xb0\xee\x82\xb2",
        "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae",
        "\xe2\x94\x82" "abcd\xe2\x94\x82\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x94\x82",
        "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4",
        "\xe2\x94\x82\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x94\x82\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x94\x82",
        "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf",
        "\xee\x82\xb2\xe2\x96\x88\xe2\x96\x88 main \xe2\x96\x88\xe2\x96\x88\xee\x82\xb0",
        /* two identical rows of full blocks and vertical lines: must tile */
        "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x94\x82\xe2\x94\x83\xe2\x95\x91",
        "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x94\x82\xe2\x94\x83\xe2\x95\x91",
    };
    ttf_font f;
    unsigned char *d;
    float p = (float)atof(argv[2]);
    ttf_metrics m;
    ttf_cache *c;
    gfx_surface s;
    unsigned *px;
    int W, H, i, nl = (int)(sizeof lines / sizeof lines[0]);
    (void)argc;
    open_font(argv[0], &f, &d);
    ttf_metrics_px(&f, p, &m);
    c = ttf_cache_new(&f, p, 512);
    W = 60 * m.advance + 16;
    H = nl * m.line_height + 16;
    px = malloc((size_t)W * H * 4);
    gfx_init(&s, px, W, H, W);
    gfx_fill_rect(&s, 0, 0, W, H, GFX_HEX(0x282828));
    for (i = 0; i < nl; i++) {
        /* place every code point at an integer cell position */
        const char *q = lines[i], *e = q + strlen(q);
        int col = 0;
        while (q < e) {
            unsigned cp = gfx_utf8_next(&q, e);
            const ttf_glyph *g = ttf_cache_get(c, cp);
            gfx_glyph(&s, 8 + col * m.advance, 8 + i * m.line_height + m.ascent, g, GFX_HEX(0xebdbb2));
            col++;
        }
    }
    printf("cell %dx%d ascent=%d\n", m.advance, m.line_height, m.ascent);
    {
        /* the last two rows start with 3 full blocks: every pixel of that
         * 3-cell x 2-row area must be fully covered (no seams) */
        int x, y, bad = 0, y0 = 8 + (nl - 2) * m.line_height;
        for (y = y0; y < y0 + 2 * m.line_height; y++)
            for (x = 8; x < 8 + 3 * m.advance; x++)
                if (s.px[y * s.stride + x] != GFX_HEX(0xebdbb2)) bad++;
        printf("block tiling: %d of %d pixels not fully covered\n", bad, 6 * m.advance * m.line_height);
        /* vertical line U+2502 in column 3: every row of both cells must have ink */
        bad = 0;
        for (y = y0; y < y0 + 2 * m.line_height; y++) {
            int ink = 0;
            for (x = 8 + 3 * m.advance; x < 8 + 4 * m.advance; x++)
                if (s.px[y * s.stride + x] != GFX_HEX(0x282828)) ink = 1;
            if (!ink) bad++;
        }
        printf("vertical line U+2502: %d of %d pixel rows without ink\n", bad, 2 * m.line_height);
    }
    write_ppm(argv[1], &s);
    ttf_cache_free(c);
    free(px);
    free(d);
    return 0;
}

static int cmd_glyph(int argc, char **argv)
{
    ttf_font f;
    unsigned char *d;
    ttf_glyph g;
    int r, gi;
    FILE *fp;
    (void)argc;
    open_font(argv[0], &f, &d);
    gi = ttf_glyph_index(&f, (unsigned)strtoul(argv[1], 0, 0));
    r = ttf_render(&f, gi, (float)atof(argv[2]), &g);
    if (r) { fprintf(stderr, "render failed %d\n", r); return 1; }
    printf("%d %d %d %d %d\n", g.w, g.h, g.left, g.top, g.advance);
    fp = fopen(argv[3], "wb");
    fprintf(fp, "P5\n%d %d\n255\n", g.w, g.h);
    if (g.cov) fwrite(g.cov, 1, (size_t)g.w * g.h, fp);
    fclose(fp);
    ttf_glyph_free(&g);
    free(d);
    return 0;
}

static int cmd_card(int argc, char **argv)
{
    ttf_font f;
    unsigned char *d;
    gfx_surface s, spr, sub;
    unsigned *px, *spx;
    ttf_cache *c;
    ttf_metrics m;
    int W = 640, H = 420, i, x, y;
    (void)argc;
    open_font(argv[0], &f, &d);
    c = ttf_cache_new(&f, 16, 256);
    ttf_metrics_px(&f, 16, &m);
    px = malloc((size_t)W * H * 4);
    gfx_init(&s, px, W, H, W);
    gfx_fill_rect(&s, 0, 0, W, H, GFX_HEX(0x282828));

    /* window-like rounded rects */
    gfx_round_rect_fill(&s, 20, 20, 180, 110, 8, GFX_HEX(0x3c3836));
    gfx_round_rect_stroke(&s, 20, 20, 180, 110, 8, 2, GFX_HEX(0xd79921));
    gfx_text(&s, c, 32, 20 + 12 + m.ascent, "stroke r8 t2", -1, GFX_HEX(0xebdbb2));
    gfx_round_rect_fill(&s, 220, 20, 180, 110, 16, GFX_HEX(0x458588));
    gfx_text(&s, c, 232, 32 + m.ascent, "fill r16", -1, GFX_HEX(0xfbf1c7));
    gfx_round_rect_stroke(&s, 420, 20, 200, 110, 24, 1, GFX_HEX(0xb8bb26));
    gfx_round_rect_stroke(&s, 430, 30, 180, 90, 4, 6, GFX_HEXA(0xfb4934, 160));
    gfx_text(&s, c, 440, 70, "t1 r24 / t6 a160", -1, GFX_HEX(0xebdbb2));

    /* circles */
    for (i = 0; i < 6; i++)
        gfx_circle_fill(&s, 40 + i * 50, 180, 4 + i * 4, GFX_HEX(0xfe8019));
    gfx_circle_fill(&s, 360, 180, 30, GFX_HEXA(0x83a598, 128));
    gfx_circle_fill(&s, 390, 180, 30, GFX_HEXA(0xd3869b, 128));
    gfx_circle_fill(&s, 3, 180, 20, GFX_HEX(0xfabd2f));  /* clipped at the left edge */

    /* lines: fan of Bresenham and AA lines */
    for (i = 0; i <= 8; i++) {
        gfx_line(&s, 460, 240, 460 + 160 * (8 - i) / 8, 240 - 80 * i / 8 + 160, GFX_HEX(0xebdbb2));
        gfx_line_aa(&s, 450.0f, 240.0f, 450.0f - 140.0f * (8 - i) / 8, 240.0f + 20.0f * i, GFX_HEX(0x8ec07c));
    }
    gfx_line(&s, -1000, -1000, 100000, 100000, GFX_HEXA(0xffffff, 60));   /* huge clipped line */

    /* alpha blends: stripes under translucent rects */
    for (x = 20; x < 300; x += 20) gfx_fill_rect(&s, x, 230, 10, 80, GFX_HEX(0xebdbb2));
    gfx_blend_rect(&s, 20, 240, 280, 20, GFX_RGBA(0, 0, 0, 128));
    gfx_blend_rect(&s, 20, 270, 280, 20, GFX_RGBA(255, 0, 0, 64));
    gfx_blend_rect(&s, 20, 300, 280, 20, GFX_RGBA(0, 128, 255, 192));

    /* sprite with an alpha gradient, blitted with source-over */
    spx = malloc(64 * 64 * 4);
    gfx_init(&spr, spx, 64, 64, 64);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) spx[y * 64 + x] = GFX_RGBA(x * 4, y * 4, 200, (x + y) * 2);
    gfx_blit_alpha(&s, 320, 230, &spr, 0, 0, 64, 64);
    gfx_blit(&s, 390, 230, &spr, 0, 0, 64, 64);
    gfx_blit(&s, 600, 380, &spr, 0, 0, 64, 64);   /* partly off-surface */

    /* clipped text: clip to a box, text overflows it */
    gfx_round_rect_stroke(&s, 19, 339, 202, 62, 0, 1, GFX_HEX(0x928374));
    gfx_set_clip(&s, 20, 340, 200, 60);
    gfx_fill_rect(&s, 0, 0, W, H, GFX_HEX(0x1d2021));
    for (i = 0; i < 4; i++)
        gfx_text(&s, c, 10 - i * 5, 340 + 4 + m.ascent + i * (m.line_height - 2),
                 "clipped text runs past the clip box edge", -1, GFX_HEX(0xebdbb2));
    gfx_reset_clip(&s);

    /* sub-surface with its own clip */
    gfx_sub(&sub, &s, 240, 340, 200, 60);
    gfx_fill_rect(&sub, 0, 0, 1000, 1000, GFX_HEX(0x504945));
    gfx_text(&sub, c, 6, 6 + m.ascent, "sub-surface \xe2\x94\x82 UTF-8 \xc3\xa9\xc3\xa0\xc3\xb1", -1, GFX_HEX(0xfabd2f));
    gfx_text(&sub, c, 6, 30 + m.ascent, "bad utf8: \xff\xc0\xaf\xed\xa0\x80 end", -1, GFX_HEX(0xfb4934));

    write_ppm(argv[1], &s);
    ttf_cache_free(c);
    free(spx);
    free(px);
    free(d);
    return 0;
}

static unsigned rng_state;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int cmd_fuzz(int argc, char **argv)
{
    unsigned len, i;
    unsigned char *orig = load_file(argv[0], &len), *buf = malloc(len);
    int n = atoi(argv[1]), it, opened = 0, rendered = 0, rerr = 0;
    ttf_font of;
    (void)argc;
    if (ttf_open(&of, orig, len)) { fprintf(stderr, "original font does not open\n"); return 1; }
    rng_state = (unsigned)strtoul(argv[2], 0, 0) | 1;
    for (it = 0; it < n; it++) {
        ttf_font f;
        unsigned flen = len, k, mode = it % 6;
        int g;
        memcpy(buf, orig, len);
        if (mode == 0) {
            flen = rnd() % len;                       /* truncation */
        } else if (mode == 1) {
            unsigned nflip = 1 + rnd() % 64;          /* random bit flips anywhere */
            for (k = 0; k < nflip; k++) buf[rnd() % len] ^= (unsigned char)(1u << (rnd() % 8));
        } else if (mode == 2) {
            /* hammer the header + table directory + cmap/head/hhea region */
            unsigned nflip = 1 + rnd() % 16;
            unsigned lim = len < 4096 ? len : 4096;
            for (k = 0; k < nflip; k++) buf[rnd() % lim] = (unsigned char)rnd();
        } else if (mode == 4) {
            /* flips inside the outlines of the first glyphs (glyf data) */
            unsigned nflip = 1 + rnd() % 8, lim = of.glyf_len < 65536 ? of.glyf_len : 65536;
            for (k = 0; k < nflip; k++) buf[of.glyf + rnd() % lim] = (unsigned char)rnd();
        } else if (mode == 5 && of.cmap_sub_len) {
            /* corrupt loca entries and the selected cmap subtable */
            unsigned nflip = 1 + rnd() % 8;
            for (k = 0; k < nflip; k++) {
                if (rnd() & 1) buf[of.loca + rnd() % of.loca_len] = (unsigned char)rnd();
                else buf[of.cmap_sub + rnd() % of.cmap_sub_len] = (unsigned char)rnd();
            }
        } else {
            /* random byte runs, plus truncation */
            unsigned nrun = 1 + rnd() % 8;
            for (k = 0; k < nrun; k++) {
                unsigned at = rnd() % len, l = 1 + rnd() % 32, j;
                for (j = 0; j < l && at + j < len; j++) buf[at + j] = (unsigned char)rnd();
            }
            if (rnd() & 1) flen = len - rnd() % (len / 4 + 1);
        }
        /* copy to an exact-size heap block so ASan sees any overread */
        {
            unsigned char *exact = malloc(flen ? flen : 1);
            memcpy(exact, buf, flen);
            if (ttf_open(&f, exact, flen) == 0) {
                opened++;
                for (g = 0; g < 300; g++) {
                    ttf_glyph gl;
                    int gi = (g < 100) ? ttf_glyph_index(&f, 32 + g)
                                       : (int)(rnd() % (unsigned)(f.num_glyphs + 10));
                    float p = (g & 1) ? 16.0f : 31.5f;
                    if (ttf_render(&f, gi, p, &gl) == 0) { rendered++; ttf_glyph_free(&gl); }
                    else rerr++;
                }
                ttf_glyph_index(&f, 0x2500);
                ttf_glyph_index(&f, 0x1F600);
                ttf_glyph_index(&f, rnd());
                {
                    ttf_cache *c = ttf_cache_new(&f, 14, 16);
                    for (g = 0; g < 64; g++) ttf_cache_get(c, rnd() % 0x3000);
                    ttf_cache_free(c);
                }
            }
            free(exact);
        }
    }
    for (i = 0; i < 3; i++) {   /* degenerate inputs */
        ttf_font f;
        static const unsigned char junk[16] = { 0, 1, 0, 0, 0xff, 0xff };
        ttf_open(&f, junk, i * 8);
    }
    printf("fuzz: %d mutations, opened=%d, glyph renders ok=%d err=%d\n", n, opened, rendered, rerr);
    {
        /* gfx primitives with random, often absurd, arguments on a small
         * exact-size surface: ASan catches any out-of-bounds write */
        static const int big[] = { 0, 1, -1, 7, 40, -40, 100000, -100000, 0x7fffffff, (int)0x80000000 };
        unsigned *px = malloc(37 * 23 * 4), *spx = malloc(11 * 9 * 4);
        gfx_surface s, src, sub;
        ttf_cache *c = ttf_cache_new(&of, 13, 32);
        gfx_init(&s, px, 37, 23, 37);
        gfx_init(&src, spx, 11, 9, 11);
        memset(spx, 0x80, 11 * 9 * 4);
#define R() ((rnd() & 3) ? (int)(rnd() % 80) - 20 : big[rnd() % 10])
        for (it = 0; it < 200000; it++) {
            unsigned col = rnd();
            if ((it & 63) == 0) { if (rnd() & 1) gfx_set_clip(&s, R(), R(), R(), R()); else gfx_reset_clip(&s); }
            switch (rnd() % 12) {
            case 0: gfx_fill_rect(&s, R(), R(), R(), R(), col); break;
            case 1: gfx_blend_rect(&s, R(), R(), R(), R(), col); break;
            case 2: gfx_round_rect_fill(&s, R(), R(), R(), R(), R(), col); break;
            case 3: gfx_round_rect_stroke(&s, R(), R(), R(), R(), R(), R(), col); break;
            case 4: gfx_line(&s, R(), R(), R(), R(), col); break;
            case 5: gfx_line_aa(&s, (float)R(), (float)R(), (float)R(), (float)R(), col); break;
            case 6: if (rnd() % 8 == 0) gfx_circle_fill(&s, R(), R(), (int)(rnd() % 60), col); else gfx_circle_fill(&s, R(), R(), R() % 300, col); break;
            case 7: gfx_blit(&s, R(), R(), &src, R(), R(), R(), R()); break;
            case 8: gfx_blit_alpha(&s, R(), R(), &src, R(), R(), R(), R()); break;
            case 9: gfx_blit(&s, R(), R(), &s, R(), R(), R(), R()); break;
            case 10: gfx_sub(&sub, &s, R(), R(), R(), R()); gfx_fill_rect(&sub, R(), R(), R(), R(), col);
                     gfx_text(&sub, c, R(), R(), "Ab\xe2\x94\x82", -1, col); break;
            default: {
                char t[8]; int k;
                for (k = 0; k < 7; k++) t[k] = (char)(rnd() & 0xff);
                t[7] = 0;
                gfx_text(&s, c, R(), R(), t, (int)(rnd() % 8), col);
            }
            }
        }
#undef R
        ttf_cache_free(c);
        free(px);
        free(spx);
        printf("gfx: 200000 random primitive calls done\n");
    }
    free(buf);
    free(orig);
    return 0;
}

static int cmd_bench(int argc, char **argv)
{
    ttf_font f;
    unsigned char *d;
    double t0, t1;
    int rep, cp, count = 0, i;
    gfx_surface s;
    unsigned *px;
    ttf_cache *c;
    char line[101];
    int W = 1200, H = 200 * 18;
    (void)argc;
    open_font(argv[0], &f, &d);

    t0 = now();
    for (rep = 0; rep < 50; rep++)
        for (cp = 32; cp < 127; cp++) {
            ttf_glyph g;
            if (ttf_render(&f, ttf_glyph_index(&f, (unsigned)cp), 16, &g) == 0) count++;
            ttf_glyph_free(&g);
        }
    t1 = now();
    printf("raster: %d glyphs (printable ASCII x50) at 16px in %.3f ms -> %.0f glyphs/s (%.2f us/glyph)\n",
           count, (t1 - t0) * 1e3, count / (t1 - t0), (t1 - t0) * 1e6 / count);

    px = malloc((size_t)W * H * 4);
    gfx_init(&s, px, W, H, W);
    c = ttf_cache_new(&f, 16, 256);
    for (i = 0; i < 100; i++) line[i] = (char)(33 + (i * 7) % 94);
    line[100] = 0;
    gfx_text(&s, c, 0, 14, line, 100, 0xffffffff);   /* warm the cache */
    t0 = now();
    for (rep = 0; rep < 20; rep++) {
        gfx_fill_rect(&s, 0, 0, W, H, GFX_HEX(0x282828));
        for (i = 0; i < 200; i++) gfx_text(&s, c, 4, 14 + i * 18, line, 100, GFX_HEX(0xebdbb2));
    }
    t1 = now();
    printf("text: 20 x (200 lines x 100 cached chars) in %.3f ms -> %.0f glyphs/s, %.3f ms per 20000-glyph screen (incl. clear)\n",
           (t1 - t0) * 1e3, 20 * 20000 / (t1 - t0), (t1 - t0) * 1e3 / 20);
    t0 = now();
    for (rep = 0; rep < 100; rep++) gfx_fill_rect(&s, 0, 0, W, H, GFX_HEX(0x282828));
    t1 = now();
    printf("fill: %.1f Mpixel/s\n", 100.0 * W * H / (t1 - t0) / 1e6);
    ttf_cache_free(c);
    free(px);
    free(d);
    return 0;
}

static int cmd_blend(void)
{
    unsigned a, c, d, bad = 0;
    for (a = 0; a < 256; a++)
        for (c = 0; c < 256; c++)
            for (d = 0; d < 256; d++) {
                unsigned want = (a * c + (255 - a) * d + 127) / 255;
                unsigned col = c | (c << 8) | (c << 16);
                unsigned dst = d | (d << 8) | (d << 16) | (d << 24);
                unsigned got = gfx_blend_px(dst, col, a);
                unsigned wa = (a * 255 + (255 - a) * d + 127) / 255;
                if (GFX_R(got) != want || GFX_G(got) != want || GFX_B(got) != want || GFX_A(got) != wa) bad++;
            }
    printf("blend: %u mismatches out of %u (a,c,d) triples\n", bad, 256u * 256 * 256);
    return bad != 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "blend")) return cmd_blend();
    if (argc < 3) {
        fprintf(stderr, "usage: see header of tools/ttftest.c\n");
        return 2;
    }
    if (!strcmp(argv[1], "info")) return cmd_info(argc - 2, argv + 2);
    if (!strcmp(argv[1], "text") && argc >= 5) return cmd_text(argc - 2, argv + 2);
    if (!strcmp(argv[1], "box") && argc >= 5) return cmd_box(argc - 2, argv + 2);
    if (!strcmp(argv[1], "glyph") && argc >= 6) return cmd_glyph(argc - 2, argv + 2);
    if (!strcmp(argv[1], "card") && argc >= 4) return cmd_card(argc - 2, argv + 2);
    if (!strcmp(argv[1], "fuzz") && argc >= 5) return cmd_fuzz(argc - 2, argv + 2);
    if (!strcmp(argv[1], "bench")) return cmd_bench(argc - 2, argv + 2);
    fprintf(stderr, "bad command\n");
    return 2;
}
