// SPDX-License-Identifier: GPL-2.0-or-later
/* The Linux system call layer, part 1: dispatch, files, memory, time, identity.
 *
 * Guest pointers are 64-bit guest addresses; every buffer is range checked against the
 * guest window and then passed to the host as a host pointer, so data is not copied.
 * Structures whose layout differs (stat, timespec on wasm32, sysinfo, iovec) are
 * converted. Calls the host cannot do are emulated here when the emulation is truthful,
 * and otherwise return -ENOSYS (logged with --trace). */
#include "sys.h"
#include "host.h"
#include "path.h"
#include "sysnames.h"

uint64_t lx_sys_count[LX_NSYS];
static uint32_t guest_umask = 022;

const char *lx_sysname(int nr) {
  if (nr >= 0 && nr < (int)(sizeof lx_sysname_tab / sizeof lx_sysname_tab[0]) && lx_sysname_tab[nr]) return lx_sysname_tab[nr];
  return "?";
}

/* ---- identity ---- */
static int64_t uid_override = -1;
uint32_t lx_uid(void) {
  if (uid_override < 0) {
    const char *e = getenv("LX_UID");
    uid_override = e ? atoi(e) : hb0(LX_SYS_getuid);
    if (uid_override < 0) uid_override = 1000;
  }
  return (uint32_t)uid_override;
}
uint32_t lx_gid(void) { return lx_uid(); }

/* ---- descriptor paths ---- */
static char *fdpath[LX_NFD];
const char *fd_path_get(int fd) { return fd >= 0 && fd < LX_NFD ? fdpath[fd] : 0; }
void fd_path_set(int fd, const char *g) {
  if (fd < 0 || fd >= LX_NFD) return;
  free(fdpath[fd]);
  fdpath[fd] = g ? strdup(g) : 0;
}
void fd_path_dup(int from, int to) { fd_path_set(to, fd_path_get(from)); }
extern int lx_errfd;
void fd_close_cloexec(void) {
  for (int fd = 0; fd < LX_NFD; fd++) {
    if (fd == lx_errfd) continue;
    int64_t r = hb2(LX_SYS_fcntl, fd, 1 /* F_GETFD */);
    if (r < 0) { if (fdpath[fd]) fd_path_set(fd, 0); continue; }
    if (r & 1) { hb1(LX_SYS_close, fd); fd_path_set(fd, 0); }
  }
}

/* ---- helpers ---- */
#define GUARD(a, n) do { if (!g_okrange((a), (n))) return -EFAULT; } while (0)
#define HP(a) ((void *)g2h(a))

static int64_t gpath(uint64_t ga, char *buf) {
  int64_t r = g_strncpy(buf, ga, LX_PATH_MAX);
  return r < 0 ? r : 0;
}

/* Translate a guest (dirfd, path) to host form; handles the engine's own /proc entries. */
static int64_t xlate(int dirfd, uint64_t gp, int follow, char *hp, int *hd) {
  char g[LX_PATH_MAX];
  int64_t r = gpath(gp, g);
  if (r < 0) return r;
  if (!strcmp(g, "/proc/self/exe") && follow) return path_to_host(LX_AT_FDCWD, lx_exe_path, 1, hp, LX_PATH_MAX) < 0 ? -ENOENT : (*hd = LX_AT_FDCWD, 0);
  return path_xlate(dirfd, g, follow, hp, LX_PATH_MAX, hd);
}

static void put_stat(uint64_t ga, const struct lx_stat *s) {
  struct g_stat g;
  memset(&g, 0, sizeof g);
  g.st_dev = s->dev; g.st_ino = s->ino; g.st_mode = s->mode; g.st_nlink = (uint32_t)s->nlink;
  g.st_uid = s->uid; g.st_gid = s->gid; g.st_rdev = s->rdev; g.st_size = s->size;
  g.st_blksize = (int32_t)s->blksize; g.st_blocks = s->blocks;
  g.g_atime = s->atime; g.g_atime_nsec = s->atime_ns; g.g_mtime = s->mtime; g.g_mtime_nsec = s->mtime_ns;
  g.g_ctime = s->ctime; g.g_ctime_nsec = s->ctime_ns;
  memcpy(g2h(ga), &g, sizeof g);
}

static int64_t sys_fstatat(int dirfd, uint64_t gp, uint64_t gst, int flags) {
  GUARD(gst, sizeof(struct g_stat));
  struct lx_stat st;
  int64_t r;
  char hp[LX_PATH_MAX];
  int hd;
  char g[LX_PATH_MAX];
  r = gpath(gp, g);
  if (r < 0) return r;
  if (!g[0] && (flags & LX_AT_EMPTY_PATH)) r = hb_fstatat(dirfd, 0, &st, 0);
  else {
    r = xlate(dirfd, gp, !(flags & LX_AT_SYMLINK_NOFOLLOW), hp, &hd);
    if (r < 0) return r;
    r = hb_fstatat(hd, hp, &st, flags & LX_AT_SYMLINK_NOFOLLOW);
  }
  if (r < 0) return r;
  put_stat(gst, &st);
  return 0;
}

static int64_t sys_statx(int dirfd, uint64_t gp, int flags, unsigned mask, uint64_t gbuf) {
  (void)mask;
  GUARD(gbuf, sizeof(struct g_statx));
  struct lx_stat st;
  int64_t r;
  char g[LX_PATH_MAX], hp[LX_PATH_MAX];
  int hd;
  r = gpath(gp, g);
  if (r < 0) return r;
  if (!g[0] && (flags & LX_AT_EMPTY_PATH)) r = hb_fstatat(dirfd, 0, &st, 0);
  else {
    r = xlate(dirfd, gp, !(flags & LX_AT_SYMLINK_NOFOLLOW), hp, &hd);
    if (r < 0) return r;
    r = hb_fstatat(hd, hp, &st, flags & LX_AT_SYMLINK_NOFOLLOW);
  }
  if (r < 0) return r;
  struct g_statx x;
  memset(&x, 0, sizeof x);
  x.stx_mask = 0x7ff; /* STATX_BASIC_STATS */
  x.stx_blksize = (uint32_t)st.blksize; x.stx_nlink = (uint32_t)st.nlink; x.stx_uid = st.uid; x.stx_gid = st.gid;
  x.stx_mode = (uint16_t)st.mode; x.stx_ino = st.ino; x.stx_size = (uint64_t)st.size; x.stx_blocks = (uint64_t)st.blocks;
  x.stx_atime.tv_sec = st.atime; x.stx_atime.tv_nsec = (uint32_t)st.atime_ns;
  x.stx_mtime.tv_sec = st.mtime; x.stx_mtime.tv_nsec = (uint32_t)st.mtime_ns;
  x.stx_ctime.tv_sec = st.ctime; x.stx_ctime.tv_nsec = (uint32_t)st.ctime_ns;
  x.stx_btime = x.stx_ctime;
  x.stx_rdev_major = (uint32_t)((st.rdev >> 8) & 0xfff); x.stx_rdev_minor = (uint32_t)((st.rdev & 0xff) | ((st.rdev >> 12) & 0xfff00));
  x.stx_dev_major = (uint32_t)((st.dev >> 8) & 0xfff); x.stx_dev_minor = (uint32_t)((st.dev & 0xff) | ((st.dev >> 12) & 0xfff00));
  memcpy(g2h(gbuf), &x, sizeof x);
  return 0;
}

