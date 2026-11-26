# File system: what exists now (branch feat/vfs-persistence)

**Status: part 1 complete and usable (2026-11-26).** A short reference for the engineers building on it (archive tools, package manager, make,
the Linux binary engine). Written 2026-11-26. Everything below was run under Node with
`tools/run.mjs`; numbers of checks are at the end. Linux semantics unless a line says
otherwise.

## Objects

| Kind | Created by | Notes |
|---|---|---|
| regular file | `openat(O_CREAT)`, `mknodat(S_IFREG)`, `creat` | sizes and offsets are 32-bit (just under 2 GiB) |
| directory | `mkdirat` | `st_nlink` = 2 + subdirectories |
| symbolic link | `symlinkat`, tar type `2` | target up to 4095 bytes; `st_size` = target length; mode 0777 |
| hard link | `linkat`, tar type `1` | not for directories (`EPERM`) |
| FIFO node | `mknodat(S_IFIFO)`, `mkfifo`, tar type `6` | exists, stats, lists; **opening it gives `ENXIO`** |
| socket node | `bind` (AF_UNIX), `mknodat(S_IFSOCK)` | as before |
| device node | kernel only | `mknodat` of `S_IFCHR`/`S_IFBLK` gives `EPERM`; tar types `3`/`4` are skipped |

Names up to 255 bytes (was 63), paths up to 4096 bytes (was 255). Each directory has a
hash table: lookups and creates stay flat (about 1.3 us per create at 50,000 entries).

## System calls (Linux generic numbers, `abi/abi.h`)

New: `symlinkat 36`, `linkat 37`, `readlinkat 78`, `renameat2 276` (`RENAME_NOREPLACE`,
`RENAME_EXCHANGE`; `RENAME_WHITEOUT` is `EINVAL`), `mknodat 33`, `fchmod 52`, `fchmodat 53`,
`fchmodat2 452`, `fchown 55`, `fchownat 54`, `faccessat 48`, `faccessat2 439`, `umask 166`,
`statx 291`, `truncate 45`, `fallocate 47` (mode 0 only, else `EOPNOTSUPP`), `fsync 82`,
`fdatasync 83`, `sync 81`, `syncfs 267`, `flock 32`, `statfs 43`, `fstatfs 44`, `readv 65`,
`writev 66`, `preadv 69`, `pwritev 70`, `fchdir 50`, `chroot 51`, `getuid 174`, `geteuid 175`,
`getgid 176`, `getegid 177`, `setuid 146`, `setgid 144`, `setreuid 145`, `setregid 143`,
`setresuid 147`, `setresgid 149`, `getresuid 148`, `getresgid 150`, `setfsuid 151`,
`setfsgid 152`, `getgroups 158`, `setgroups 159` (at most 16 groups).

Changed: `openat`, `newfstatat` (honours `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`),
`utimensat` (all forms: directory descriptor, NULL path = the descriptor, `UTIME_NOW`,
`UTIME_OMIT`, nanoseconds, `AT_SYMLINK_NOFOLLOW`), `unlinkat`, `renameat`, `getdents64`
(`d_type` for every kind), `fcntl` (`F_GETLK F_SETLK F_SETLKW`, the `64` forms, the
`F_OFD_*` forms, `F_DUPFD_CLOEXEC`), `getcwd`, `chdir`, `ftruncate`.

Still `ENOSYS`: `sendfile`, `copy_file_range`, `openat2`, `name_to_handle_at`, xattrs,
`inotify`, `fanotify`, `mount`. Callers are expected to fall back.

Argument convention: one 32-bit register per argument, as for every call here. Offsets in
`truncate`, `fallocate`, `preadv`, `pwritev` are a single 32-bit value (Linux on 32-bit
machines splits a 64-bit offset over two registers; the binary engine must translate).

## Semantics worth knowing

- **Path walk**: links in the middle are always followed; in the last component when the
  call follows (`stat`, `open` without `O_NOFOLLOW`) or the path ends in `/`. At most 40
  links per walk, then `ELOOP`. `..` is physical: after following a link to a directory,
  `..` is that directory's real parent. `getcwd` after `chdir` through a link gives the
  physical path. `unlink`, `rmdir`, `rename`, `lchown`, `AT_SYMLINK_NOFOLLOW` act on the link.
  A trailing slash on a non-directory is `ENOTDIR`; `open("missing/", O_CREAT)` is `EISDIR`.
