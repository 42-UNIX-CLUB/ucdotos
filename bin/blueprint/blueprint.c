// SPDX-License-Identifier: GPL-2.0-or-later
/* blueprint: show text files as a slideshow in the terminal.
 * Written for the club pages in ~/unix_club: they are laid out by hand (ASCII art,
 * tables, "$ command" lines, "# comment" lines), so a slide is shown exactly as it is
 * in the file, never re-wrapped. Lines get a little colour by what they look like.
 *
 *   blueprint           ~/unix_club
 *   blueprint DIR          the files in DIR, then its subdirectories, in name order
 *   blueprint FILE...    these files, in this order
 *
 * One file is one slide. A line that is exactly "---" splits a file into more slides.
 * Keys follow the slides program (github.com/maaslalani/slides): next is space, Enter,
 * Right, Down, l, j, n, PgDn; back is Left, Up, h, k, p, N, PgUp; gg first, G last,
 * NUMBER G slide NUMBER, NUMBER before next/back moves that many; q or Ctrl-C quits.
 * A slide taller than the window scrolls before moving on. Files that change on disk
 * are read again. Output that is not a terminal gets every slide as plain text. */
#include <dirent.h>
#include <sys/stat.h>
#include "term.h"

/* --- the deck --- */
struct file { char *path, *label, *text; long long mtime, size; };
struct slide { int file, part, parts; char **line; int nlines; };
static struct file *files;
static int nfiles;
static struct slide *slides;
static int nslides, deck_width;

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) { tty_restore(); fprintf(stderr, "blueprint: out of memory\n"); exit(1); }
  return q;
}
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; return memcpy(xrealloc(0, n), s, n); }

/* Read a file. \r is dropped, a tab becomes 4 spaces (as in the original), other control
 * bytes become '?' so a file cannot send escape sequences to the terminal. */
static char *read_text(const char *path, long long *mtime, long long *size) {
  struct stat st;
  if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) return 0;
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  size_t len = 0, cap = 4096;
  char *s = xrealloc(0, cap);
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (len + 5 > cap) s = xrealloc(s, cap *= 2);
    if (c == '\r') continue;
    if (c == '\t') { memcpy(s + len, "    ", 4); len += 4; continue; }
    if (c < 0x20 && c != '\n') c = '?';
    if (c == 0x7f) c = '?';
    s[len++] = (char)c;
  }
  fclose(f);
  s[len] = 0;
  *mtime = st.st_mtime;
  *size = st.st_size;
  return s;
}

static void add_file(const char *path, const char *label) {
  files = xrealloc(files, (nfiles + 1) * sizeof *files);
  struct file *f = &files[nfiles++];
  f->path = xstrdup(path);
  f->label = xstrdup(label);
  f->text = 0;
  f->mtime = f->size = -1;
}

/* Text files only: names ending in .md, or with no dot at all (07_join_us/questions).
 * That leaves out intro.sh and pictures. */
static int wanted(const char *name) {
  if (name[0] == '.') return 0;
  const char *dot = strrchr(name, '.');
  return !dot || !strcmp(dot, ".md");
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* A directory: its files in name order, then each subdirectory the same way. */
static void add_dir(const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  if (!d) return;
  char **names = 0;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    names = xrealloc(names, (n + 1) * sizeof *names);
    names[n++] = xstrdup(e->d_name);
  }
  closedir(d);
  qsort(names, n, sizeof *names, cmp_str);
  char path[1024], label[1024];
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n; i++) {
      struct stat st;
      snprintf(path, sizeof path, "%s/%s", dir, names[i]);
      snprintf(label, sizeof label, "%s%s", prefix, names[i]);
      if (stat(path, &st) < 0) continue;
      if (pass == 0 && S_ISREG(st.st_mode) && wanted(names[i])) add_file(path, label);
      if (pass == 1 && S_ISDIR(st.st_mode)) {
        char sub[1100];
        snprintf(sub, sizeof sub, "%s/", label);
        add_dir(path, sub);
      }
    }
  for (int i = 0; i < n; i++) free(names[i]);
  free(names);
}

static int is_blank(const char *s) { while (*s == ' ') s++; return !*s; }

/* Cut every file into slides: at lines that are exactly "---", without blank lines at
 * the top and bottom of a slide. Empty slides are dropped. A file that cannot be read
 * becomes one slide that says so. */