static int64_t sys_openat(int dirfd, uint64_t gp, int flags, int mode) {
  char g[LX_PATH_MAX], hp[LX_PATH_MAX];
  int64_t r = gpath(gp, g);
  if (r < 0) return r;
  int hd;
  r = xlate(dirfd, gp, !(flags & LX_O_NOFOLLOW), hp, &hd);
  if (r < 0) return r;
  if (flags & LX_O_CREAT) mode &= ~(int)guest_umask;
  r = hb4(LX_SYS_openat, hd, hbp(hp), flags, mode);
  if (r >= 0) {
    char abs[LX_PATH_MAX];
    if (path_resolve(dirfd, g, !(flags & LX_O_NOFOLLOW), abs, sizeof abs) == 0) fd_path_set((int)r, abs);
    else fd_path_set((int)r, 0);
  }
  return r;
}

static int64_t sys_readlinkat(int dirfd, uint64_t gp, uint64_t gbuf, uint64_t n) {
  char g[LX_PATH_MAX], hp[LX_PATH_MAX], out[LX_PATH_MAX];
  int64_t r = gpath(gp, g);
  if (r < 0) return r;
  if ((int64_t)n <= 0) return -EINVAL;
  GUARD(gbuf, n);
  const char *val = 0;
  if (!strcmp(g, "/proc/self/exe")) val = lx_exe_path;
  else if (!strncmp(g, "/proc/self/fd/", 14)) {
    val = fd_path_get(atoi(g + 14));
    if (!val) return -ENOENT;
  }
  if (val) {
    size_t l = strlen(val);
    if (l > n) l = n;
    memcpy(g2h(gbuf), val, l);
    return (int64_t)l;
  }
  int hd;
  r = xlate(dirfd, gp, 0, hp, &hd);
  if (r < 0) return r;
  r = hb4(LX_SYS_readlinkat, hd, hbp(hp), hbp(out), sizeof out);
  if (r < 0) return r;
  if ((uint64_t)r > n) r = (int64_t)n;
  memcpy(g2h(gbuf), out, (size_t)r);
  return r;
}

/* Calls of the form f(dirfd, path, x, y) where only the path needs translating. */
static int64_t path_call(int nr, int dirfd, uint64_t gp, int64_t x, int64_t y, int follow) {
  char hp[LX_PATH_MAX];
  int hd;
  int64_t r = xlate(dirfd, gp, follow, hp, &hd);
  if (r < 0) return r;
  return hb4(nr, hd, hbp(hp), x, y);
}

static int64_t sys_faccessat(int dirfd, uint64_t gp, int mode, int flags) {
  char hp[LX_PATH_MAX];
  int hd;
  int64_t r = xlate(dirfd, gp, !(flags & LX_AT_SYMLINK_NOFOLLOW), hp, &hd);
  if (r < 0) return r;
  r = hb4(LX_SYS_faccessat, hd, hbp(hp), mode, 0);
  if (r != -ENOSYS) return r;
  /* No faccessat on the host (UC.OS today): existence, and execute permission from the mode. */
  struct lx_stat st;
  r = hb_fstatat(hd, hp, &st, 0);
  if (r < 0) return r;
  if ((mode & 1) && !(st.mode & 0111) && (st.mode & LX_S_IFMT) != LX_S_IFDIR) return -EACCES;
  return 0;
}

static int64_t sys_renameat2(int ofd, uint64_t op, int nfd, uint64_t np, unsigned flags) {
  if (flags) return -EINVAL; /* RENAME_NOREPLACE, RENAME_EXCHANGE: not supported */
  char h1[LX_PATH_MAX], h2[LX_PATH_MAX];
  int d1, d2;
  int64_t r = xlate(ofd, op, 0, h1, &d1);
  if (r < 0) return r;
  r = xlate(nfd, np, 0, h2, &d2);
  if (r < 0) return r;
  return hb4(LX_SYS_renameat, d1, hbp(h1), d2, hbp(h2));
}

static int64_t sys_linkat(int ofd, uint64_t op, int nfd, uint64_t np, int flags) {
  char h1[LX_PATH_MAX], h2[LX_PATH_MAX];
  int d1, d2;
  int64_t r = xlate(ofd, op, (flags & LX_AT_SYMLINK_FOLLOW) != 0, h1, &d1);
  if (r < 0) return r;
  r = xlate(nfd, np, 0, h2, &d2);
  if (r < 0) return r;
  return hb_raw(LX_SYS_linkat, d1, hbp(h1), d2, hbp(h2), flags & ~LX_AT_SYMLINK_FOLLOW, 0);
}

static int64_t sys_symlinkat(uint64_t target, int nfd, uint64_t np) {
  char t[LX_PATH_MAX], h[LX_PATH_MAX];
  int d;
  int64_t r = gpath(target, t);
  if (r < 0) return r;
  r = xlate(nfd, np, 0, h, &d);
  if (r < 0) return r;
  /* The target text is stored as the guest wrote it; absolute targets are resolved inside
     the root when followed (path.c). */
  return hb3(LX_SYS_symlinkat, hbp(t), d, hbp(h));
}

static int64_t sys_chdir(uint64_t gp) {
  char g[LX_PATH_MAX], hp[LX_PATH_MAX], abs[LX_PATH_MAX];
  int64_t r = gpath(gp, g);
  if (r < 0) return r;
  if (!path_is_rooted()) return hb1(LX_SYS_chdir, hbp(g));
  r = path_resolve(LX_AT_FDCWD, g, 1, abs, sizeof abs);
  if (r < 0) return r;
  r = path_to_host(LX_AT_FDCWD, abs, 1, hp, sizeof hp);
  if (r < 0) return r;
  r = hb1(LX_SYS_chdir, hbp(hp));
  if (r == 0) path_set_cwd(abs);
  return r;
}
static int64_t sys_fchdir(int fd) {
  const char *g = fd_path_get(fd);
  if (!g) {
    if (!path_is_rooted()) return hb1(LX_SYS_fchdir, fd);
    return -EBADF;
  }
  char hp[LX_PATH_MAX];
  if (path_to_host(LX_AT_FDCWD, g, 1, hp, sizeof hp) < 0) return -ENOENT;
  int64_t r = hb1(LX_SYS_chdir, hbp(hp));
  if (r == 0 && path_is_rooted()) path_set_cwd(g);
  return r;
}
static int64_t sys_getcwd(uint64_t gbuf, uint64_t n) {
  char tmp[LX_PATH_MAX];
  const char *g;
  if (path_is_rooted()) g = path_cwd();
  else {
    int64_t r = hb2(LX_SYS_getcwd, hbp(tmp), sizeof tmp);
    if (r < 0) return r;
    g = tmp;
  }
  size_t l = strlen(g) + 1;
  if (l > n) return -ERANGE;
  GUARD(gbuf, l);
  memcpy(g2h(gbuf), g, l);
  return (int64_t)l;
}

