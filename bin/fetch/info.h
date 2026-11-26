// SPDX-License-Identifier: GPL-2.0-or-later
/* info.h: system information behind one small interface (identical copies in fetch and htop).
 * On the OS it uses uname, sysinfo and the private SYS_procinfo call; natively it uses
 * uname(2), sysinfo(2) and /proc so the programs can be developed on Linux. */
#ifndef INFO_H
#define INFO_H
#pragma GCC diagnostic ignored "-Wunused-function"
#include <string.h>
#include <stdio.h>

struct sys_uname { char sysname[65], nodename[65], release[65], version[65], machine[65]; };
struct sys_stats {
  long uptime;          /* seconds */
  double total, used;   /* bytes: all memory the system holds, and the part in use */
  double kheap;         /* bytes of kernel heap in use */
  int procs;
};
struct pinfo {
  int pid, ppid, state; /* state: 'R' running, 'S' sleeping, 'Z' zombie, ... */
  double mem;           /* bytes */
  unsigned long long syscalls;
  unsigned long long cpu_us; /* microseconds spent running */
  char name[32];
};

#ifdef __wasm__
#include <sys/syscall.h>
static void info_uname(struct sys_uname *o) {
  struct utsname u;
  memset(o, 0, sizeof *o);
  if (__syscall1(SYS_uname, (long)&u) != 0) { strcpy(o->sysname, "unknown"); return; }
  memcpy(o->sysname, u.sysname, 65); memcpy(o->nodename, u.nodename, 65); memcpy(o->release, u.release, 65);
  memcpy(o->version, u.version, 65); memcpy(o->machine, u.machine, 65);
}
static void info_stats(struct sys_stats *o) {
  struct sysinfo si;
  memset(o, 0, sizeof *o);
  memset(&si, 0, sizeof si);
  if (__syscall1(SYS_sysinfo, (long)&si) != 0) return;
  double unit = si.mem_unit ? si.mem_unit : 1;
  o->uptime = si.uptime;
  o->total = si.totalram * unit;
  o->used = (si.totalram - si.freeram) * unit;
  o->kheap = si.bufferram * unit;
  o->procs = si.procs;
}
/* fill up to max entries; returns the count */
static int info_procs(struct pinfo *out, int max) {
  int n = 0;
  for (int i = 0; n < max; i++) {
    struct procinfo p;
    if (__syscall2(SYS_procinfo, i, (long)&p) != 0) break;
    out[n].pid = p.pid; out[n].ppid = p.ppid;
    out[n].state = p.state == 1 ? 'R' : p.state == 2 ? 'Z' : p.state == 3 ? 'S' : '?';
    out[n].mem = (double)p.mem_pages * 65536.0;
    out[n].syscalls = p.syscalls;
    out[n].cpu_us = p.cpu_us;
    memcpy(out[n].name, p.name, 32);
    out[n].name[31] = 0;
    n++;
  }
  return n;
}
static int info_kill(int pid, int sig) { return __syscall2(SYS_kill, pid, sig) == 0 ? 0 : -1; }
#else
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
static void info_uname(struct sys_uname *o) {
  struct utsname u;
  memset(o, 0, sizeof *o);
  if (uname(&u)) { strcpy(o->sysname, "unknown"); return; }
  snprintf(o->sysname, 65, "%s", u.sysname); snprintf(o->nodename, 65, "%s", u.nodename);
  snprintf(o->release, 65, "%s", u.release); snprintf(o->version, 65, "%s", u.version);
  snprintf(o->machine, 65, "%s", u.machine);
}
static void info_stats(struct sys_stats *o) {
  struct sysinfo si;
  memset(o, 0, sizeof *o);
  if (sysinfo(&si)) return;
  double unit = si.mem_unit ? si.mem_unit : 1;
  o->uptime = si.uptime;
  o->total = si.totalram * unit;
  o->used = (si.totalram - si.freeram - si.bufferram) * unit;
  o->kheap = si.bufferram * unit;
  o->procs = si.procs;
}
static int info_procs(struct pinfo *out, int max) {
  DIR *d = opendir("/proc");
  if (!d) return 0;
  int n = 0;
  struct dirent *e;
  while (n < max && (e = readdir(d))) {
    if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
    char path[300], buf[1024];
    snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp) continue;
    struct pinfo *p = &out[n];
    memset(p, 0, sizeof *p);
    p->pid = atoi(buf);
    size_t nl = rp - lp - 1 < 31 ? (size_t)(rp - lp - 1) : 31;
    memcpy(p->name, lp + 1, nl);
    char st = 0;
    long rss = 0, ut = 0, stt = 0;
    int ppid = 0;
    /* fields after the name: state ppid ... (rss is field 24) */
    char *s = rp + 2;
    st = *s;
    int field = 3;
    for (char *t = s; *t; t++)
      if (*t == ' ') {
        field++;
        if (field == 4) ppid = atoi(t + 1);
        if (field == 14) ut = atol(t + 1);
        if (field == 15) stt = atol(t + 1);
        if (field == 24) { rss = atol(t + 1); break; }
      }
    p->state = st; p->ppid = ppid;
    p->mem = (double)rss * 4096.0;
    p->cpu_us = (unsigned long long)(ut + stt) * (1000000ULL / (unsigned long long)sysconf(_SC_CLK_TCK));
    snprintf(path, sizeof path, "/proc/%s/io", e->d_name);
    if ((f = fopen(path, "r"))) { /* syscall counts: syscr + syscw (read and write calls) */
      char line[128];
      while (fgets(line, sizeof line, f))
        if (!strncmp(line, "syscr:", 6) || !strncmp(line, "syscw:", 6)) p->syscalls += strtoull(line + 6, 0, 10);
      fclose(f);
    }
    n++;
  }
  closedir(d);
  return n;
}
static int info_kill(int pid, int sig) { return kill(pid, sig); }
#endif
#endif