static void build(void) {
  for (int i = 0; i < nslides; i++) free(slides[i].line);
  free(slides);
  slides = 0;
  nslides = 0;
  deck_width = 1;
  for (int fi = 0; fi < nfiles; fi++) {
    struct file *f = &files[fi];
    free(f->text); /* lines are cut in place, so every build reads the file again */
    f->text = read_text(f->path, &f->mtime, &f->size);
    if (!f->text) {
      f->text = xstrdup("(could not read this file)");
      f->mtime = f->size = -1;
    }
    /* split into lines, in place */
    char **line = 0;
    int nl = 0;
    char *p = f->text;
    for (;;) {
      char *nlp = strchr(p, '\n');
      line = xrealloc(line, (nl + 1) * sizeof *line);
      line[nl++] = p;
      if (!nlp) break;
      *nlp = 0;
      p = nlp + 1;
    }
    if (nl > 0 && !*line[nl - 1]) nl--; /* the file's last newline */
    int first = nslides, start = 0;
    for (int i = 0; i <= nl; i++) {
      if (i < nl && strcmp(line[i], "---")) continue;
      int a = start, b = i;
      while (a < b && is_blank(line[a])) a++;
      while (b > a && is_blank(line[b - 1])) b--;
      if (b > a) {
        slides = xrealloc(slides, (nslides + 1) * sizeof *slides);
        struct slide *s = &slides[nslides++];
        s->file = fi;
        s->nlines = b - a;
        s->line = xrealloc(0, s->nlines * sizeof *s->line);
        memcpy(s->line, line + a, s->nlines * sizeof *s->line);
        for (int k = 0; k < s->nlines; k++) {
          int w = u8_width(s->line[k]);
          if (w > deck_width) deck_width = w;
        }
      }
      start = i + 1;
    }
    free(line);
    for (int k = first; k < nslides; k++) { slides[k].part = k - first + 1; slides[k].parts = nslides - first; }
  }
}

/* A file changed on disk (or appeared, or went away): read the deck again. Returns 1 if so. */
static int reload_changed(void) {
  int changed = 0;
  for (int i = 0; i < nfiles; i++) {
    struct stat st;
    long long m = -1, z = -1;
    if (stat(files[i].path, &st) == 0) { m = st.st_mtime; z = st.st_size; }
    /* times are whole seconds here, so the size catches a second save in the same second */
    if (m != files[i].mtime || z != files[i].size) changed = 1;
  }
  if (changed) build();
  return changed;
}

/* --- drawing --- */
static int R = 24, C = 80;
static int page, scroll;

static int body_top(void) { return R >= 10 ? 1 : 0; }
static int body_rows(void) { int h = R - 1 - body_top() - (R >= 10 ? 1 : 0); return h < 1 ? 1 : h; }
static int max_scroll(void) { int m = slides[page].nlines - body_rows(); return m > 0 ? m : 0; }

static int is_box(const unsigned char *p) { /* U+2500..U+259F: box drawing and blocks */
  return p[0] == 0xe2 && (p[1] == 0x94 || p[1] == 0x95 || p[1] == 0x96);
}

/* Emit n bytes of s in colour fg (bold if b), stopping at *cols_left columns. */
static void put_run(const char *s, int n, unsigned fg, int bold, int *cols_left) {
  if (n <= 0 || *cols_left <= 0) return;
  ob_fg(fg);
  if (bold) ob_puts("\033[1m");
  const char *p = s, *end = s + n;
  while (p < end && *cols_left > 0) {
    int l = u8_len((unsigned char)*p);
    if (p + l > end) l = (int)(end - p);
    /* box drawing in its own colour, inside any run that is not bold */
    if (!bold && is_box((const unsigned char *)p)) {
      ob_fg(th->blue); ob_put(p, l); ob_fg(fg);
    } else ob_put(p, l);
    p += l;
    (*cols_left)--;
  }
  if (bold) ob_puts("\033[22m");
}

/* Where a "# comment" starts in s, or -1. Two spaces then '#' then a space or the end
 * ("$ date   # note", "│   # note"), or '#' as the first thing on an indented line. */
static int comment_at(const char *s, int from) {
  for (int i = from; s[i]; i++) {
    if (s[i] != '#' || (s[i + 1] && s[i + 1] != ' ')) continue;
    if (i == from && from > 0) return i;
    if (i >= 2 && s[i - 1] == ' ' && s[i - 2] == ' ') return i;
  }
  return -1;
}