static int64_t rw_vec(int write, int fd, uint64_t giov, uint64_t cnt, int64_t off) {
  if (cnt > 1024) return -EINVAL;
  GUARD(giov, cnt * sizeof(struct g_iovec));
  int64_t total = 0;
  for (uint64_t i = 0; i < cnt; i++) {
    struct g_iovec v;
    memcpy(&v, g2h(giov + i * sizeof v), sizeof v);
    if (!v.len) continue;
    if (!g_okrange(v.base, v.len)) return total ? total : -EFAULT;
    int64_t r;
    if (off < 0) r = hb3(write ? LX_SYS_write : LX_SYS_read, fd, hbp(g2h(v.base)), v.len);
    else r = hb4(write ? LX_SYS_pwrite64 : LX_SYS_pread64, fd, hbp(g2h(v.base)), v.len, off + total);
    if (r < 0) return total ? total : r;
    total += r;
    if ((uint64_t)r < v.len) break;
  }
  return total;
}

static int64_t sys_ppoll(uint64_t gfds, uint64_t n, uint64_t gts) {
  if (n > LX_NFD) return -EINVAL;
  if (n) GUARD(gfds, n * 8);
  int64_t ns = -1;
  if (gts) {
    GUARD(gts, 16);
    struct g_timespec ts;
    memcpy(&ts, g2h(gts), 16);
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
    ns = ts.tv_sec > 100000000 ? INT64_MAX : ts.tv_sec * 1000000000 + ts.tv_nsec;
  }
  return hb_ppoll(n ? g2h(gfds) : 0, (int)n, ns);
}

/* pselect6 on top of ppoll. fd_set is an array of 64-bit words, bit fd%64 of word fd/64. */
static int64_t sys_pselect6(int nfds, uint64_t rs, uint64_t ws, uint64_t es, uint64_t gts) {
  if (nfds < 0 || nfds > LX_NFD) return -EINVAL;
  uint64_t words = ((uint64_t)nfds + 63) / 64;
  uint64_t *set[3] = {0, 0, 0};
  uint64_t gs[3] = {rs, ws, es};
  for (int k = 0; k < 3; k++)
    if (gs[k]) { GUARD(gs[k], words * 8); set[k] = (uint64_t *)g2h(gs[k]); }
  struct { int fd; short ev, rev; } *pf = malloc(sizeof *pf * (size_t)(nfds ? nfds : 1));
  if (!pf) return -ENOMEM;
  int n = 0;
  for (int fd = 0; fd < nfds; fd++) {
    short ev = 0;
    uint64_t bit = 1ull << (fd & 63);
    uint64_t w;
    if (set[0]) { memcpy(&w, &set[0][fd / 64], 8); if (w & bit) ev |= 1; }
    if (set[1]) { memcpy(&w, &set[1][fd / 64], 8); if (w & bit) ev |= 4; }
    if (set[2]) { memcpy(&w, &set[2][fd / 64], 8); if (w & bit) ev |= 2; }
    if (ev) { pf[n].fd = fd; pf[n].ev = ev; pf[n].rev = 0; n++; }
  }
  int64_t ns = -1;
  if (gts) {
    GUARD(gts, 16);
    struct g_timespec ts;
    memcpy(&ts, g2h(gts), 16);
    ns = ts.tv_sec * 1000000000 + ts.tv_nsec;
  }
  int64_t r = hb_ppoll(pf, n, ns);
  if (r < 0) { free(pf); return r; }
  for (int k = 0; k < 3; k++) if (set[k]) memset(set[k], 0, words * 8);
  int64_t cnt = 0;
  for (int i = 0; i < n; i++) {
    int fd = pf[i].fd;
    uint64_t bit = 1ull << (fd & 63), w;
    short rv = pf[i].rev;
    if (rv & 0x20) { free(pf); return -EBADF; } /* POLLNVAL */
    if (set[0] && (pf[i].ev & 1) && (rv & (1 | 8 | 16))) { memcpy(&w, &set[0][fd / 64], 8); w |= bit; memcpy(&set[0][fd / 64], &w, 8); cnt++; }
    if (set[1] && (pf[i].ev & 4) && (rv & (4 | 8))) { memcpy(&w, &set[1][fd / 64], 8); w |= bit; memcpy(&set[1][fd / 64], &w, 8); cnt++; }
    if (set[2] && (pf[i].ev & 2) && (rv & 2)) { memcpy(&w, &set[2][fd / 64], 8); w |= bit; memcpy(&set[2][fd / 64], &w, 8); cnt++; }
  }
  free(pf);
  return cnt;
}

static int64_t sys_clock_gettime(int clk, uint64_t gts) {
  GUARD(gts, 16);
  int64_t s, ns;
  int64_t r = hb_clock_gettime(clk, &s, &ns);
  if (r < 0) return r;
  struct g_timespec ts = {s, ns};
  memcpy(g2h(gts), &ts, 16);
  return 0;
}

static int64_t sys_nanosleep(uint64_t greq, uint64_t grem) {
  GUARD(greq, 16);
  struct g_timespec ts;
  memcpy(&ts, g2h(greq), 16);
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
  int64_t r = hb_nanosleep(ts.tv_sec, ts.tv_nsec);
  if (grem && g_okrange(grem, 16)) memset(g2h(grem), 0, 16);
  return r;
}

