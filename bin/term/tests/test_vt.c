// SPDX-License-Identifier: GPL-2.0-or-later
/* Native unit tests of the parser and screen model.
 *   cc -std=c11 -g -fsanitize=address,undefined -I.. test_vt.c ../vt.c -o test_vt && ./test_vt */
#include "../vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void feed(vt *t, const char *s) { vt_feed(t, s, strlen(s)); }
static const char *row(vt *t, int y) {
  static char buf[8][1024];
  static int k;
  char *b = buf[k++ & 7];
  vt_row_text(t, y, b, 1024);
  return b;
}
#define ROW(t, y, s) CHECK(!strcmp(row(t, y), s), "row %d is \"%s\", want \"%s\"", y, row(t, y), s)
#define CUR(t, cx, cy) CHECK((t)->x == (cx) && (t)->y == (cy), "cursor at %d,%d, want %d,%d", (t)->x, (t)->y, cx, cy)
static vt_cell C(vt *t, int x, int y) { return t->scr[y][x]; }
static int reply_is(vt *t, const char *s) {
  int ok = t->reply_len == (int)strlen(s) && !memcmp(t->reply, s, strlen(s));
  if (!ok) printf("  reply was \"%.*s\" (%d bytes)\n", t->reply_len, t->reply, t->reply_len);
  t->reply_len = 0;
  return ok;
}

static void test_c0(void) {
  vt *t = vt_new(20, 5, 100);
  feed(t, "abc");
  ROW(t, 0, "abc"); CUR(t, 3, 0);
  feed(t, "\r");
  CUR(t, 0, 0);
  feed(t, "\nX");
  ROW(t, 1, "X"); CUR(t, 1, 1);
  feed(t, "\b\bY");
  ROW(t, 1, "Y"); CUR(t, 1, 1);
  feed(t, "\x0b" "V\x0c" "F");
  ROW(t, 2, " V"); ROW(t, 3, "  F");
  feed(t, "\x07\x07");
  CHECK(t->bell == 2, "bell count %d", t->bell);
  feed(t, "\r\t");
  CUR(t, 8, 3);
  feed(t, "\t");
  CUR(t, 16, 3);
  feed(t, "\t");
  CUR(t, 19, 3); /* no more stops: last column */
  feed(t, "\t");
  CUR(t, 19, 3);
  feed(t, "\x7f"); /* DEL is ignored */
  CUR(t, 19, 3);
  feed(t, "\x1b[H\x01\x02\x03Z"); /* other C0 controls do nothing */
  ROW(t, 0, "Zbc");
  vt_free(t);
}

static void test_wrap(void) {
  vt *t = vt_new(10, 3, 100);
  feed(t, "0123456789");
  CUR(t, 9, 0);
  CHECK(t->pending, "pending wrap after the last column");
  ROW(t, 1, "");
  feed(t, "A");
  ROW(t, 0, "0123456789"); ROW(t, 1, "A"); CUR(t, 1, 1);
  feed(t, "\x1b[1;1H0123456789\r");
  CHECK(!t->pending, "CR clears pending wrap");
  CUR(t, 0, 0);
  feed(t, "\x1b[2;1H0123456789\x1b[1C");
  CHECK(!t->pending, "cursor movement clears pending wrap");
  feed(t, "\x1b[3;1H0123456789\b");
  CUR(t, 8, 2); /* BS from the pending position goes to the column before last (xterm) */
  /* wrap at the bottom scrolls */
  feed(t, "\x1b[3;1H0123456789Z");
  ROW(t, 2, "Z"); ROW(t, 1, "0123456789"); CUR(t, 1, 2);
  /* autowrap off: the last column is overwritten */
  feed(t, "\x1b[?7l\x1b[1;1Habcdefghijklm");
  ROW(t, 0, "abcdefghim"); CUR(t, 9, 0);
  feed(t, "\x1b[?7h");
  /* ESC 7 / ESC 8 remember the pending state */
  feed(t, "\x1b[2J\x1b[1;1H0123456789\x1b" "7\x1b[3;3H\x1b" "8Q");
  ROW(t, 1, "Q");
  /* EL in pending state clears it */
  feed(t, "\x1b[2J\x1b[1;1H0123456789\x1b[K");
  CHECK(!t->pending, "EL resets pending wrap");
  ROW(t, 0, "012345678");
  vt_free(t);
}

