// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel.h"

struct file *dev_open(struct inode *i, int flags) {
  if (i->dev == DEV_PTMX) return pty_open_master(i, flags);
  if (i->dev >= DEV_PTS0) return pty_open_slave(i, flags);
  if (i->dev == DEV_FB) return fb_open(i, flags);
  if (i->dev == DEV_INPUT) return ev_open(i, flags);
  const struct fops *ops = dev_fops(i->dev);
  return ops ? file_new(ops, i, flags, NULL) : NULL;
}
EXPORT(boot_alloc) void *boot_alloc(u32 len) { return kmalloc(len); }
EXPORT(proc_state) int proc_state(int pid) { return procs[pid].state; }

/* Entry point. The boot loader has copied the initrd (a tar archive) into our memory. */
EXPORT(kmain) int kmain(u8 *initrd, u32 initrd_len, const char *cmdline) {
  const char *init = "/bin/init";
  static char init_buf[PATH_MAX];
  for (const char *c = cmdline; c && *c; c++) /* kernel command line: init=/path */
    if (!strncmp(c, "init=", 5) && (c == cmdline || c[-1] == ' ')) {
      u32 n = 0;
      for (c += 5; *c && *c != ' ' && n < PATH_MAX - 1; c++) init_buf[n++] = *c;
      init_buf[n] = 0; init = init_buf;
      if (!*c) break;
    }
  double t0 = host_now();
  vfs_init();
  pty_init();
  fb_init();
  int files = vfs_load_tar(initrd, initrd_len);
  kprintf("[kernel] %s %s (wasm32), initrd %u KiB, %d files, up in %.2f ms\n", OS_NAME, OS_RELEASE,
          initrd_len >> 10, files, host_now() - t0);
  u8 *args = kmalloc(16);
  u32 counts[2] = {1, 0};
  memcpy(args, counts, 8);
  memcpy(args + 8, "init", 5);
  int pid = proc_spawn(NULL, init, args, 13, NULL);
  if (pid < 0) { kprintf("[kernel] cannot start %s: error %d\n", init, pid); return pid; }
  struct inode *con = vfs_lookup(vfs_root, "/dev/console");
  for (int i = 0; i < 3; i++) procs[pid].fd[i] = file_new(dev_fops(DEV_CONSOLE), con, O_RDWR, NULL);
  return 0;
}