static int64_t sys_uname(uint64_t gbuf) {
  GUARD(gbuf, sizeof(struct g_utsname));
  struct g_utsname u, h;
  memset(&u, 0, sizeof u);
  int64_t r = hb1(LX_SYS_uname, hbp(&h));
  /* What a guest sees (an assumption for the owners, see docs/LINUX-BINARIES.md): a
     Linux kernel of the version Debian 13 ships, the guest architecture, the host's
     node name, and the engine named in the version string. */
  strcpy(u.sysname, "Linux");
  strcpy(u.nodename, r == 0 ? h.nodename : "localhost");
  strcpy(u.release, "6.12.0-" LX_NAME);
  snprintf(u.version, sizeof u.version, "#1 SMP %s Linux binary engine on %s", LX_NAME, r == 0 ? h.sysname : "?");
  strcpy(u.machine, arch_cur->uname_machine);
  strcpy(u.domainname, "(none)");
  memcpy(g2h(gbuf), &u, sizeof u);
  return 0;
}

static int64_t sys_sysinfo(uint64_t gbuf) {
  GUARD(gbuf, sizeof(struct g_sysinfo));
  struct lx_sysinfo s;
  int64_t r = hb_sysinfo(&s);
  if (r < 0) return r;
  struct g_sysinfo g;
  memset(&g, 0, sizeof g);
  g.uptime = s.uptime; g.totalram = s.totalram; g.freeram = s.freeram; g.bufferram = s.bufferram;
  g.procs = (uint16_t)s.procs; g.mem_unit = 1;
  memcpy(g2h(gbuf), &g, sizeof g);
  return 0;
}

static int64_t sys_prlimit(int pid, unsigned res, uint64_t gnew, uint64_t gold) {
  (void)pid; (void)gnew;
  if (res >= 16) return -EINVAL;
  if (gold) {
    GUARD(gold, 16);
    struct g_rlimit l = {~0ull, ~0ull};
    if (res == 3) { l.cur = 8ull << 20; }                 /* RLIMIT_STACK: what the loader maps */
    else if (res == 7) { l.cur = l.max = LX_NFD; }        /* RLIMIT_NOFILE */
    else if (res == 4) { l.cur = 0; }                     /* RLIMIT_CORE */
    memcpy(g2h(gold), &l, 16);
  }
  return 0;
}

static int64_t sys_times(uint64_t gbuf) {
  int64_t s, ns;
  hb_clock_gettime(2, &s, &ns); /* process CPU time */
  int64_t ticks = s * 100 + ns / 10000000;
  if (gbuf) {
    GUARD(gbuf, sizeof(struct g_tms));
    struct g_tms t = {ticks, 0, 0, 0};
    memcpy(g2h(gbuf), &t, sizeof t);
  }
  hb_clock_gettime(1, &s, &ns);
  return s * 100 + ns / 10000000;
}

static int64_t sys_getrusage(int who, uint64_t gbuf) {
  (void)who;
  GUARD(gbuf, sizeof(struct g_rusage));
  struct g_rusage r;
  memset(&r, 0, sizeof r);
  int64_t s, ns;
  if (who == 0 && hb_clock_gettime(2, &s, &ns) == 0) { r.ru_utime.tv_sec = s; r.ru_utime.tv_usec = ns / 1000; }
  memcpy(g2h(gbuf), &r, sizeof r);
  return 0;
}

static int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off) {
  int64_t r = mem_map(addr, len, prot, flags, (flags & LX_MAP_ANON) ? -1 : fd, off);
  if (r >= 0 && (prot & LX_PROT_EXEC)) arch_cur->flush_code((uint64_t)r, len);
  return r;
}

static int64_t sys_madvise(uint64_t a, uint64_t len, int advice) {
  if (a & (LX_PAGE - 1)) return -EINVAL;
  if (advice == 4 /* MADV_DONTNEED */ || advice == 8 /* MADV_FREE: we may zero too */) {
    uint64_t l = (len + LX_PAGE - 1) & ~(LX_PAGE - 1);
    if (!mem_is_mapped(a, l)) return -ENOMEM;
    memset(g2h(a), 0, l); /* private anonymous memory reads back as zero, as on Linux */
  }
  return 0;
}

static int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
  switch (cmd) {
  case 1030: { /* F_DUPFD_CLOEXEC */
    int64_t r = hb3(LX_SYS_fcntl, fd, 1030, arg);
    if (r == -EINVAL) { r = hb3(LX_SYS_fcntl, fd, 0, arg); if (r >= 0) hb3(LX_SYS_fcntl, r, 2, 1); }
    if (r >= 0) fd_path_dup(fd, (int)r);
    return r;
  }
  case 0: { int64_t r = hb3(LX_SYS_fcntl, fd, 0, arg); if (r >= 0) fd_path_dup(fd, (int)r); return r; }
  case 5: case 6: case 7: case 36: case 37: case 38: { /* record locks: one process, so every lock is granted */
    int64_t r = hb2(LX_SYS_fcntl, fd, 1);
    if (r < 0) return r;
    if ((cmd == 5 || cmd == 36) && g_okrange(arg, 2)) { uint16_t unlck = 2; memcpy(g2h(arg), &unlck, 2); } /* F_GETLK: l_type = F_UNLCK */
    return 0;
  }
  case 1024: case 1025: return 0;      /* F_SETLEASE, F_GETLEASE */
  case 1031: case 1032: return 0x10000; /* F_SETPIPE_SZ, F_GETPIPE_SZ */
  case 1033: case 1034: return 0;      /* seals */
  }
  return hb3(LX_SYS_fcntl, fd, cmd, arg);
}

static int64_t sys_ioctl(int fd, unsigned long req, uint64_t arg) {
  switch (req) {
  case 0x5401: case 0x5402: case 0x5403: case 0x5404: /* TCGETS TCSETS TCSETSW TCSETSF: struct termios is 36 bytes everywhere */
    GUARD(arg, 36); return hb3(LX_SYS_ioctl, fd, req, hbp(g2h(arg)));
  case 0x5413: case 0x5414: GUARD(arg, 8); return hb3(LX_SYS_ioctl, fd, req, hbp(g2h(arg))); /* TIOC[GS]WINSZ */
  case 0x5410: if (lx_in_emulated_child()) return 0; /* TIOCSPGRP from an emulated child: the parent does it too */
    GUARD(arg, 4); return hb3(LX_SYS_ioctl, fd, req, hbp(g2h(arg)));
  case 0x540f: case 0x541b: case 0x5429: /* TIOCGPGRP FIONREAD TIOCGSID */
    GUARD(arg, 4); return hb3(LX_SYS_ioctl, fd, req, hbp(g2h(arg)));
  case 0x5421: return 0; /* FIONBIO: handled by fcntl in practice */
  case 0x540e: return 0; /* TIOCSCTTY */
  case 0x5422: return 0; /* TIOCNOTTY */
  case 0x5451: case 0x5450: return hb3(LX_SYS_fcntl, fd, 2, req == 0x5451); /* FIOCLEX FIONCLEX */
  }
  /* Other requests: pass through only if they take no pointer we would have to translate. */
  if (((req >> 30) & 3) == 0 && arg == 0) return hb3(LX_SYS_ioctl, fd, req, 0);
  return -ENOTTY;
}