static void test_cursor(void) {
  vt *t = vt_new(20, 10, 0);
  feed(t, "\x1b[5;10H");
  CUR(t, 9, 4);
  feed(t, "\x1b[2A"); CUR(t, 9, 2);
  feed(t, "\x1b[A"); CUR(t, 9, 1);
  feed(t, "\x1b[0A"); CUR(t, 9, 0);
  feed(t, "\x1b[99A"); CUR(t, 9, 0);
  feed(t, "\x1b[3B"); CUR(t, 9, 3);
  feed(t, "\x1b[99B"); CUR(t, 9, 9);
  feed(t, "\x1b[4C"); CUR(t, 13, 9);
  feed(t, "\x1b[99C"); CUR(t, 19, 9);
  feed(t, "\x1b[5D"); CUR(t, 14, 9);
  feed(t, "\x1b[99D"); CUR(t, 0, 9);
  feed(t, "\x1b[5;5H\x1b[2E"); CUR(t, 0, 6);
  feed(t, "\x1b[5;5H\x1b[3F"); CUR(t, 0, 1);
  feed(t, "\x1b[7G"); CUR(t, 6, 1);
  feed(t, "\x1b[G"); CUR(t, 0, 1);
  feed(t, "\x1b[99G"); CUR(t, 19, 1);
  feed(t, "\x1b[H"); CUR(t, 0, 0);
  feed(t, "\x1b[3;4f"); CUR(t, 3, 2);
  feed(t, "\x1b[;8H"); CUR(t, 7, 0);
  feed(t, "\x1b[6d"); CUR(t, 7, 5);
  feed(t, "\x1b[99;99H"); CUR(t, 19, 9);
  feed(t, "\x1b[0;0H"); CUR(t, 0, 0);
  feed(t, "\x1b[2;3H\x1b[3`"); CUR(t, 2, 1); /* HPA */
  feed(t, "\x1b[2a"); CUR(t, 4, 1);         /* HPR */
  feed(t, "\x1b[2e"); CUR(t, 4, 3);         /* VPR */
  vt_free(t);
}

static void fillscreen(vt *t) {
  feed(t, "\x1b[H");
  for (int y = 0; y < t->rows; y++) {
    char b[256];
    for (int x = 0; x < t->cols; x++) b[x] = (char)('a' + y);
    b[t->cols] = 0;
    char p[32];
    snprintf(p, sizeof p, "\x1b[%d;1H", y + 1);
    feed(t, p); feed(t, b);
  }
}

static void test_erase(void) {
  vt *t = vt_new(5, 4, 10);
  fillscreen(t);
  feed(t, "\x1b[2;3H\x1b[J");
  ROW(t, 0, "aaaaa"); ROW(t, 1, "bb"); ROW(t, 2, ""); ROW(t, 3, "");
  fillscreen(t);
  feed(t, "\x1b[2;3H\x1b[0J");
  ROW(t, 1, "bb");
  fillscreen(t);
  feed(t, "\x1b[3;3H\x1b[1J");
  ROW(t, 0, ""); ROW(t, 1, ""); ROW(t, 2, "   cc"); ROW(t, 3, "ddddd");
  fillscreen(t);
  feed(t, "\x1b[2J");
  for (int y = 0; y < 4; y++) ROW(t, y, "");
  CUR(t, 4, 3); /* ED does not move the cursor */
  fillscreen(t);
  feed(t, "\x1b[2;3H\x1b[K");
  ROW(t, 1, "bb");
  fillscreen(t);
  feed(t, "\x1b[2;3H\x1b[1K");
  ROW(t, 1, "   bb");
  fillscreen(t);
  feed(t, "\x1b[2;3H\x1b[2K");
  ROW(t, 1, ""); ROW(t, 0, "aaaaa");
  fillscreen(t);
  feed(t, "\x1b[2;2H\x1b[2X");
  ROW(t, 1, "b  bb"); CUR(t, 1, 1);
  feed(t, "\x1b[2;4H\x1b[9X");
  ROW(t, 1, "b");
  /* erase uses the current background colour */
  feed(t, "\x1b[44m\x1b[3;1H\x1b[K\x1b[m");
  CHECK(C(t, 0, 2).bg == VT_PAL(4) && C(t, 4, 2).bg == VT_PAL(4) && C(t, 0, 2).ch == ' ', "bce");
  CHECK(C(t, 0, 2).attr == 0 && C(t, 0, 2).fg == VT_DEFAULT, "erased cells have no attributes");
  /* ED 3 clears the scrollback only */
  for (int i = 0; i < 8; i++) feed(t, "\r\nline");
  CHECK(t->sb_len > 0, "scrollback filled");
  feed(t, "\x1b[3J");
  CHECK(t->sb_len == 0, "ED 3 clears scrollback");
  ROW(t, 3, "line");
  vt_free(t);
}

static void test_insert_delete(void) {
  vt *t = vt_new(6, 5, 0);
  feed(t, "abcdef\x1b[1;3H\x1b[2@");
  ROW(t, 0, "ab  cd"); CUR(t, 2, 0);
  feed(t, "\x1b[@");
  ROW(t, 0, "ab   c");
  feed(t, "\x1b[99@");
  ROW(t, 0, "ab");
  feed(t, "\x1b[1;1Habcdef\x1b[1;2H\x1b[2P");
  ROW(t, 0, "adef");
  feed(t, "\x1b[P");
  ROW(t, 0, "aef");
  feed(t, "\x1b[99P");
  ROW(t, 0, "a");
  fillscreen(t);
  feed(t, "\x1b[2;4H\x1b[2L");
  ROW(t, 0, "aaaaaa"); ROW(t, 1, ""); ROW(t, 2, ""); ROW(t, 3, "bbbbbb"); ROW(t, 4, "cccccc");
  CUR(t, 0, 1); /* IL moves to the first column */
  fillscreen(t);
  feed(t, "\x1b[2;4H\x1b[2M");
  ROW(t, 1, "dddddd"); ROW(t, 2, "eeeeee"); ROW(t, 3, ""); ROW(t, 4, "");
  fillscreen(t);
  feed(t, "\x1b[2;4r\x1b[1;1H\x1b[L"); /* outside the region: no effect */
  ROW(t, 0, "aaaaaa"); ROW(t, 1, "bbbbbb");
  feed(t, "\x1b[3;1H\x1b[L");
  ROW(t, 1, "bbbbbb"); ROW(t, 2, ""); ROW(t, 3, "cccccc"); ROW(t, 4, "eeeeee");
  feed(t, "\x1b[2;1H\x1b[M");
  ROW(t, 1, ""); ROW(t, 2, "cccccc"); ROW(t, 3, ""); ROW(t, 4, "eeeeee");
  feed(t, "\x1b[r");
  /* insert mode (IRM) */
  feed(t, "\x1b[2J\x1b[Habc\x1b[1;1H\x1b[4hXY\x1b[4l");
  ROW(t, 0, "XYabc");
  vt_free(t);
}