- **open**: `O_NOFOLLOW` on a link is `ELOOP` (or opens the link with `O_PATH`). `O_CREAT`
  through a dangling link creates the target; `O_CREAT|O_EXCL` never follows (`EEXIST`).
  `O_PATH` gives a descriptor for `fstat`, `fchdir`, `*at` and `close` only. `O_TMPFILE` is
  `EOPNOTSUPP`. `O_APPEND` writes are atomic (one thread). `O_DIRECTORY` checked.
- **Hard links and unlink**: data stays while a name or an open descriptor remains.
  `linkat(fd, "", ..., AT_EMPTY_PATH)` of an unlinked file is `ENOENT` (no `O_TMPFILE`).
  `rename` between two names of the same file does nothing, as POSIX says.
- **Modes and owners** are stored and reported exactly: all 12 mode bits, uid, gid. New
  files get the caller's fsuid/fsgid, the mode minus the umask (default 022, inherited by
  children), and the directory's group if it has set-group-ID. `chown` clears set-user-ID
  (and set-group-ID when group-execute is set), as Linux. `chmod` of a link with
  `AT_SYMLINK_NOFOLLOW` is `EOPNOTSUPP`. Files from the boot image keep the tar's mode,
  uid, gid and mtime (the build makes everything uid 0, gid 0).
- **Users**: every process starts as uid/gid 1000 (as `getuid` said before). The set*id
  calls work with Linux rules for root; while enforcement is off every process counts as
  privileged, so `setuid(0)` succeeds. Whether a root user exists is an owner decision.
