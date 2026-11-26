// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ttftest_ft.c - reference renderer using the host FreeType (test only).
 * Writes one glyph's coverage as PGM, unhinted (FT_LOAD_NO_HINTING) or
 * hinted, and prints "w h left top advance" like `ttftest glyph`.
 *
 * Build: cc -O2 $(pkg-config --cflags freetype2) -o ttftest_ft tools/ttftest_ft.c $(pkg-config --libs freetype2)
 * Usage: ttftest_ft FONT CODEPOINT PX OUT.pgm [hinted]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

int main(int argc, char **argv)
{
    FT_Library lib;
    FT_Face face;
    FT_GlyphSlot g;
    FILE *fp;
    unsigned r;
    int flags = FT_LOAD_RENDER;
    if (argc < 5) { fprintf(stderr, "usage: ttftest_ft FONT CP PX OUT.pgm [hinted]\n"); return 2; }
    if (argc < 6 || strcmp(argv[5], "hinted")) flags |= FT_LOAD_NO_HINTING;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[1], 0, &face)) return 1;
    FT_Set_Char_Size(face, 0, (FT_F26Dot6)(atof(argv[3]) * 64.0 + 0.5), 72, 72);
    if (FT_Load_Char(face, (FT_ULong)strtoul(argv[2], 0, 0), flags)) return 1;
    g = face->glyph;
    printf("%u %u %d %d %ld\n", g->bitmap.width, g->bitmap.rows, g->bitmap_left, g->bitmap_top,
           (g->advance.x + 32) >> 6);
    fp = fopen(argv[4], "wb");
    fprintf(fp, "P5\n%u %u\n255\n", g->bitmap.width, g->bitmap.rows);
    for (r = 0; r < g->bitmap.rows; r++)
        fwrite(g->bitmap.buffer + (long)r * g->bitmap.pitch, 1, g->bitmap.width, fp);
    fclose(fp);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return 0;
}