static int64_t sys_memfd_create(uint64_t gname, unsigned flags) {
  char name[256], hp[LX_PATH_MAX];
  if (g_strncpy(name, gname, sizeof name) < 0) return -EFAULT;
  for (int i = 0; i < 100; i++) {
    uint32_t rnd;
    hb3(LX_SYS_getrandom, hbp(&rnd), 4, 0);
    snprintf(hp, sizeof hp, "/tmp/.%s-memfd-%08x", LX_NAME, rnd);
    int64_t fd = hb4(LX_SYS_openat, LX_AT_FDCWD, hbp(hp), 2 | LX_O_CREAT | LX_O_EXCL | ((flags & 1) ? LX_O_CLOEXEC : 0), 0600);
    if (fd == -EEXIST) continue;
    if (fd < 0) return fd;
    hb3(LX_SYS_unlinkat, LX_AT_FDCWD, hbp(hp), 0);
    char g[300];
    snprintf(g, sizeof g, "/memfd:%s", name);
    fd_path_set((int)fd, g);
    return fd;
  }
  return -EEXIST;
}

static int64_t copy_loop(int out, int in, uint64_t goff, uint64_t count) {
  char buf[16384];
  int64_t off = -1, total = 0;
  if (goff) { GUARD(goff, 8); memcpy(&off, g2h(goff), 8); }
  while ((uint64_t)total < count) {
    uint64_t want = count - (uint64_t)total < sizeof buf ? count - (uint64_t)total : sizeof buf;
    int64_t r = off >= 0 ? hb4(LX_SYS_pread64, in, hbp(buf), want, off) : hb3(LX_SYS_read, in, hbp(buf), want);
    if (r <= 0) { if (r < 0 && !total) return r; break; }
    int64_t w = 0;
    while (w < r) {
      int64_t k = hb3(LX_SYS_write, out, hbp(buf + w), r - w);
      if (k <= 0) { if (off >= 0) off += w; if (goff) memcpy(g2h(goff), &off, 8); return total + w ? total + w : k; }
      w += k;
    }
    total += r;
    if (off >= 0) off += r;
    if ((uint64_t)r < want) break;
  }
  if (goff) memcpy(g2h(goff), &off, 8);
  return total;
}

static int64_t sys_statfs(uint64_t gbuf) {
  GUARD(gbuf, 120);
  /* struct statfs (generic 64-bit): type, bsize, blocks, bfree, bavail, files, ffree, fsid,
     namelen, frsize, flags, spare. A RAM file system with plenty of room. */
  uint64_t v[15];
  memset(v, 0, sizeof v);
  v[0] = 0x01021994; /* TMPFS_MAGIC */
  v[1] = 4096; v[2] = 1u << 20; v[3] = 1u << 19; v[4] = 1u << 19; v[5] = 1u << 20; v[6] = 1u << 19;
  v[8] = 255; v[9] = 4096;
  memcpy(g2h(gbuf), v, 120);
  return 0;
}

static int64_t sys_sched_getaffinity(uint64_t size, uint64_t gmask) {
  if (size < 8 || (size & 7)) return -EINVAL;
  GUARD(gmask, size);
  memset(g2h(gmask), 0, size);
  *(uint8_t *)g2h(gmask) = 1; /* one CPU */
  return 8;
}

uint64_t lx_clear_tid;
int lx_child_exit(int code, int sig);
int lx_in_emulated_child(void);
/* An emulated fork child runs while its parent is stopped, so it must not wait for data
 * the parent would write. bash, for one, makes its child read a pipe until the parent
 * closes it (PGRP_PIPE): the end-of-file it waits for is what the child gets. */
static int child_read_would_block(int fd) {
  struct lx_stat st;
  if (hb_fstatat(fd, 0, &st, 0) < 0 || (st.mode & LX_S_IFMT) != 0010000 /* S_IFIFO */) return 0;
  struct { int fd; short ev, rev; } p = {fd, 1, 0};
  return hb_ppoll(&p, 1, 0) == 0;
}