- **Permission enforcement** is one switch, `VFS_ENFORCE_DEFAULT` in `kernel/kernel.h`,
  **0 today**: modes are stored and reported, nothing is refused (like running everything
  as root, plus root's exec rule turned off). Level 1 refuses to run a file with no execute
  bit (`EACCES` from spawn, as Linux does even for root). Level 2 is full owner/group/other
  checks on open, create, unlink, search, utimensat, chmod, chown, with the sticky bit.
  `access()` answers by the same switch. Level 1 breaks the shell suite's `./s.sh` test
  (the script is made without `chmod +x`), which is why it is not the default.
- **Times**: nanoseconds, from the host clock (millisecond resolution). mtime and ctime
  change on write, truncate, `O_TRUNC`; ctime on link, unlink (of a remaining name), rename,
  chmod, chown, utimensat; a directory's mtime and ctime on any change of its entries.
  **atime**: updated on read only when the file changed since it was last read (atime older
  than mtime or ctime), "relatime" without the 24-hour rule, so reading an unchanged file
  never reads the clock. Times have nanoseconds everywhere: `struct stat` now has `st_atim`, `st_mtim`, `st_ctim` (`struct timespec`; `st_mtime` and friends are macros for the seconds, as in glibc; this changes the size of `struct stat`, so every program must be rebuilt). The clock is the wall clock at boot plus `performance.now()` since (0.1 ms steps in Chrome, finer under Node), strictly increasing: two changes in a row never get the same stamp.
- **Locks**, advisory: `flock` whole-file locks owned by the open file (dropped when its
  last descriptor closes); POSIX record locks with byte ranges, owned by the process (any
  `close` by that process of any descriptor of the file drops them, as POSIX; a process
  that exits loses them at exit); OFD locks owned by the open file. `F_SETLKW` and blocking
  `flock` wait until an unlock, a close or a process exit wakes them. A record-lock wait that
  would close a circle of waiting processes returns `EDEADLK` (flock and OFD waits are not
  checked, as on Linux). flock and record locks do not see each other (as Linux). Locks are
  kept on regular files and directories only: `flock` on a device succeeds and records
  nothing, `fcntl` locks on anything else are `EINVAL`.
- **statfs**: `f_type` is `RAMFS_MAGIC` (0x858458f6) for the tree and `PROC_SUPER_MAGIC`
  for `/proc`; `f_bsize` 4096; free space is the memory the kernel could still claim
  (device memory, at most 4 GiB, minus what all instances use). Part 2 adds the store quota.
- **fsync, fdatasync, syncfs, sync** return 0 at once for now; with persistence they wait
  until the data is in the store.
- **Credentials and spawn**: a child inherits uid/gid sets, groups, umask and the chroot
  directory. `kernel/proc.c` calls `vfs_proc_start()` in `proc_spawn`; an `exec`
  implementation must call it too (it records `/proc/PID/exe`).

## /proc and /dev

| Path | What |
|---|---|
| `/proc/self` | link to `/proc/PID` |
| `/proc/PID/fd/N` | "magic" link: following it reaches the open file itself (also an unlinked file; for a pipe or socket, `open` returns a new descriptor for the same open file); `readlink` gives the path, `pipe:[N]` for pipes and sockets |
| `/proc/PID/exe`, `cwd` | magic links to the program and the working directory |
| `/proc/PID/cmdline`, `comm`, `status`, `mounts` | generated text (status: Name, State, Tgid, Pid, PPid, Uid, Gid) |
| `/proc/cpuinfo`, `meminfo`, `mounts`, `uptime`, `version`, `filesystems` | generated text |
| `/dev/fd` | link to `/proc/self/fd`; `/dev/stdin`, `stdout`, `stderr` link to fds 0, 1, 2 |
| `/dev/tty` | character device; opening it opens the caller's terminal (the first of fds 0..2 that is a pty or the console), else `ENXIO` |
| `/dev/shm` | empty directory, mode 1777 (files there are ordinary files: no shared memory) |
| `/dev/random` | same as `/dev/urandom` |

Not there, and what each would take: `/proc/self/maps` (wasm has one linear memory and no
mappings; a single line describing it is possible but tools that parse it expect ELF
mappings), `/proc/PID/stat` and `statm` (fields exist in the process table; a few lines),
`/proc/loadavg` (no load average is computed), `/proc/sys/*` (nothing to tune),
`/proc/self/environ` (the blob is in the process table: a few lines), `/etc/mtab` (a link
to `/proc/self/mounts` in the image would do it).

## C library (libc/src/fs_posix.c and headers)

`symlink symlinkat readlink readlinkat link linkat unlinkat renameat renameat2 fchdir
chroot mkdirat mknod mknodat mkfifo mkfifoat creat fstatat statx statfs fstatfs statvfs
fstatvfs chmod fchmod fchmodat chown lchown fchown fchownat umask utimensat futimens utimes
lutimes futimes utime faccessat eaccess euidaccess truncate fallocate posix_fallocate fsync
fdatasync syncfs sync flock lockf pread pwrite readv writev preadv pwritev setuid setgid
seteuid setegid setreuid setregid setresuid setresgid getresuid getresgid getgroups
setgroups dirfd fdopendir rewinddir`. Fixed in place: `lstat` (no longer the same as
`stat`), `access` (asks the kernel), `geteuid getgid getegid` (real calls), `realpath`
(resolves links: opens with `O_PATH` and reads `/proc/self/fd/N`).

New headers: `sys/file.h sys/statfs.h sys/vfs.h sys/statvfs.h sys/uio.h utime.h`.
`sys/stat.h` gained `S_ISLNK S_ISSOCK S_ISBLK` and the `S_IRUSR...` family; `sys/types.h`
gained `dev_t ino_t nlink_t blksize_t blkcnt_t`. `struct stat` gained nanoseconds (see Times above).

## Tools

New: `ln [-sfn]`, `chmod [-R] MODE` (octal and symbolic), `readlink [-f]`,
`stat [-L] [-c FORMAT]`. Changed: `ls -l` (type letter, set-ID and sticky letters, link
targets, `-F` marks links with `@`, links in colour), `cp -a`/`-P`/`-p` (links copied as
links; mode, owner, times kept; hard links between copied files are not kept), `rm` (never
follows a link).

## Limits and what is not done

- No sparse files: a file takes memory equal to its size.
- Hard links between files copied by `cp -a` become separate files.
- FIFOs cannot be opened. Devices cannot be created.
- No extended attributes, no ACLs, no file capabilities.
- `/proc/PID/fd/N` of an unlinked file reads as `/ (deleted inode N)`, not Linux's
  `/path (deleted)`; the name is gone when the last name goes. A file whose first name was
  removed while it has other names in other directories also loses its readable path.
- No mount table, no other file systems, `st_dev` is 1 (2 for /proc).

## Persistence

Part 2 (what is kept across reloads) is described in docs/PERSISTENCE.md. For programs it
changes three things: `fsync`, `fdatasync`, `syncfs` and `sync` wait until the data is
stored (and return `EIO` once storage failed); `statfs` reports the storage quota when a
store is attached; `/proc/ucos-store` shows the store's state and takes `wipe` and `sync`.

## Cost

Measured against the kernel before this work (A/B, 8 runs each, Node): read, write and
getppid unchanged within noise; open + close 15 to 29% slower (about 30 to 50 ns), stat 3 to
19% slower. Details in docs/PERSISTENCE.md section 7.

## Tests

`bin/ktest.c` (`fs_tests`): 101 new checks (210 in all). `bin/libctest.c`
(`test_fs_posix`): 26 new (316). `tools/tests/vfs-tar.mjs`: 11 checks of the image loader.
`bin/kfuzz.c` issues every new call. `bin/fsbench.c` measures directory scaling and memory.
