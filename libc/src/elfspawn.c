// SPDX-License-Identifier: GPL-2.0-or-later
/* PROTOTYPE (integration, 2026-11-26): start a Linux riscv64 program the kernel refused
 * with ENOEXEC through the binary engine /bin/lx, as Linux's binfmt_misc would
 * (docs/LINUX-BINARIES.md section 8.2). spawn() calls this; so every program that starts
 * others through the C library (the shell, env, system, popen) gets it.
 * A file inside the guest root (LX_ROOT, else /debian) is given to the engine by its path
 * inside that root, so its dynamic loader and libraries come from the root; any other
 * file runs with the root "/". Whether this belongs here, in the shell or in the kernel is
 * the owners' choice. */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>

pid_t __elf_spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3]) {
  unsigned char h[20];
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -ENOEXEC;
  long n = read(fd, h, sizeof h);
  close(fd);
  /* ELF, 64-bit, little-endian, e_machine 243 (RISC-V) */
  if (n < 20 || memcmp(h, "\177ELF", 4) || h[4] != 2 || h[5] != 1 || h[18] != 243 || h[19] != 0) return -ENOEXEC;
  char abs[4096], a0[300], rootopt[300];
  if (!realpath(path, abs)) return -errno;
  const char *root = getenv("LX_ROOT");
  if (!root || !*root) root = "/debian";
  size_t rl = strlen(root);
  const char *guest = abs;
  if (!strncmp(abs, root, rl) && abs[rl] == '/') guest = abs + rl;
  else root = "/";
  int argc = 0;
  while (argv && argv[argc]) argc++;
  char **av = malloc((argc + 5) * sizeof *av);
  if (!av) return -ENOMEM;
  int k = 0;
  av[k++] = "lx";
  if (strlen(root) + 8 >= sizeof rootopt || strlen(argc ? argv[0] : path) + 8 >= sizeof a0) { free(av); return -ENAMETOOLONG; }
  strcpy(rootopt, "--root="); strcat(rootopt, root);
  av[k++] = rootopt;
  strcpy(a0, "--argv0="); strcat(a0, argc ? argv[0] : path);
  av[k++] = a0;
  av[k++] = (char *)guest;
  for (int i = 1; i < argc; i++) av[k++] = argv[i];
  av[k] = 0;
  long r = __syscall4(SYS_spawn, "/bin/lx", av, envp, fdmap);
  free(av);
  return r;
}