/* One line of a slide, coloured by what it looks like. */
static void draw_text_line(const char *s, int cols) {
  int left = cols;
  int n = (int)strlen(s);
  if (s[0] == '#') { /* "# Heading" at the start of the line */
    int h = 0;
    while (s[h] == '#') h++;
    if (s[h] == ' ' || !s[h]) {
      put_run(s, h + (s[h] == ' '), th->orange, 1, &left);
      put_run(s + h + 1, n - h - 1 > 0 ? n - h - 1 : 0, th->yellow, 1, &left);
      return;
    }
  }
  if (!strncmp(s, "```", 3) || !strncmp(s, "~~~", 3)) { put_run(s, n, th->grey, 0, &left); return; }
  int i = 0;
  while (s[i] == ' ') i++;
  put_run(s, i, th->fg, 0, &left);
  int c = comment_at(s, i);
  int body_end = c >= 0 ? c : n;
  if (s[i] == '$' && s[i + 1] == ' ') { /* "$ command" */
    put_run(s + i, 1, th->green, 1, &left);
    i++;
    put_run(s + i, body_end - i, th->fg, 1, &left);
  } else if (i < body_end) {
    int m = 0; /* a list marker: "* ", "+ ", "- ", "> ", "[ ] ", "[x] ", or a number "01 " */
    if (strchr("*+->", s[i]) && s[i + 1] == ' ') m = 1;
    else if (s[i] == '[' && s[i + 1] && s[i + 2] == ']' && s[i + 3] == ' ') m = 3;
    else if (i > 0 && s[i] >= '0' && s[i] <= '9') {
      int k = i;
      while (k < body_end && s[k] >= '0' && s[k] <= '9') k++;
      if (k - i <= 2 && s[k] == ' ') m = k - i;
    }
    put_run(s + i, m, th->orange, 1, &left);
    /* "> " after two spaces is a marker too: the second column of bsd.md */
    int from = i + m;
    for (int j = from; j < body_end; j++)
      if (j >= 2 && s[j] == '>' && s[j + 1] == ' ' && s[j - 1] == ' ' && s[j - 2] == ' ') {
        put_run(s + from, j - from, th->fg, 0, &left);
        put_run(s + j, 1, th->orange, 1, &left);
        from = j + 1;
      }
    put_run(s + from, body_end - from, th->fg, 0, &left);
  }
  if (c >= 0) put_run(s + c, n - c, strstr(s + c, "UNIX_CLUB") ? th->aqua : th->grey, 0, &left);
}

static int in_code(const struct slide *s, int upto) { /* inside a ``` block at line upto? */
  int on = 0;
  for (int k = 0; k < upto; k++)
    if (!strncmp(s->line[k], "```", 3) || !strncmp(s->line[k], "~~~", 3)) on = !on;
  return on;
}

static char nav[16]; /* pending count or "g" */

static void draw(void) {
  const struct slide *s = &slides[page];
  int top = body_top(), rows = body_rows();
  int margin = C > deck_width ? (C - deck_width) / 2 : 0;
  ob_puts("\033[?25l\033[0m");
  ob_bg(th->bg);
  int code = in_code(s, scroll);
  for (int r = 0; r < R - 1; r++) {
    ob_goto(r, 0);
    ob_bg(th->bg);
    ob_puts("\033[K");
    int k = r - top + scroll;
    if (r < top || r >= top + rows || k >= s->nlines) continue;
    const char *ln = s->line[k];
    int fence = !strncmp(ln, "```", 3) || !strncmp(ln, "~~~", 3);
    ob_goto(r, margin);
    if (code && !fence) { int left = C - margin; put_run(ln, (int)strlen(ln), th->aqua, 0, &left); }
    else draw_text_line(ln, C - margin);
    if (fence) code = !code;
  }
  /* status bar: where we are on the left, the page on the right */
  char lbuf[300], rbuf[64];
  const struct file *f = &files[s->file];
  if (s->parts > 1) snprintf(lbuf, sizeof lbuf, " %s (%d/%d)", f->label, s->part, s->parts);
  else snprintf(lbuf, sizeof lbuf, " %s", f->label);
  if (scroll < max_scroll()) strncat(lbuf, "  (more below)", sizeof lbuf - strlen(lbuf) - 1);
  snprintf(rbuf, sizeof rbuf, "%s%sSlide %d / %d ", nav, nav[0] ? "  " : "", page + 1, nslides);
  int rw = u8_width(rbuf);
  ob_goto(R - 1, 0);
  ob_bg(th->bg1);
  ob_fg(th->grey);
  ob_puts("\033[K");
  ob_padded(lbuf, C - rw > 0 ? C - rw : 0, 0);
  if (rw <= C) { ob_fg(th->orange); ob_puts(rbuf); }
  ob_puts("\033[0m");
  ob_flush();
}