static void test_scroll(void) {
  vt *t = vt_new(4, 5, 100);
  fillscreen(t);
  feed(t, "\x1b[2S");
  ROW(t, 0, "cccc"); ROW(t, 2, "eeee"); ROW(t, 3, ""); ROW(t, 4, "");
  CHECK(t->sb_len == 2, "SU saves to scrollback (%d)", t->sb_len);
  feed(t, "\x1b[T");
  ROW(t, 0, ""); ROW(t, 1, "cccc");
  fillscreen(t);
  feed(t, "\x1b[2;4r");
  CUR(t, 0, 0); /* DECSTBM homes the cursor */
  feed(t, "\x1b[4;1H\nX");
  ROW(t, 0, "aaaa"); ROW(t, 1, "cccc"); ROW(t, 2, "dddd"); ROW(t, 3, "X"); ROW(t, 4, "eeee");
  feed(t, "\x1b[2;1H\x1bM");
  ROW(t, 1, ""); ROW(t, 2, "cccc"); ROW(t, 3, "dddd"); ROW(t, 4, "eeee");
  feed(t, "\x1b[5;1H\n\n");
  CUR(t, 0, 4); /* below the region: LF stops at the last line */
  ROW(t, 4, "eeee");
  feed(t, "\x1b[S");
  ROW(t, 1, "cccc"); ROW(t, 3, ""); ROW(t, 0, "aaaa");
  feed(t, "\x1b[3;3r");
  CHECK(t->top == 1 && t->bot == 3, "invalid region ignored");
  feed(t, "\x1b[r");
  CHECK(t->top == 0 && t->bot == 4, "region reset");
  /* region scrolling never touches the scrollback */
  int before = t->sb_len;
  feed(t, "\x1b[2;5r\x1b[5;1H\n\n\x1b[r");
  CHECK(t->sb_len == before, "region with top > 0 does not save lines");
  /* RI at the top of the screen scrolls down */
  fillscreen(t);
  feed(t, "\x1b[H\x1bM");
  ROW(t, 0, ""); ROW(t, 1, "aaaa"); ROW(t, 4, "dddd");
  /* ESC D and ESC E */
  feed(t, "\x1b[5;3H\x1b" "D");
  CUR(t, 2, 4); ROW(t, 3, "dddd");
  feed(t, "\x1b" "E");
  CUR(t, 0, 4);
  vt_free(t);
}

