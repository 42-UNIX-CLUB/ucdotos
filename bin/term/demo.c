// SPDX-License-Identifier: GPL-2.0-or-later
/* demo.c - small programs built into term for testing it inside the OS
 * (`term --demo NAME`). They live in the term binary because a program is a
 * directory under bin/ and only bin/term is ours.
 *   shell         a tiny job-control shell (used when /bin/sh is missing)
 *   flood [secs]  print numbered lines as fast as possible for secs (default 1)
 *   sleep         block forever (to be killed with Ctrl-C)
 *   colors        16, 256 and truecolor palettes, attributes, box drawing
 *   keys          raw mode: show the bytes every key sends (q quits)
 *   anim [secs]   truecolor half-block animation at 30 frames per second
 *   size          print the window size */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static double ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}
static void out(const char *s) { write(1, s, strlen(s)); }
static void console(const char *s) { /* the host log, for test scripts */
  int fd = open("/dev/console", O_WRONLY);
  if (fd >= 0) { write(fd, s, strlen(s)); close(fd); }
}

static int shell(void) {
  char line[512], msg[600];
  int me = getpid();
  __syscall0(SYS_setsid);
  __syscall3(SYS_ioctl, 0, TIOCSPGRP, &me);
  for (;;) {
    out("\x1b[1;32mdemo\x1b[0m:\x1b[1;34m~\x1b[0m$ ");
    long n = read(0, line, sizeof line - 1);
    if (n <= 0) break;
    line[n] = 0;
    if (n && line[n - 1] == '\n') line[--n] = 0;
    snprintf(msg, sizeof msg, "[demo-shell] line: %s\n", line);
    console(msg);
    char *argv[32];
    int argc = 0;
    for (char *p = strtok(line, " \t"); p && argc < 31; p = strtok(NULL, " \t")) argv[argc++] = p;
    argv[argc] = NULL;
    if (!argc) continue;
    if (!strcmp(argv[0], "exit")) return argc > 1 ? atoi(argv[1]) : 0;
    char path[256];
    if (strchr(argv[0], '/')) snprintf(path, sizeof path, "%s", argv[0]);
    else snprintf(path, sizeof path, "/bin/%s", argv[0]);
    int pid = spawn(path, argv, NULL, NULL);
    if (pid < 0) { snprintf(msg, sizeof msg, "%s: command not found\n", argv[0]); out(msg); continue; }
    __syscall2(SYS_setpgid, pid, pid);
    __syscall3(SYS_ioctl, 0, TIOCSPGRP, &pid);
    int status = 0;
    waitpid(pid, &status, 0);
    __syscall3(SYS_ioctl, 0, TIOCSPGRP, &me);
    snprintf(msg, sizeof msg, "[demo-shell] %s exited, status %d signal %d\n", argv[0], WEXITSTATUS(status), WTERMSIG(status));
    console(msg);
  }
  return 0;
}

static int flood(double secs) {
  char buf[8192];
  double t0 = ms();
  long lines = 0, bytes = 0;
  while (ms() - t0 < secs * 1000) {
    int n = 0;
    for (int i = 0; i < 64; i++, lines++)
      n += snprintf(buf + n, sizeof buf - n, "\x1b[3%ldmflood line %ld\x1b[0m the quick brown fox jumps over the lazy dog\n", 1 + lines % 7, lines);
    write(1, buf, (size_t)n);
    bytes += n;
  }
  char msg[128];
  snprintf(msg, sizeof msg, "[demo-flood] %ld lines, %ld bytes in %.0f ms\n", lines, bytes, ms() - t0);
  console(msg);
  return 0;
}

static int colors(void) {
  char b[128];
  out("16 colours:\n");
  for (int i = 0; i < 16; i++) { snprintf(b, sizeof b, "\x1b[%dm %2d ", i < 8 ? 40 + i : 92 + i, i); out(b); }
  out("\x1b[0m\n256 colours:\n");
  for (int i = 0; i < 256; i++) { snprintf(b, sizeof b, "\x1b[48;5;%dm  %s", i, (i + 1) % 32 ? "" : "\x1b[0m\n"); out(b); }
  out("truecolor:\n");
  for (int x = 0; x < 64; x++) { snprintf(b, sizeof b, "\x1b[48;2;%d;%d;%dm ", x * 4, 255 - x * 4, 128); out(b); }
  out("\x1b[0m\n\x1b[1mbold\x1b[0m \x1b[2mdim\x1b[0m \x1b[4munderline\x1b[0m \x1b[9mstrike\x1b[0m \x1b[7minverse\x1b[0m\n");
  out("\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90 \x1b(0lqqk\x1b(B\n\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98 \x1b(0mqqj\x1b(B\n");
  return 0;
}

