// SPDX-License-Identifier: GPL-2.0-or-later
/* Pseudo-terminals with a Linux-style line discipline.
 * /dev/ptmx gives a master; the matching slave appears as /dev/pts/N.
 * A terminal emulator holds the master, the shell and its children hold the slave. */
#include "kernel.h"

#define NPTY 16
#define IN_SZ 4096
#define OUT_SZ 65536
struct pty {
  int used, index;
  struct termios tio;
  struct winsize ws;
  u8 in[IN_SZ];       /* input the slave can read */
  u32 in_head, in_len;
  u8 line[1024];      /* canonical line being edited */
  u32 line_len;
  int eof_pending;
  u8 *out;            /* output the master can read */
  u32 out_head, out_len;
  waitq in_wq, out_rq, out_wq;
  int fg_pgid;
  int master_open, slave_refs, slave_ever;
};
static struct pty ptys[NPTY];

static u32 out_room(struct pty *t) { return OUT_SZ - t->out_len; }
static void out_put(struct pty *t, u8 c) {
  if (t->out_len < OUT_SZ) { t->out[(t->out_head + t->out_len) % OUT_SZ] = c; t->out_len++; }
}
static void out_char(struct pty *t, u8 c) { /* output processing */
  if (c == '\n' && (t->tio.c_oflag & OPOST) && (t->tio.c_oflag & ONLCR)) out_put(t, '\r');
  out_put(t, c);
}
static void echo(struct pty *t, u8 c) {
  if (!(t->tio.c_lflag & ECHO)) return;
  if (c < 32 && c != '\n' && c != '\t') { out_put(t, '^'); out_put(t, c + 64); }
  else out_char(t, c);
}
static void in_put(struct pty *t, u8 c) {
  if (t->in_len < IN_SZ) { t->in[(t->in_head + t->in_len) % IN_SZ] = c; t->in_len++; }
}
static void line_commit(struct pty *t) {
  for (u32 i = 0; i < t->line_len; i++) in_put(t, t->line[i]);
  t->line_len = 0;
}
/* Ctrl-C and friends: kill the foreground job. The session leader (the shell) is not
 * killed; it gets a pending-interrupt flag that it reads with SYS_intr, so it can stop
 * a loop of its own. */
static void kill_foreground(struct pty *t, int sig) {
  for (int i = 1; i < NPROC; i++) {
    struct proc *p = &procs[i];
    if (p->state != P_RUN || p->pgid != t->fg_pgid) continue;
    if (p->sid == p->pid) { p->intr = 1; host_wake(p->pid); continue; }
    proc_exit(p, 256 + sig);
    host_wake(p->pid);
  }
}
/* The terminal went away: everything in its session dies, the shell included. */
static void hangup(struct pty *t) {
  /* The session is the one the foreground job belongs to. If that is also the session of
   * whoever is closing the terminal (no shell ever called setsid), only the job is killed. */
  int session = 0;
  for (int i = 1; i < NPROC && !session; i++)
    if (procs[i].state == P_RUN && procs[i].pgid == t->fg_pgid) session = procs[i].sid;
  if (!session || session == cur->sid) { kill_foreground(t, 1); return; }
  for (int i = 1; i < NPROC; i++) {
    struct proc *p = &procs[i];
    if (p->state != P_RUN || p->sid != session || p == cur) continue;
    proc_exit(p, 256 + 1 /* SIGHUP */);
    host_wake(p->pid);
  }
}
/* one byte typed at the terminal */
static void ldisc_input(struct pty *t, u8 c) {
  u32 lf = t->tio.c_lflag;
  const u8 *cc = t->tio.c_cc;
  if (c == '\r' && (t->tio.c_iflag & ICRNL)) c = '\n';
  if ((lf & ISIG) && (c == cc[VINTR] || c == cc[VQUIT])) {
    echo(t, c);
    if (lf & ECHO) out_char(t, '\n');
    t->line_len = 0;
    kill_foreground(t, c == cc[VINTR] ? SIGINT : 3);
    return;
  }
  if (!(lf & ICANON)) { in_put(t, c); echo(t, c); return; }
  if (c == cc[VERASE] || c == 8) {
    if (!t->line_len) return;
    do t->line_len--; while (t->line_len && (t->line[t->line_len] & 0xc0) == 0x80); /* whole UTF-8 character */
    if (lf & ECHO) { out_put(t, 8); out_put(t, ' '); out_put(t, 8); }
    return;
  }
  if (c == cc[VKILL]) {
    while (t->line_len) { t->line_len--; if ((lf & ECHO) && (t->line[t->line_len] & 0xc0) != 0x80) { out_put(t, 8); out_put(t, ' '); out_put(t, 8); } }
    return;
  }
  if (c == cc[VEOF]) {
    if (t->line_len) line_commit(t); else t->eof_pending = 1;
    return;
  }
  if (t->line_len < sizeof t->line - 1) t->line[t->line_len++] = c;
  echo(t, c);
  if (c == '\n') line_commit(t);
}