static void test_sgr(void) {
  vt *t = vt_new(40, 3, 0);
  feed(t, "\x1b[1;2;3;4;5;7;8;9mA");
  vt_cell a = C(t, 0, 0);
  CHECK(a.attr == (VT_BOLD | VT_DIM | VT_ITALIC | VT_UNDERLINE | VT_BLINK | VT_INVERSE | VT_HIDDEN | VT_STRIKE), "all attrs %x", a.attr);
  feed(t, "\x1b[22mB");
  CHECK(!(C(t, 1, 0).attr & (VT_BOLD | VT_DIM)), "22 clears bold and dim");
  feed(t, "\x1b[23;24;25;27;28;29mC");
  CHECK(C(t, 2, 0).attr == 0, "all resets %x", C(t, 2, 0).attr);
  feed(t, "\x1b[31;42mD");
  CHECK(C(t, 3, 0).fg == VT_PAL(1) && C(t, 3, 0).bg == VT_PAL(2), "basic colours");
  feed(t, "\x1b[97;107mE");
  CHECK(C(t, 4, 0).fg == VT_PAL(15) && C(t, 4, 0).bg == VT_PAL(15), "bright colours");
  feed(t, "\x1b[90;100mF");
  CHECK(C(t, 5, 0).fg == VT_PAL(8) && C(t, 5, 0).bg == VT_PAL(8), "bright black");
  feed(t, "\x1b[39;49mG");
  CHECK(C(t, 6, 0).fg == VT_DEFAULT && C(t, 6, 0).bg == VT_DEFAULT, "39/49");
  feed(t, "\x1b[38;5;196;48;5;21mH");
  CHECK(C(t, 7, 0).fg == VT_PAL(196) && C(t, 7, 0).bg == VT_PAL(21), "256 colours");
  feed(t, "\x1b[38;2;1;2;3;48;2;250;251;252mI");
  CHECK(C(t, 8, 0).fg == VT_RGB(1, 2, 3) && C(t, 8, 0).bg == VT_RGB(250, 251, 252), "truecolor ;");
  feed(t, "\x1b[38:2:10:20:30mJ");
  CHECK(C(t, 9, 0).fg == VT_RGB(10, 20, 30), "truecolor : %x", C(t, 9, 0).fg);
  feed(t, "\x1b[38:2::40:50:60mK");
  CHECK(C(t, 10, 0).fg == VT_RGB(40, 50, 60), "truecolor : with colour space");
  feed(t, "\x1b[48:5:99mL");
  CHECK(C(t, 11, 0).bg == VT_PAL(99), "256 colours with :");
  feed(t, "\x1b[0;1;38;5;3mM");
  CHECK(C(t, 12, 0).fg == VT_PAL(3) && C(t, 12, 0).attr == VT_BOLD && C(t, 12, 0).bg == VT_DEFAULT, "0 then bold then colour");
  feed(t, "\x1b[mN");
  CHECK(C(t, 13, 0).fg == VT_DEFAULT && C(t, 13, 0).attr == 0, "CSI m resets");
  feed(t, "\x1b[4:0;1mO");
  CHECK(C(t, 14, 0).attr == VT_BOLD, "4:0 is no underline, then bold");
  feed(t, "\x1b[4:3mP");
  CHECK(C(t, 15, 0).attr & VT_UNDERLINE, "curly underline counts as underline");
  feed(t, "\x1b[0;38;5mQ"); /* truncated: ignored, no crash */
  CHECK(C(t, 16, 0).fg == VT_DEFAULT, "truncated 38;5");
  feed(t, "\x1b[38;2;1;2mR");
  CHECK(C(t, 17, 0).fg == VT_DEFAULT, "truncated 38;2");
  feed(t, "\x1b[38;5;300mS");
  CHECK(C(t, 18, 0).fg == VT_PAL(300 & 255), "out of range index wraps, no crash");
  feed(t, "\x1b[0;58:2::1:2:3;1mT");
  CHECK(C(t, 19, 0).attr == VT_BOLD && C(t, 19, 0).fg == VT_DEFAULT, "underline colour ignored");
  feed(t, "\x1b[0;7;31;44mU\x1b[0m");
  CHECK(C(t, 20, 0).attr == VT_INVERSE && C(t, 20, 0).fg == VT_PAL(1) && C(t, 20, 0).bg == VT_PAL(4), "inverse + colours");
  feed(t, "\x1b[21mV\x1b[24mW");
  CHECK((C(t, 21, 0).attr & VT_UNDERLINE) && !(C(t, 22, 0).attr & VT_UNDERLINE), "21, 24");
  feed(t, "\x1b[1;;31mX");
  CHECK(C(t, 23, 0).attr == 0 && C(t, 23, 0).fg == VT_PAL(1), "empty parameter means 0");
  feed(t, "\x1b[38;2;300;0;0mY\x1b[m");
  CHECK(C(t, 24, 0).fg == VT_RGB(300 & 255, 0, 0), "rgb component masked");
  vt_free(t);
}

static void test_reports(void) {
  vt *t = vt_new(80, 24, 0);
  feed(t, "\x1b[5n");
  CHECK(reply_is(t, "\x1b[0n"), "DSR 5");
  feed(t, "\x1b[10;20H\x1b[6n");
  CHECK(reply_is(t, "\x1b[10;20R"), "DSR 6");
  feed(t, "\x1b[c");
  CHECK(reply_is(t, "\x1b[?62;1;2;6;7;8;9c"), "DA");
  feed(t, "\x1b[0c");
  CHECK(reply_is(t, "\x1b[?62;1;2;6;7;8;9c"), "DA 0");
  feed(t, "\x1b[>c");
  CHECK(reply_is(t, "\x1b[>1;10;0c"), "DA2");
  feed(t, "\x1b[5;20r\x1b[?6h\x1b[2;3H\x1b[6n");
  CHECK(reply_is(t, "\x1b[2;3R"), "CPR relative to the origin");
  CUR(t, 2, 5);
  feed(t, "\x1b[?6n");
  CHECK(reply_is(t, "\x1b[?2;3R"), "DECXCPR");
  feed(t, "\x1b[?6l\x1b[r");
  feed(t, "\x1b[18t");
  CHECK(reply_is(t, "\x1b[8;24;80t"), "window size report");
  feed(t, "\x1b[?2004$p");
  CHECK(reply_is(t, "\x1b[?2004;2$y"), "DECRQM");
  feed(t, "\x1b[?2004h\x1b[?2004$p");
  CHECK(reply_is(t, "\x1b[?2004;1$y"), "DECRQM after set");
  feed(t, "\x1b[99n\x1b[>5;1m");
  CHECK(reply_is(t, ""), "no reply to unknown requests");
  vt_free(t);
}

static void test_save_restore(void) {
  vt *t = vt_new(20, 5, 0);
  feed(t, "\x1b[3;4H\x1b[1;31m\x1b" "7\x1b[H\x1b[0m\x1b" "8X");
  CUR(t, 4, 2);
  CHECK(C(t, 3, 2).attr == VT_BOLD && C(t, 3, 2).fg == VT_PAL(1), "DECRC restores the pen");
  feed(t, "\x1b[2;2H\x1b[s\x1b[5;5H\x1b[uY");
  CUR(t, 2, 1);
  ROW(t, 1, " Y");
  feed(t, "\x1b[H\x1b" "8");
  CUR(t, 1, 1); /* ESC 8 and CSI u share one slot: the last save was CSI s at 2;2 */
  vt_free(t);
}