static int keys(void) {
  struct termios t, saved;
  __syscall3(SYS_ioctl, 0, TCGETS, &t);
  saved = t;
  t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
  t.c_iflag &= ~ICRNL;
  __syscall3(SYS_ioctl, 0, TCSETS, &t);
  out("press keys, q quits\r\n");
  for (;;) {
    unsigned char c[64];
    long n = read(0, c, sizeof c);
    if (n <= 0 || (n == 1 && c[0] == 'q')) break;
    char line[512], msg[600];
    int k = 0;
    for (long i = 0; i < n && k < 400; i++)
      k += c[i] < 32 || c[i] >= 127 ? snprintf(line + k, sizeof line - k, "\\x%02x", c[i]) : snprintf(line + k, sizeof line - k, "%c", c[i]);
    snprintf(msg, sizeof msg, "key: %s\r\n", line);
    out(msg);
    snprintf(msg, sizeof msg, "[demo-keys] %s\n", line);
    console(msg);
  }
  __syscall3(SYS_ioctl, 0, TCSETS, &saved);
  return 0;
}

static int anim(double secs) {
  struct winsize ws = {24, 80, 0, 0};
  __syscall3(SYS_ioctl, 1, TIOCGWINSZ, &ws);
  int rows = ws.ws_row - 1, cols = ws.ws_col;
  size_t cap = (size_t)rows * cols * 40 + 64;
  char *buf = malloc(cap);
  if (!buf) return 1;
  out("\x1b[?25l\x1b[2J");
  double t0 = ms(), next = t0;
  int frames = 0;
  while (ms() - t0 < secs * 1000) {
    size_t n = (size_t)sprintf(buf, "\x1b[H");
    int f = frames * 4;
    for (int y = 0; y < rows; y++) {
      for (int x = 0; x < cols; x++) {
        int r1 = (x * 255 / cols + f) & 255, g1 = (2 * y) * 255 / (2 * rows), r2 = r1, g2 = (2 * y + 1) * 255 / (2 * rows);
        n += (size_t)sprintf(buf + n, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80", r1, g1, 255 - r1, r2, g2, 255 - r2);
      }
      n += (size_t)sprintf(buf + n, "\x1b[0m\r\n");
    }
    write(1, buf, n);
    frames++;
    next += 1000.0 / 30;
    double wait = next - ms();
    if (wait > 0) usleep((unsigned)(wait * 1000));
  }
  out("\x1b[0m\x1b[?25h\n");
  char msg[128];
  snprintf(msg, sizeof msg, "[demo-anim] %d frames in %.0f ms, %d bytes per frame\n", frames, ms() - t0, rows * cols * 38);
  console(msg);
  free(buf);
  return 0;
}

int demo_main(int argc, char **argv) {
  const char *n = argc > 1 ? argv[1] : "";
  double arg = argc > 2 ? atof(argv[2]) : 1;
  if (!strcmp(n, "shell")) return shell();
  if (!strcmp(n, "flood")) return flood(arg);
  if (!strcmp(n, "sleep")) { out("sleeping\n"); for (;;) sleep(100); }
  if (!strcmp(n, "colors")) return colors();
  if (!strcmp(n, "keys")) return keys();
  if (!strcmp(n, "anim")) return anim(argc > 2 ? arg : 3);
  if (!strcmp(n, "size")) {
    struct winsize ws = {0, 0, 0, 0};
    __syscall3(SYS_ioctl, 1, TIOCGWINSZ, &ws);
    char b[64];
    snprintf(b, sizeof b, "%d rows, %d cols\n", ws.ws_row, ws.ws_col);
    out(b);
    console("[demo-size] "); console(b);
    return 0;
  }
  fprintf(stderr, "term --demo: shell | flood [secs] | sleep | colors | keys | anim [secs] | size\n");
  return 2;
}