/* The dispatcher. Returns the result; *how says whether to write it back. */
static int64_t dispatch(int nr, uint64_t *a, int *how) {
  switch (nr) {
  /* files */
  case LX_SYS_read:
    GUARD(a[1], a[2]);
    if (lx_in_emulated_child() && a[2] && child_read_would_block((int)a[0])) return 0;
    return hb3(LX_SYS_read, a[0], hbp(HP(a[1])), a[2]);
  case LX_SYS_write: {
    GUARD(a[1], a[2]);
    int64_t r = hb3(LX_SYS_write, a[0], hbp(HP(a[1])), a[2]);
    if (r == -EPIPE) sig_raise(LX_SIGPIPE);
    return r;
  }
  case LX_SYS_readv: return rw_vec(0, (int)a[0], a[1], a[2], -1);
  case LX_SYS_writev: { int64_t r = rw_vec(1, (int)a[0], a[1], a[2], -1); if (r == -EPIPE) sig_raise(LX_SIGPIPE); return r; }
  case LX_SYS_preadv: return rw_vec(0, (int)a[0], a[1], a[2], (int64_t)a[3]);
  case LX_SYS_pwritev: return rw_vec(1, (int)a[0], a[1], a[2], (int64_t)a[3]);
  case LX_SYS_pread64: GUARD(a[1], a[2]); return hb4(LX_SYS_pread64, a[0], hbp(HP(a[1])), a[2], a[3]);
  case LX_SYS_pwrite64: GUARD(a[1], a[2]); return hb4(LX_SYS_pwrite64, a[0], hbp(HP(a[1])), a[2], a[3]);
  case LX_SYS_openat: return sys_openat((int)a[0], a[1], (int)a[2], (int)a[3]);
  case LX_SYS_openat2: {
    GUARD(a[2], 24);
    uint64_t how3[3];
    memcpy(how3, g2h(a[2]), 24);
    return sys_openat((int)a[0], a[1], (int)how3[0], (int)how3[1]);
  }
  case LX_SYS_close: { int64_t r = hb1(LX_SYS_close, a[0]); if (r == 0) fd_path_set((int)a[0], 0); return r; }
  case LX_SYS_close_range: {
    uint64_t hi = a[1] < LX_NFD ? a[1] : LX_NFD - 1;
    for (uint64_t fd = a[0]; fd <= hi; fd++) {
      if (a[2] & 4) hb3(LX_SYS_fcntl, fd, 2, 1); /* CLOSE_RANGE_CLOEXEC */
      else if (hb1(LX_SYS_close, fd) == 0) fd_path_set((int)fd, 0);
    }
    return 0;
  }
  case LX_SYS_lseek: return hb_lseek((int)a[0], (int64_t)a[1], (int)a[2]);
  case LX_SYS_getdents64: GUARD(a[1], a[2]); return hb3(LX_SYS_getdents64, a[0], hbp(HP(a[1])), a[2] > 65536 ? 65536 : a[2]);
  case LX_SYS_newfstatat: return sys_fstatat((int)a[0], a[1], a[2], (int)a[3]);
  case LX_SYS_fstat: {
    GUARD(a[1], sizeof(struct g_stat));
    struct lx_stat st;
    int64_t r = hb_fstatat((int)a[0], 0, &st, 0);
    if (r == 0) put_stat(a[1], &st);
    return r;
  }
  case LX_SYS_statx: return sys_statx((int)a[0], a[1], (int)a[2], (unsigned)a[3], a[4]);
  case LX_SYS_readlinkat: return sys_readlinkat((int)a[0], a[1], a[2], a[3]);
  case LX_SYS_faccessat: return sys_faccessat((int)a[0], a[1], (int)a[2], 0);
  case LX_SYS_faccessat2: return sys_faccessat((int)a[0], a[1], (int)a[2], (int)a[3]);
  case LX_SYS_mkdirat: return path_call(LX_SYS_mkdirat, (int)a[0], a[1], (int64_t)(a[2] & ~guest_umask), 0, 0);
  case LX_SYS_mknodat: return path_call(LX_SYS_mknodat, (int)a[0], a[1], (int64_t)a[2], (int64_t)a[3], 0);
  case LX_SYS_unlinkat: return path_call(LX_SYS_unlinkat, (int)a[0], a[1], (int64_t)a[2], 0, 0);
  case LX_SYS_renameat2: return sys_renameat2((int)a[0], a[1], (int)a[2], a[3], (unsigned)a[4]);
  case LX_SYS_linkat: return sys_linkat((int)a[0], a[1], (int)a[2], a[3], (int)a[4]);
  case LX_SYS_symlinkat: return sys_symlinkat(a[0], (int)a[1], a[2]);
  case LX_SYS_fchmodat: case LX_SYS_fchmodat2: return path_call(LX_SYS_fchmodat, (int)a[0], a[1], (int64_t)a[2], 0, 1);
  case LX_SYS_fchownat: {
    char hp[LX_PATH_MAX]; int hd;
    int64_t r = xlate((int)a[0], a[1], !(a[4] & LX_AT_SYMLINK_NOFOLLOW), hp, &hd);
    if (r < 0) return r;
    r = hb_raw(LX_SYS_fchownat, hd, hbp(hp), (int64_t)a[2], (int64_t)a[3], (int64_t)a[4], 0);
    return (r == -EPERM || r == -ENOSYS) && lx_uid() == 0 ? 0 : r; /* a guest root may chown; we cannot */
  }
  case LX_SYS_fchmod: return hb2(LX_SYS_fchmod, a[0], a[1]);
  case LX_SYS_fchown: { int64_t r = hb3(LX_SYS_fchown, a[0], a[1], a[2]); return (r == -EPERM || r == -ENOSYS) && lx_uid() == 0 ? 0 : r; }
  case LX_SYS_utimensat: {
    int64_t ts[4], *tp = 0;
    if (a[2]) { GUARD(a[2], 32); memcpy(ts, g2h(a[2]), 32); tp = ts; }
    if (!a[1]) return hb_utimensat((int)a[0], 0, tp, (int)a[3]);
    char hp[LX_PATH_MAX]; int hd;
    int64_t r = xlate((int)a[0], a[1], !(a[3] & LX_AT_SYMLINK_NOFOLLOW), hp, &hd);
    if (r < 0) return r;
    return hb_utimensat(hd, hp, tp, (int)a[3]);
  }
  case LX_SYS_truncate: {
    char hp[LX_PATH_MAX]; int hd;
    int64_t r = xlate(LX_AT_FDCWD, a[0], 1, hp, &hd);
    if (r < 0) return r;
    int64_t fd = hb4(LX_SYS_openat, hd, hbp(hp), 1 | LX_O_CLOEXEC, 0);
    if (fd < 0) return fd;
    r = hb2(LX_SYS_ftruncate, fd, a[1]);
    hb1(LX_SYS_close, fd);
    return r;
  }
  case LX_SYS_ftruncate: return hb2(LX_SYS_ftruncate, a[0], a[1]);
  case LX_SYS_fallocate: return -EOPNOTSUPP;
  case LX_SYS_fsync: case LX_SYS_fdatasync: { int64_t r = hb1(nr, a[0]); return r == -ENOSYS ? 0 : r; }
  case LX_SYS_sync: return 0;
  case LX_SYS_fadvise64: case LX_SYS_readahead: return 0;
  case LX_SYS_flock: return 0; /* one process: every lock is granted */
  case LX_SYS_dup: { int64_t r = hb1(LX_SYS_dup, a[0]); if (r >= 0) fd_path_dup((int)a[0], (int)r); return r; }
  case LX_SYS_dup3: { int64_t r = hb3(LX_SYS_dup3, a[0], a[1], a[2]); if (r >= 0) fd_path_dup((int)a[0], (int)r); return r; }
  case LX_SYS_fcntl: return sys_fcntl((int)a[0], (int)a[1], a[2]);
  case LX_SYS_ioctl: return sys_ioctl((int)a[0], (unsigned long)a[1], a[2]);
  case LX_SYS_pipe2: {
    GUARD(a[0], 8);
    int fds[2];
    int64_t r = hb2(LX_SYS_pipe2, hbp(fds), a[1]);
    if (r == 0) { memcpy(g2h(a[0]), fds, 8); fd_path_set(fds[0], "pipe:"); fd_path_set(fds[1], "pipe:"); }
    return r;
  }
  case LX_SYS_ppoll: return sys_ppoll(a[0], a[1], a[2]);
  case LX_SYS_pselect6: return sys_pselect6((int)a[0], a[1], a[2], a[3], a[4]);
  case LX_SYS_chdir: return sys_chdir(a[0]);
  case LX_SYS_fchdir: return sys_fchdir((int)a[0]);
  case LX_SYS_getcwd: return sys_getcwd(a[0], a[1]);
  case LX_SYS_umask: { uint32_t o = guest_umask; guest_umask = (uint32_t)a[0] & 0777; return o; }
  case LX_SYS_statfs: case LX_SYS_fstatfs: return sys_statfs(a[1]);
  case LX_SYS_memfd_create: return sys_memfd_create(a[0], (unsigned)a[1]);
  case LX_SYS_sendfile: return copy_loop((int)a[0], (int)a[1], a[2], a[3]);
  case LX_SYS_copy_file_range: return copy_loop((int)a[2], (int)a[0], a[1], a[4]);

  /* memory */
  case LX_SYS_brk: return mem_brk(a[0]);
  case LX_SYS_mmap: return sys_mmap(a[0], a[1], (int)a[2], (int)a[3], (int)a[4], (int64_t)a[5]);
  case LX_SYS_munmap: arch_cur->flush_code(a[0], a[1]); return mem_unmap(a[0], a[1]);
  case LX_SYS_mprotect: { int64_t r = mem_protect(a[0], a[1], (int)a[2]); if (a[2] & LX_PROT_EXEC) arch_cur->flush_code(a[0], a[1]); return r; }
  case LX_SYS_mremap: arch_cur->flush_code(a[0], a[1]); return mem_remap(a[0], a[1], a[2], (int)a[3], a[4]);
  case LX_SYS_madvise: return sys_madvise(a[0], a[1], (int)a[2]);
  case LX_SYS_msync: return mem_sync(a[0], a[1]);
  case LX_SYS_mlock: case LX_SYS_munlock: case LX_SYS_mlockall: case LX_SYS_munlockall: return 0;
  case LX_SYS_mincore: {
    uint64_t pages = (a[1] + LX_PAGE - 1) / LX_PAGE;
    GUARD(a[2], pages);
    if (!mem_is_mapped(a[0], a[1])) return -ENOMEM;
    memset(g2h(a[2]), 1, pages);
    return 0;
  }

  /* time */
  case LX_SYS_clock_gettime: return sys_clock_gettime((int)a[0], a[1]);
  case LX_SYS_clock_getres: if (a[1]) { GUARD(a[1], 16); struct g_timespec r = {0, 1}; memcpy(g2h(a[1]), &r, 16); } return 0;
  case LX_SYS_gettimeofday:
    if (a[0]) {
      GUARD(a[0], 16);
      int64_t s, ns;
      hb_clock_gettime(0, &s, &ns);
      struct g_timeval tv = {s, ns / 1000};
      memcpy(g2h(a[0]), &tv, 16);
    }
    if (a[1]) { GUARD(a[1], 8); memset(g2h(a[1]), 0, 8); }
    return 0;
  case LX_SYS_nanosleep: return sys_nanosleep(a[0], a[1]);
  case LX_SYS_clock_nanosleep: {
    if (a[1] & 1) { /* TIMER_ABSTIME */
      GUARD(a[2], 16);
      struct g_timespec t;
      memcpy(&t, g2h(a[2]), 16);
      int64_t s, ns;
      hb_clock_gettime((int)a[0], &s, &ns);
      int64_t d = (t.tv_sec - s) * 1000000000 + (t.tv_nsec - ns);
      if (d <= 0) return 0;
      return hb_nanosleep(d / 1000000000, d % 1000000000);
    }
    return sys_nanosleep(a[2], a[3]);
  }
  case LX_SYS_times: return sys_times(a[0]);
  case LX_SYS_getrusage: return sys_getrusage((int)a[0], a[1]);
  case LX_SYS_getitimer: if (a[1]) { GUARD(a[1], 32); memset(g2h(a[1]), 0, 32); } return 0;
  case LX_SYS_setitimer: if (a[2]) { GUARD(a[2], 32); memset(g2h(a[2]), 0, 32); } return a[1] ? -ENOSYS : 0;

  /* identity and system */
  case LX_SYS_getpid: return hb0(LX_SYS_getpid);
  case LX_SYS_gettid: return lx_gettid();
  case LX_SYS_getppid: return hb0(LX_SYS_getppid);
  case LX_SYS_getuid: case LX_SYS_geteuid: return lx_uid();
  case LX_SYS_getgid: case LX_SYS_getegid: return lx_gid();
  case LX_SYS_getresuid: case LX_SYS_getresgid:
    for (int i = 0; i < 3; i++) { GUARD(a[i], 4); uint32_t v = lx_uid(); memcpy(g2h(a[i]), &v, 4); }
    return 0;
  case LX_SYS_setuid: case LX_SYS_setgid: case LX_SYS_setreuid: case LX_SYS_setregid:
  case LX_SYS_setresuid: case LX_SYS_setresgid: case LX_SYS_setfsuid: case LX_SYS_setfsgid:
    return lx_uid() == 0 ? 0 : -EPERM;
  case LX_SYS_getgroups: return 0;
  case LX_SYS_setgroups: return lx_uid() == 0 ? 0 : -EPERM;
  case LX_SYS_capget: if (a[1]) { GUARD(a[1], 24); memset(g2h(a[1]), 0, 24); } return 0;
  case LX_SYS_getpgid: return hb1(LX_SYS_getpgid, a[0]);
  case LX_SYS_setpgid:
    if (lx_in_emulated_child() && (a[0] == 0 || (int64_t)a[0] == hb0(LX_SYS_getpid))) return 0; /* the parent sets the child's group */
    return hb2(LX_SYS_setpgid, a[0], a[1]);
  case LX_SYS_getsid: { int64_t r = hb1(LX_SYS_getsid, a[0]); return r == -ENOSYS ? hb1(LX_SYS_getpgid, 0) : r; }
  case LX_SYS_setsid: return lx_in_emulated_child() ? hb0(LX_SYS_getpid) : hb0(LX_SYS_setsid);
  case LX_SYS_uname: return sys_uname(a[0]);
  case LX_SYS_sethostname: return -EPERM;
  case LX_SYS_sysinfo: return sys_sysinfo(a[0]);
  case LX_SYS_getrandom: GUARD(a[0], a[1]); return hb3(LX_SYS_getrandom, hbp(HP(a[0])), a[1] > 33554431 ? 33554431 : a[1], a[2] & ~4ull);
  case LX_SYS_prlimit64: return sys_prlimit((int)a[0], (unsigned)a[1], a[2], a[3]);
  case LX_SYS_getrlimit: return sys_prlimit(0, (unsigned)a[0], 0, a[1]);
  case LX_SYS_setrlimit: return 0;
  case LX_SYS_prctl:
    switch (a[0]) {
    case 15: return 0; /* PR_SET_NAME */
    case 16: GUARD(a[1], 16); memset(g2h(a[1]), 0, 16); strcpy((char *)g2h(a[1]), LX_NAME); return 0;
    case 1: case 2: case 4: case 3: case 36: case 37: case 38: case 39: return 0; /* PDEATHSIG, DUMPABLE, SUBREAPER, NO_NEW_PRIVS */
    }
    return -EINVAL;
  case LX_SYS_sched_getaffinity: return sys_sched_getaffinity(a[1], a[2]);
  case LX_SYS_sched_setaffinity: return 0;
  case LX_SYS_sched_yield: return hb0(LX_SYS_sched_yield);
  case LX_SYS_sched_getscheduler: case LX_SYS_sched_getparam: case LX_SYS_sched_setscheduler: case LX_SYS_sched_setparam: return 0;
  case LX_SYS_sched_get_priority_max: case LX_SYS_sched_get_priority_min: return 0;
  case LX_SYS_getpriority: return 20;
  case LX_SYS_setpriority: return 0;
  case LX_SYS_getcpu:
    if (a[0]) { GUARD(a[0], 4); memset(g2h(a[0]), 0, 4); }
    if (a[1]) { GUARD(a[1], 4); memset(g2h(a[1]), 0, 4); }
    return 0;
  case LX_SYS_set_tid_address: lx_clear_tid = a[0]; return hb0(LX_SYS_getpid);
  case LX_SYS_set_robust_list: return 0;
  case LX_SYS_rseq: return -ENOSYS;
  case LX_SYS_membarrier: return a[0] == 0 ? 0 : 0;
  case LX_SYS_futex: { int noret = 0; int64_t r = lx_futex(a, &noret); if (noret) *how = SR_NORET; return r; }
  case LX_SYS_seccomp: return -EINVAL;
  case LX_SYS_exit:
    if (lx_thread_exit()) { *how = SR_NORET; return 0; } /* one thread of several */
    __attribute__((fallthrough)); /* the last thread ends the process */
  case LX_SYS_exit_group:
    if (lx_child_exit((int)a[0], 0)) { *how = SR_NORET; return 0; }
    mem_reset(); /* writes back shared file mappings */
    if (lx_cfg.stats) lx_print_stats();
    hb_exit((int)a[0] & 0xff);

  /* signals and processes */
  case LX_SYS_rt_sigaction: return sys_rt_sigaction(a);
  case LX_SYS_rt_sigprocmask: return sys_rt_sigprocmask(a);
  case LX_SYS_sigaltstack: return sys_sigaltstack(a);
  case LX_SYS_rt_sigreturn: *how = SR_NORET; return sys_rt_sigreturn();
  case LX_SYS_rt_sigsuspend: { int64_t r = sys_rt_sigsuspend(a); if (r == LX_SIG_DELIVERED) *how = SR_NORET; return r; }
  case LX_SYS_rt_sigpending: return sys_rt_sigpending(a);
  case LX_SYS_kill: case LX_SYS_tkill: case LX_SYS_tgkill: return sys_kill(a, nr);
  case LX_SYS_clone: return sys_clone(a);
  case LX_SYS_clone3: return sys_clone3(a);
  case LX_SYS_execve: { int noret = 0; int64_t r = sys_execve(a, &noret); if (noret) *how = SR_NORET; return r; }
  case LX_SYS_wait4: return sys_wait4(a);
  case LX_SYS_waitid: return sys_waitid(a);

  /* sockets: AF_UNIX only on UC.OS; the address structures have the same layout */
  case LX_SYS_socket: return hb3(LX_SYS_socket, a[0], a[1], a[2]);
  case LX_SYS_bind: case LX_SYS_connect: GUARD(a[1], a[2]); return hb3(nr, a[0], hbp(HP(a[1])), a[2]);
  case LX_SYS_listen: return hb2(LX_SYS_listen, a[0], a[1]);
  case LX_SYS_accept: case LX_SYS_accept4: {
    if (a[1]) return -EINVAL;
    int64_t r = hb3(LX_SYS_accept, a[0], 0, 0);
    if (r >= 0 && nr == LX_SYS_accept4 && (a[3] & LX_O_CLOEXEC)) hb3(LX_SYS_fcntl, r, 2, 1);
    return r;
  }
  case LX_SYS_socketpair: case LX_SYS_getsockname: case LX_SYS_getpeername: case LX_SYS_sendto:
  case LX_SYS_recvfrom: case LX_SYS_setsockopt: case LX_SYS_getsockopt: case LX_SYS_shutdown:
  case LX_SYS_sendmsg: case LX_SYS_recvmsg:
    return -ENOSYS;
  case LX_SYS_eventfd2: case LX_SYS_epoll_create1: case LX_SYS_epoll_ctl: case LX_SYS_epoll_pwait:
  case LX_SYS_epoll_pwait2: case LX_SYS_signalfd4: case LX_SYS_inotify_init1: case LX_SYS_pidfd_open:
    return -ENOSYS;
  case LX_SYS_getxattr: case LX_SYS_lgetxattr: case LX_SYS_fgetxattr: return -ENODATA;
  case LX_SYS_listxattr: case LX_SYS_llistxattr: case LX_SYS_flistxattr: return 0;
  }
  int64_t r = arch_cur->sys_arch(nr, a);
  return r;
}