static void test_rep_tabs(void) {
  vt *t = vt_new(30, 3, 0);
  feed(t, "ab\x1b[3b");
  ROW(t, 0, "abbbb");
  feed(t, "\r\n\x1b[b");
  ROW(t, 1, "b"); /* REP repeats the last printed character */
  feed(t, "\x1b[3g\r\t");
  CUR(t, 29, 1); /* all stops cleared */
  feed(t, "\r\x1b[5C\x1bH\x1b[12G\x1bH\r\t");
  CUR(t, 5, 1);
  feed(t, "\t");
  CUR(t, 11, 1);
  feed(t, "\x1b[g\r\t\t");
  CUR(t, 29, 1); /* stop at 11 cleared by TBC 0 */
  feed(t, "\x1b" "c\x1b[2I");
  CUR(t, 16, 0);
  feed(t, "\x1b[Z");
  CUR(t, 8, 0);
  feed(t, "\x1b[5Z");
  CUR(t, 0, 0);
  vt_free(t);
}

static void test_modes(void) {
  vt *t = vt_new(10, 4, 50);
  feed(t, "\x1b[?1h");
  CHECK(t->appcursor, "DECCKM set");
  feed(t, "\x1b[?1l");
  CHECK(!t->appcursor, "DECCKM reset");
  feed(t, "\x1b[?25l");
  CHECK(!t->cursor_visible && !vt_cursor_in_view(t), "cursor hidden");
  feed(t, "\x1b[?25h");
  CHECK(t->cursor_visible, "cursor shown");
  feed(t, "\x1b[?12h\x1b[?12l");
  CHECK(1, "12 accepted");
  feed(t, "\x1b[?2004h");
  CHECK(t->bracketed_paste, "2004");
  feed(t, "\x1b[?1000h\x1b[?1006h");
  CHECK(t->mouse == 1000 && t->mouse_sgr, "mouse modes");
  feed(t, "\x1b[?1002h");
  CHECK(t->mouse == 1002, "drag mode");
  feed(t, "\x1b[?1002;1006l");
  CHECK(t->mouse == 0 && !t->mouse_sgr, "multiple modes in one sequence");
  /* alternate screen 1049 */
  feed(t, "\x1b[Hmain1\r\nmain2\x1b[1;31m");
  feed(t, "\x1b[?1049h");
  CHECK(t->alt_active, "alt active");
  ROW(t, 0, ""); ROW(t, 1, "");
  feed(t, "\x1b[0m\x1b[Halt");
  ROW(t, 0, "alt");
  feed(t, "\x1b[?1049l");
  CHECK(!t->alt_active, "back to main");
  ROW(t, 0, "main1"); ROW(t, 1, "main2"); CUR(t, 5, 1);
  CHECK(t->pen.fg == VT_PAL(1) && t->pen.attr == VT_BOLD, "1049 restores the pen");
  feed(t, "\x1b[?1049h");
  ROW(t, 0, ""); /* alt screen cleared on entry */
  feed(t, "\x1b[?1049l\x1b[0m");
  /* 47: no clear on entry */
  feed(t, "\x1b[?47hkeep\x1b[?47l\x1b[?47h");
  CHECK(!strncmp(row(t, 1), "     keep", 9), "47 keeps the alt contents (%s)", row(t, 1));
  feed(t, "\x1b[?47l");
  ROW(t, 0, "main1");
  /* 1047 clears the alt screen when leaving */
  feed(t, "\x1b[?1047hzz\x1b[?1047l\x1b[?47h");
  ROW(t, 1, "");
  feed(t, "\x1b[?47l");
  /* scrolling on the alt screen never feeds the scrollback */
  int sb = t->sb_len;
  feed(t, "\x1b[?1049h\n\n\n\n\n\n\x1b[?1049l");
  CHECK(t->sb_len == sb, "alt screen does not scroll into history");
  /* origin mode */
  feed(t, "\x1b[2;3r\x1b[?6h");
  CUR(t, 0, 1);
  feed(t, "\x1b[5;1H");
  CUR(t, 0, 2); /* clamped to the region */
  feed(t, "\x1b[?6l");
  CUR(t, 0, 0);
  feed(t, "\x1b[r");
  /* LNM */
  feed(t, "\x1b[2J\x1b[20h\x1b[1;4H\nA\x1b[20l");
  ROW(t, 1, "A");
  /* cursor style */
  feed(t, "\x1b[5 q");
  CHECK(t->cursor_style == 5, "DECSCUSR");
  /* DECSTR soft reset */
  feed(t, "\x1b[?7l\x1b[4h\x1b[1m\x1b[!p");
  CHECK(t->autowrap && !t->insert && t->pen.attr == 0, "DECSTR");
  vt_free(t);
}

static void test_charset(void) {
  vt *t = vt_new(20, 3, 0);
  feed(t, "\x1b(0lqqk\x1b(B");
  CHECK(C(t, 0, 0).ch == 0x250c && C(t, 1, 0).ch == 0x2500 && C(t, 3, 0).ch == 0x2510, "DEC graphics");
  feed(t, "lq");
  CHECK(C(t, 4, 0).ch == 'l', "back to ASCII");
  feed(t, "\x1b)0\x0ex\x0fx");
  CHECK(C(t, 6, 0).ch == 0x2502 && C(t, 7, 0).ch == 'x', "G1 via SO/SI");
  feed(t, "\x1b*0\x1b+B");
  CHECK(1, "G2/G3 designations accepted");
  feed(t, "\x1b(0\x1b" "7\x1b(B\x1b" "8q\x1b(B");
  CHECK(C(t, 8, 0).ch == 0x2500, "DECSC saves the charset");
  feed(t, "\x1b(0" "ABC\x1b(B");
  CHECK(C(t, 9, 0).ch == 'A', "upper case unaffected by DEC graphics");
  vt_free(t);
}