/* --- keys, as in the original's navigation.Navigate --- */
static int nav_count(void) { /* the pending number, -1 if none */
  if (!nav[0] || nav[0] < '0' || nav[0] > '9') return -1;
  return atoi(nav);
}
static void go(int p) {
  if (p < 0) p = 0;
  if (p > nslides - 1) p = nslides - 1;
  if (p != page) scroll = 0;
  page = p;
}
static void key(int k) {
  int n = nav_count();
  if (k >= '0' && k <= '9') {
    size_t l = n >= 0 ? strlen(nav) : 0;
    if (l < sizeof nav - 1) { nav[l] = (char)k; nav[l + 1] = 0; }
    return;
  }
  if (k == 'g') {
    if (!strcmp(nav, "g")) { nav[0] = 0; go(0); }
    else strcpy(nav, "g");
    return;
  }
  nav[0] = 0;
  int step = n > 0 ? n : 1;
  switch (k) {
  case 'G': go(n >= 0 ? n - 1 : nslides - 1); break;
  case ' ': case K_DOWN: case 'j': case K_RIGHT: case 'l': case '\r': case '\n': case 'n': case K_PGDN:
    if (n < 0 && scroll < max_scroll()) { /* show the rest of a tall slide first */
      scroll += body_rows() - 1 > 0 ? body_rows() - 1 : 1;
      if (scroll > max_scroll()) scroll = max_scroll();
    } else go(page + step);
    break;
  case K_UP: case 'k': case K_LEFT: case 'h': case 'p': case 'N': case K_PGUP:
    if (n < 0 && scroll > 0) {
      scroll -= body_rows() - 1 > 0 ? body_rows() - 1 : 1;
      if (scroll < 0) scroll = 0;
    } else go(page - step);
    break;
  case K_HOME: go(0); break;
  case K_END: go(nslides - 1); break;
  }
}

/* Not a terminal: every slide as plain text, for pipes and tests. */
static void dump(void) {
  for (int i = 0; i < nslides; i++) {
    const struct slide *s = &slides[i];
    printf("--- slide %d / %d: %s", i + 1, nslides, files[s->file].label);
    if (s->parts > 1) printf(" (%d/%d)", s->part, s->parts);
    printf("\n");
    for (int k = 0; k < s->nlines; k++) printf("%s\n", s->line[k]);
  }
}

static void usage(void) {
  fprintf(stderr, "usage: blueprint [--dump] [DIR | FILE...]\n"
                  "  with no argument: ~/unix_club\n");
}

int main(int argc, char **argv) {
  int want_dump = 0, nargs = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dump")) want_dump = 1;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
    else if (argv[i][0] == '-' && argv[i][1]) { usage(); return 2; }
    else nargs++;
  }
  char home_club[512];
  if (!nargs) {
    const char *home = getenv("HOME");
    snprintf(home_club, sizeof home_club, "%s/unix_club", home && *home ? home : "/home/user");
  }
  for (int i = 1; i <= argc; i++) {
    const char *a = i < argc ? argv[i] : (nargs ? 0 : home_club);
    if (!a || (i < argc && a[0] == '-' && a[1])) continue;
    struct stat st;
    if (stat(a, &st) < 0) { fprintf(stderr, "blueprint: %s: no such file or directory\n", a); return 1; }
    if (S_ISDIR(st.st_mode)) add_dir(a, "");
    else add_file(a, a);
    if (!nargs) break;
  }
  build();
  if (!nslides) { fprintf(stderr, "blueprint: nothing to show\n"); return 1; }

  theme_init();
  if (want_dump || !tty_check(0) || !tty_check(1)) { dump(); return 0; }
  tty_size(1, &R, &C);
  if (tty_raw(0) < 0) { dump(); return 0; }
  ob_puts("\033[?1049h");
  draw();
  double next_check = now_ms() + 1000;
  for (;;) {
    int k = key_read(200);
    if (k == -2 || k == 'q' || k == 3) break;
    int redraw = 0;
    if (k >= 0) { key(k); redraw = 1; }
    if (k == 12) redraw = 1; /* Ctrl-L */
    /* no resize signal here: look at the size and the desktop theme on every wake */
    int r, c;
    if (tty_size(1, &r, &c) == 0 && (r != R || c != C)) {
      R = r; C = c;
      if (scroll > max_scroll()) scroll = max_scroll();
      redraw = 1;
    }
    if (theme_reload()) redraw = 1;
    if (now_ms() >= next_check) {
      next_check = now_ms() + 1000;
      if (reload_changed()) {
        if (page > nslides - 1) page = nslides - 1;
        if (page < 0) page = 0;
        if (scroll > max_scroll()) scroll = max_scroll();
        redraw = 1;
      }
    }
    if (redraw && nslides) draw();
  }
  ob_puts("\033[0m\033[?25h\033[?1049l");
  ob_flush();
  tty_restore();
  return 0;
}