/* ---- slave side ---- */
static long pts_read(struct file *f, u32 ubuf, u32 len) {
  struct pty *t = f->priv;
  int canon = t->tio.c_lflag & ICANON;
  if (!t->in_len) {
    if (t->eof_pending) { t->eof_pending = 0; return 0; }
    if (!t->master_open) return 0;
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    if (!canon && t->tio.c_cc[VMIN] == 0) { /* timed read: VTIME tenths of a second */
      double now = host_now();
      if (!t->tio.c_cc[VTIME]) return 0;
      if (cur->sleep_until == 0) cur->sleep_until = now + t->tio.c_cc[VTIME] * 100.0;
      if (now >= cur->sleep_until) { cur->sleep_until = 0; return 0; }
      wq_add(&t->in_wq);
      return block(cur->sleep_until - now);
    }
    wq_add(&t->in_wq);
    return block(-1);
  }
  cur->sleep_until = 0;
  u8 tmp[512];
  u32 n = 0;
  while (n < len && n < sizeof tmp && t->in_len) {
    u8 c = t->in[t->in_head];
    t->in_head = (t->in_head + 1) % IN_SZ; t->in_len--;
    tmp[n++] = c;
    if (canon && c == '\n') break;
  }
  return copy_to_user(ubuf, tmp, n) ? -EFAULT : (long)n;
}
static long pts_write(struct file *f, u32 ubuf, u32 len) {
  struct pty *t = f->priv;
  if (!t->master_open) return -EIO;
  u8 tmp[512];
  u32 done = 0;
  while (done < len) {
    u32 n = len - done < sizeof tmp ? len - done : sizeof tmp;
    if (n * 2 > out_room(t)) n = out_room(t) / 2; /* worst case every byte is \n -> \r\n */
    if (!n) break;
    if (copy_from_user(tmp, ubuf + done, n)) return -EFAULT;
    for (u32 i = 0; i < n; i++) out_char(t, tmp[i]);
    done += n;
  }
  if (done) { wq_wake(&t->out_rq); return done; }
  if (f->flags & O_NONBLOCK) return -EAGAIN;
  wq_add(&t->out_wq);
  return block(-1);
}
static int pts_poll(struct file *f, int add) {
  struct pty *t = f->priv;
  int m = ((t->in_len || t->eof_pending) ? POLLIN : 0) | (out_room(t) >= 2 ? POLLOUT : 0) | (!t->master_open ? POLLHUP : 0);
  if (add) { if (!(m & (POLLIN | POLLHUP))) wq_add(&t->in_wq); if (!(m & POLLOUT)) wq_add(&t->out_wq); }
  return m;
}
static long tty_ioctl(struct file *f, u32 cmd, u32 arg) {
  struct pty *t = f->priv;
  switch (cmd) {
  case TCGETS: return copy_to_user(arg, &t->tio, sizeof t->tio);
  case TCSETS: case TCSETSW: case TCSETSF: {
    struct termios n;
    if (copy_from_user(&n, arg, sizeof n)) return -EFAULT;
    if ((t->tio.c_lflag & ICANON) && !(n.c_lflag & ICANON)) line_commit(t);
    if (cmd == TCSETSF) { t->in_len = 0; t->line_len = 0; }
    t->tio = n;
    if (t->in_len) wq_wake(&t->in_wq);
    return 0;
  }
  case TIOCGWINSZ: return copy_to_user(arg, &t->ws, sizeof t->ws);
  case TIOCSWINSZ: return copy_from_user(&t->ws, arg, sizeof t->ws);
  case TIOCGPGRP: return copy_to_user(arg, &t->fg_pgid, 4);
  case TIOCSPGRP: return copy_from_user(&t->fg_pgid, arg, 4);
  case 0x540B: t->in_len = 0; t->line_len = 0; return 0; /* TCFLSH */
  case 0x540A: case 0x5409: return 0;                     /* TCXONC, TCSBRK */
  case TIOCGPTN: return copy_to_user(arg, &t->index, 4);
  case 0x540E: return 0; /* TIOCSCTTY */
  }
  return -ENOTTY;
}
static void pty_release(struct pty *t) {
  if (t->master_open || t->slave_refs) return;
  kfree(t->out);
  t->out = NULL;
  t->used = 0;
}
static void pts_close(struct file *f) {
  struct pty *t = f->priv;
  t->slave_refs--;
  wq_wake(&t->out_rq); /* the master sees hangup when the last slave goes */
  pty_release(t);
}
static const struct fops pts_fops = {pts_read, pts_write, pts_poll, pts_close, tty_ioctl};