static void test_esc(void) {
  vt *t = vt_new(10, 3, 0);
  feed(t, "\x1b[1;31mhello\x1b]0;title\x07\x1b[?1049h\x1b" "c");
  CHECK(!t->alt_active && t->pen.attr == 0 && t->x == 0 && t->y == 0, "RIS");
  ROW(t, 0, "");
  CHECK(t->title[0] == 0, "RIS clears the title");
  feed(t, "\x1b#8");
  ROW(t, 0, "EEEEEEEEEE"); ROW(t, 2, "EEEEEEEEEE");
  feed(t, "\x1b" "c\x1b%G\x1b P\x1b" "6\x1b" "9ok");
  ROW(t, 0, "ok");
  vt_free(t);
}

static void test_osc(void) {
  vt *t = vt_new(20, 3, 0);
  feed(t, "\x1b]0;hello world\x07");
  CHECK(!strcmp(t->title, "hello world") && t->title_changed, "OSC 0 BEL");
  t->title_changed = 0;
  feed(t, "\x1b]2;second\x1b\\");
  CHECK(!strcmp(t->title, "second") && t->title_changed, "OSC 2 ST");
  t->title_changed = 0;
  feed(t, "\x1b]1;icon\x07\x1b]7;file:///x\x07\x1b]8;;http://a\x1b\\link\x1b]8;;\x1b\\");
  CHECK(!t->title_changed && !strcmp(t->title, "second"), "other OSC ignored");
  ROW(t, 0, "link");
  feed(t, "\x1b]10;?\x07\x1b]11;rgb:0/0/0\x1b\\");
  ROW(t, 0, "link");
  feed(t, "\x1b]2;caf\xc3\xa9 \xe2\x9c\x93\x07");
  CHECK(!strcmp(t->title, "caf\xc3\xa9 \xe2\x9c\x93"), "UTF-8 title");
  char big[3000];
  memset(big, 'x', sizeof big);
  memcpy(big, "\x1b]2;", 4);
  big[sizeof big - 1] = 7;
  vt_feed(t, big, sizeof big);
  CHECK(strlen(t->title) == 255, "long title truncated (%zu)", strlen(t->title));
  feed(t, "\x1b]2;bad\x18X");
  ROW(t, 0, "linkX"); /* CAN ends the OSC; then X prints */
  feed(t, "\x1b]");
  feed(t, "\x1b[2;1HY");
  ROW(t, 1, "Y"); /* ESC inside OSC starts a new sequence */
  vt_free(t);
}

static void test_dcs(void) {
  vt *t = vt_new(20, 3, 0);
  feed(t, "\x1bP1$r0m\x1b\\A");
  ROW(t, 0, "A");
  feed(t, "\x1bPq#0;2;0;0;0#0~~@@vv@@~~$-\x1b\\B");
  ROW(t, 0, "AB");
  feed(t, "\x1b_Gf=24;AAAA\x1b\\C\x1b^pm\x1b\\D\x1bXsos\x1b\\E");
  ROW(t, 0, "ABCDE");
  feed(t, "\x1bPabc\x07" "def\x1b\\F"); /* BEL does not end a DCS */
  ROW(t, 0, "ABCDEF");
  vt_free(t);
}

static void test_malformed(void) {
  vt *t = vt_new(20, 4, 0);
  feed(t, "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31;32;33;34;35;36;37mA");
  ROW(t, 0, "A");
  feed(t, "\x1b[99999999999999999999CB");
  CHECK(C(t, 19, 0).ch == 'B', "huge parameter clamps");
  feed(t, "\x1b[2K");
  feed(t, "\x1b[H\x1b[1;2\"?qC"); /* intermediate then parameter byte: ignored until final */
  ROW(t, 0, "C");
  feed(t, "\x1b[2;3?HD"); /* private marker in the middle: ignored */
  CUR(t, 2, 0);
  feed(t, "\x1b\x1b[2;1HE");
  ROW(t, 1, "E");
  feed(t, "\x1b[3;1\x18HF"); /* CAN aborts the CSI */
  ROW(t, 1, "EHF");
  feed(t, "\x1b[1\nB");
  CHECK(t->y == 3 && t->x == 3, "C0 inside CSI executes: %d,%d", t->x, t->y);
  feed(t, "\x1b[H\x1b[2\x1a" "AG");
  ROW(t, 0, "AG"); /* SUB aborts too: A and G print over row 0 */
  feed(t, "\x1b[\x7f" "1;1HH"); /* DEL inside CSI is ignored */
  ROW(t, 0, "HG");
  feed(t, "\x1b[?1;2;3;4;5\x80\xff" "h"); /* high bytes inside CSI dropped */
  feed(t, "\x1b(\x1b[HI");
  ROW(t, 0, "IG");
  feed(t, "\x1b[;;;;;;;H");
  CUR(t, 0, 0);
  feed(t, "\x1b[:::::m\x1b[38:2m\x1b[38:5m\x1b[38;9;1mJ");
  CHECK(C(t, 0, 0).ch == 'J', "odd SGR survives");
  feed(t, "\x1b[>0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9c");
  CHECK(1, "many DA2 params");
  feed(t, "\x1b[0K\x1b[9J\x1b[9K\x1b[9g");
  CHECK(1, "unknown erase modes");
  t->reply_len = 0;
  vt_free(t);
}