static void trace_call(int nr, const uint64_t *a, int64_t r) {
  fprintf(stderr, "[%s %d] %s(%llx, %llx, %llx, %llx) = %lld\n", LX_NAME, (int)hb0(LX_SYS_getpid), lx_sysname(nr),
          (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3], (long long)r);
}

void lx_syscall(void) {
  int nr = arch_cur->sys_nr();
  uint64_t a[6];
  arch_cur->sys_args(a);
  if (nr >= 0 && nr < LX_NSYS) lx_sys_count[nr]++;
  int how = SR_RET;
  if (lx_cfg.trace && nr == LX_SYS_exit_group) trace_call(nr, a, 0);
  int64_t r = dispatch(nr, a, &how);
  if (lx_cfg.trace) trace_call(nr, a, r);
  if (how == SR_NORET) return;
  if (r == -EINTR && sig_restart_wanted()) return; /* re-run the call after the handler (SA_RESTART) */
  arch_cur->sys_ret(r);
  arch_cur->sys_skip();
}

extern uint64_t rv_blocks_built, rv_flushes;
void rv_prof_dump(void);
int lx_errfd = 2; /* the engine's own copy of stderr: guests may close fd 2 before exiting */
void lx_print_stats(void) {
  rv_prof_dump();
  static char buf[16384];
  int n = snprintf(buf, sizeof buf, "%s: %llu guest instructions, %llu blocks decoded, %llu code flushes\n%s: system calls made by the guest:\n",
                   LX_NAME, (unsigned long long)arch_cur->get_instret(), (unsigned long long)rv_blocks_built,
                   (unsigned long long)rv_flushes, LX_NAME);
  for (int i = 0; i < LX_NSYS && n < (int)sizeof buf - 64; i++)
    if (lx_sys_count[i]) n += snprintf(buf + n, sizeof buf - (size_t)n, "  %-20s %3d %10llu\n", lx_sysname(i), i, (unsigned long long)lx_sys_count[i]);
  hb3(LX_SYS_write, lx_errfd, hbp(buf), n);
}