/* ---- master side ---- */
static long ptm_read(struct file *f, u32 ubuf, u32 len) {
  struct pty *t = f->priv;
  if (!t->out_len) {
    if (t->slave_ever && !t->slave_refs) return 0; /* everyone hung up */
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&t->out_rq);
    return block(-1);
  }
  if (len > t->out_len) len = t->out_len;
  u32 first = OUT_SZ - t->out_head < len ? OUT_SZ - t->out_head : len;
  if (copy_to_user(ubuf, t->out + t->out_head, first) || copy_to_user(ubuf + first, t->out, len - first)) return -EFAULT;
  t->out_head = (t->out_head + len) % OUT_SZ;
  t->out_len -= len;
  wq_wake(&t->out_wq);
  return len;
}
static long ptm_write(struct file *f, u32 ubuf, u32 len) {
  struct pty *t = f->priv;
  u8 tmp[256];
  for (u32 done = 0; done < len;) {
    u32 n = len - done < sizeof tmp ? len - done : sizeof tmp;
    if (copy_from_user(tmp, ubuf + done, n)) return -EFAULT;
    for (u32 i = 0; i < n; i++) ldisc_input(t, tmp[i]);
    done += n;
  }
  if (t->in_len || t->eof_pending) wq_wake(&t->in_wq);
  if (t->out_len) wq_wake(&t->out_rq); /* echo */
  return len;
}
static int ptm_poll(struct file *f, int add) {
  struct pty *t = f->priv;
  int m = POLLOUT | (t->out_len ? POLLIN : 0) | ((t->slave_ever && !t->slave_refs) ? POLLHUP : 0);
  if (add && !(m & (POLLIN | POLLHUP))) wq_add(&t->out_rq);
  return m;
}
static void ptm_close(struct file *f) {
  struct pty *t = f->priv;
  t->master_open = 0;
  hangup(t);
  wq_wake(&t->in_wq);
  wq_wake(&t->out_wq);
  pty_release(t);
}
static const struct fops ptm_fops = {ptm_read, ptm_write, ptm_poll, ptm_close, tty_ioctl};

struct file *pty_open_master(struct inode *ino, int flags) {
  for (int i = 0; i < NPTY; i++) {
    struct pty *t = &ptys[i];
    if (t->used) continue;
    memset(t, 0, sizeof *t);
    if (!(t->out = kmalloc(OUT_SZ))) return NULL;
    t->used = 1; t->index = i; t->master_open = 1;
    t->tio.c_iflag = ICRNL;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_cflag = 0xbf;
    t->tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    t->tio.c_cc[VINTR] = 3; t->tio.c_cc[VQUIT] = 28; t->tio.c_cc[VERASE] = 127;
    t->tio.c_cc[VKILL] = 21; t->tio.c_cc[VEOF] = 4; t->tio.c_cc[VMIN] = 1;
    t->ws.ws_row = 24; t->ws.ws_col = 80;
    char path[32];
    snprintf(path, sizeof path, "/dev/pts/%d", i);
    if (!vfs_lookup(vfs_root, path)) vfs_mkdev(path, DEV_PTS0 + i);
    return file_new(&ptm_fops, ino, flags, t);
  }
  return NULL;
}
struct file *pty_open_slave(struct inode *ino, int flags) {
  struct pty *t = &ptys[ino->dev - DEV_PTS0];
  if (!t->used || !t->master_open) return NULL;
  t->slave_refs++;
  t->slave_ever = 1;
  return file_new(&pts_fops, ino, flags, t);
}
void pty_init(void) {
  int err;
  vfs_create(vfs_root, "/dev/pts", S_IFDIR | 0755, &err);
  vfs_mkdev("/dev/ptmx", DEV_PTMX);
}