static void test_utf8(void) {
  vt *t = vt_new(20, 3, 0);
  feed(t, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z");
  CHECK(C(t, 0, 0).ch == 0xe9 && C(t, 1, 0).ch == 0x20ac && C(t, 2, 0).ch == 0x1f600 && C(t, 3, 0).ch == 'z', "valid UTF-8");
  feed(t, "\x1b[H\xc3(");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 1, 0).ch == '(', "truncated 2-byte");
  feed(t, "\x1b[H\xe2\x82(");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 1, 0).ch == '(', "truncated 3-byte");
  feed(t, "\x1b[H\xc0\xaf");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 1, 0).ch == 0xfffd, "overlong C0 AF");
  feed(t, "\x1b[H\xe0\x80\xaf");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 1, 0).ch == 0xfffd && C(t, 2, 0).ch == 0xfffd, "overlong E0");
  feed(t, "\x1b[H\xed\xa0\x80");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 1, 0).ch == 0xfffd && C(t, 2, 0).ch == 0xfffd, "surrogate");
  feed(t, "\x1b[H\xf4\x90\x80\x80");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 3, 0).ch == 0xfffd, "above U+10FFFF");
  feed(t, "\x1b[H\xff\xfe" "a");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 1, 0).ch == 0xfffd && C(t, 2, 0).ch == 'a', "FF FE");
  feed(t, "\x1b[H\x80\xbf" "b");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 1, 0).ch == 0xfffd && C(t, 2, 0).ch == 'b', "stray continuation bytes");
  feed(t, "\x1b[2J\x1b[H\xe2\x82\x1b[2;1Hq");
  CHECK(C(t, 0, 0).ch == 0xfffd && C(t, 0, 1).ch == 'q', "ESC interrupts a sequence");
  feed(t, "\x1b[H\xc2\x85x"); /* U+0085 (C1 NEL) is ignored */
  CHECK(C(t, 0, 0).ch == 'x', "C1 code point ignored");
  feed(t, "\x1b[H\xe2");
  feed(t, "\x82");
  feed(t, "\xac");
  CHECK(C(t, 0, 0).ch == 0x20ac, "sequence split across feeds");
  feed(t, "\x1b[2J\x1b[He\xcc\x81x"); /* combining acute dropped */
  ROW(t, 0, "ex");
  feed(t, "\x1b[2J\x1b[H\xe4\xb8\xad\xe6\x96\x87!"); /* wide characters take one cell */
  CHECK(C(t, 0, 0).ch == 0x4e2d && C(t, 1, 0).ch == 0x6587 && C(t, 2, 0).ch == '!', "CJK");
  vt_free(t);
}

static void test_scrollback(void) {
  vt *t = vt_new(10, 3, 5);
  for (int i = 0; i < 10; i++) { char b[16]; snprintf(b, sizeof b, "%sl%d", i ? "\r\n" : "", i); feed(t, b); }
  ROW(t, 0, "l7"); ROW(t, 2, "l9");
  CHECK(t->sb_len == 5, "scrollback capped at 5 (%d)", t->sb_len);
  vt_scroll_view(t, 1);
  ROW(t, 0, "l6"); ROW(t, 2, "l8");
  CHECK(!vt_cursor_in_view(t), "cursor not shown while scrolled back");
  vt_scroll_view(t, 100);
  CHECK(t->view == 5, "view clamped (%d)", t->view);
  ROW(t, 0, "l2"); ROW(t, 2, "l4");
  vt_scroll_view(t, -2);
  ROW(t, 0, "l4");
  vt_view_bottom(t);
  ROW(t, 0, "l7");
  CHECK(t->view == 0, "back to bottom");
  vt_free(t);
  t = vt_new(10, 3, 2000);
  for (int i = 0; i < 2500; i++) feed(t, "x\r\n");
  CHECK(t->sb_len == 2000, "2000 lines kept");
  vt_free(t);
}

static void test_resize(void) {
  vt *t = vt_new(10, 6, 100);
  for (int i = 0; i < 6; i++) { char b[16]; snprintf(b, sizeof b, "%sr%d", i ? "\r\n" : "", i); feed(t, b); }
  CUR(t, 2, 5);
  vt_resize(t, 10, 3);
  ROW(t, 0, "r3"); ROW(t, 2, "r5"); CUR(t, 2, 2);
  CHECK(t->sb_len == 3, "shrunk rows saved (%d)", t->sb_len);
  vt_resize(t, 10, 6);
  ROW(t, 0, "r0"); ROW(t, 5, "r5"); CUR(t, 2, 5);
  CHECK(t->sb_len == 0, "pulled back from scrollback");
  vt_resize(t, 4, 6);
  ROW(t, 5, "r5"); CUR(t, 2, 5);
  feed(t, "ab");
  CHECK(t->pending, "wrap at the new width");
  vt_resize(t, 20, 6);
  ROW(t, 5, "r5ab");
  feed(t, "\x1b[2J\x1b[Htop\r\n");
  vt_resize(t, 20, 3); /* cursor near the top: blank lines below are dropped, not the top */
  ROW(t, 0, "top"); CUR(t, 0, 1);
  /* alt screen resize keeps the main screen intact */
  feed(t, "\x1b[?1049h\x1b[Halt");
  vt_resize(t, 30, 8);
  ROW(t, 0, "alt");
  feed(t, "\x1b[?1049l");
  ROW(t, 0, "top");
  vt_resize(t, 1, 1);
  feed(t, "hello\r\nworld\x1b[5;5H\x1b[6n");
  CHECK(t->x == 0 && t->y == 0, "1x1 terminal");
  CHECK(reply_is(t, "\x1b[1;1R"), "CPR on 1x1");
  vt_resize(t, 0, -5);
  CHECK(t->cols == 1 && t->rows == 1, "bad sizes clamp");
  vt_free(t);
}

/* Feed a stream whole, byte by byte, and split at every position: the final
 * screens must be identical. */
static int same(vt *a, vt *b) {
  if (a->x != b->x || a->y != b->y || a->pending != b->pending || a->alt_active != b->alt_active) return 0;
  if (memcmp(&a->pen, &b->pen, sizeof a->pen) || strcmp(a->title, b->title) || a->reply_len != b->reply_len) return 0;
  if (memcmp(a->reply, b->reply, (size_t)a->reply_len)) return 0;
  for (int y = 0; y < a->rows; y++) {
    if (memcmp(a->main[y], b->main[y], sizeof(vt_cell) * (size_t)a->cols)) return 0;
    if (memcmp(a->alt[y], b->alt[y], sizeof(vt_cell) * (size_t)a->cols)) return 0;
  }
  return a->sb_len == b->sb_len;
}
static void test_split(void) {
  static const char stream[] =
    "\x1b[1;31mred\x1b[0m pl\xc3\xa9in \xe2\x82\xac \xf0\x9f\x98\x80\r\n"
    "\x1b[38;2;10;20;30mtc\x1b[48:5:200mbg\x1b[m\x1b]0;ti\xc3\xa9tle\x07\x1b]2;st\x1b\\"
    "\x1b[5;10H\x1b[6n\x1b[c\x1b(0lqk\x1b(B\x1b[2;4r\x1b[4;1H\n\n\x1bM\x1b[r"
    "\x1b[?1049h\x1b[Halt\x1b[?1049l\x1b" "7\x1b[3;3H\x1b" "8tail\x1bPdcs\x1b\\\x1b[3@\x1b[2P"
    "\xe2\x82\x1b[1;1H\xc3\x28\x1b[?25l\x1b[4hins\x1b[4l\x1b[2b\x1b[1;5;7;9m0123456789012345678901234567890\x1b[0m";
  int n = (int)sizeof stream - 1;
  vt *ref = vt_new(24, 8, 50);
  vt_feed(ref, stream, (size_t)n);
  int bad = 0;
  for (int k = 0; k <= n; k++) {
    vt *t = vt_new(24, 8, 50);
    vt_feed(t, stream, (size_t)k);
    vt_feed(t, stream + k, (size_t)(n - k));
    if (!same(ref, t)) { bad++; if (bad < 4) printf("  split at %d differs\n", k); }
    vt_free(t);
  }
  CHECK(bad == 0, "%d of %d split points differ", bad, n + 1);
  for (int k1 = 0; k1 <= n; k1 += 7) /* three pieces */
    for (int k2 = k1; k2 <= n; k2 += 5) {
      vt *t = vt_new(24, 8, 50);
      vt_feed(t, stream, (size_t)k1);
      vt_feed(t, stream + k1, (size_t)(k2 - k1));
      vt_feed(t, stream + k2, (size_t)(n - k2));
      if (!same(ref, t)) bad++;
      vt_free(t);
    }
  CHECK(bad == 0, "three-way splits differ: %d", bad);
  vt *t = vt_new(24, 8, 50);
  for (int i = 0; i < n; i++) vt_feed(t, stream + i, 1);
  CHECK(same(ref, t), "byte-by-byte feed matches");
  vt_free(t);
  vt_free(ref);
}

static void test_dirty(void) {
  vt *t = vt_new(10, 4, 10);
  memset(t->dirty, 0, 4); t->all_dirty = 0;
  feed(t, "\x1b[3;1Hx");
  CHECK(t->dirty[2] && !t->dirty[0] && !t->dirty[1] && !t->dirty[3], "only the written row is dirty");
  memset(t->dirty, 0, 4);
  feed(t, "\x1b[4;1H\n");
  CHECK(t->dirty[0] && t->dirty[3], "scroll marks every row");
  t->all_dirty = 0;
  vt_scroll_view(t, 1);
  CHECK(t->all_dirty, "view change marks everything");
  vt_free(t);
}

int main(void) {
  test_c0(); test_wrap(); test_cursor(); test_erase(); test_insert_delete(); test_scroll();
  test_sgr(); test_reports(); test_save_restore(); test_rep_tabs(); test_modes(); test_charset();
  test_esc(); test_osc(); test_dcs(); test_malformed(); test_utf8(); test_scrollback();
  test_resize(); test_split(); test_dirty();
  printf("test_vt: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
