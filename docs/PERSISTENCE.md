# Persistence: designs, what was built, measurements, open decisions

Stream `feat/vfs-persistence`, written 2026-11-26. Everything stated as measured was run on
the owner's laptop (8 threads, shared with other builds; load average 5 to 11 during the
day, which makes timings noisy). What was not run is marked.

## 1. The constraints

- The kernel never suspends: a system call runs to completion. A call that must wait
  returns 512 and is retried when woken (ARCHITECTURE section 4).
- Browser storage is asynchronous on the main thread (IndexedDB, and OPFS through
  `createWritable`). OPFS synchronous access handles exist only in Workers.
- A tab can be closed or killed at any moment. `pagehide` and `visibilitychange` are best
  effort: an asynchronous write started there may not finish.
- Storage is evicted under pressure unless `navigator.storage.persist()` is granted.
- Two tabs of the same origin share the same storage.

Quotas (MDN, "Storage quotas and eviction criteria", read 2026-11-26):

| Browser | Best effort | Persistent | Notes |
|---|---|---|---|
| Chrome, Edge | up to 60% of total disk per origin | same | quota from total disk, not free space; all origins together at most 80% of disk; eviction LRU by origin, persistent origins skipped |
| Firefox | min(10% of disk, 10 GiB group limit per site) | up to 50% of disk, capped at 8 TiB | `persist()` shows the user a prompt |
| Safari (macOS 14, iOS 17 and later) | about 60% of disk for browser apps, 15% for embedded web views | (no separate figure) | all origins at most 80% (20% non-browser); with tracking prevention on, script-created data of a site without user interaction in 7 days of use is deleted |

Chrome and Safari grant or deny `persist()` silently from the user's history with the site.
Eviction is all or nothing per origin.

## 2. The designs

| | (a) write-behind of changes | (b) whole OS in a Worker, synchronous OPFS | (c) snapshot of the whole tree | (d) block device with an on-disk format |
|---|---|---|---|---|
| How | kernel keeps the truth in memory, notes changed inodes and namespace operations; the host drains them in batches into IndexedDB (or OPFS); `fsync` waits for "durable" with park-and-retry | the machine runs in a Worker (WORKER-PROCESSES.md approach D); the kernel could write through `FileSystemSyncAccessHandle` synchronously inside the call | serialize the whole tree on a timer and on `pagehide`/`visibilitychange` | a real file system format (ext2-like) over fixed-size blocks; the block layer reads and writes through the host |
| Lost on a random tab kill | changes since the last committed batch (at most the write-behind delay, 0.5 s here, unless an `fsync` returned) | nothing that a returned `fsync` or `write` covered, if every write goes through synchronously; with a cache, the unflushed cache | everything since the last snapshot (seconds to minutes); a snapshot torn by the kill must be rejected as a whole | depends on journaling; without it, a torn metadata update corrupts |
| Write amplification | each changed 64 KiB block once per batch, plus a metadata record per changed inode; appending 100 bytes to a 50 MiB file writes one block (measured below) | whatever the on-disk layout needs; with per-file storage, about 1 | the whole tree every time (100 MiB stored = 100 MiB per snapshot) | journal plus data: 2x for journaled data, 1x plus metadata for ordered mode |
| Memory (is data held twice?) | once in kernel memory; the store holds the other copy on disk; JavaScript holds a batch (8 MiB at most) only while writing it | could be held once (on disk, read on demand), at the price of a page cache in the kernel | a full serialized copy during each snapshot | a block cache plus the store |
| Boot time with stored data | metadata only at boot; contents fetched on first open | small if lazy; the Worker must start first | must read and unpack the whole snapshot: grows linearly (1 GiB = seconds to tens of seconds, and 1 GiB of memory) | metadata blocks on demand |
| Complexity | kernel: about 650 lines (store.c); JavaScript: about 330 lines (store.mjs), off the system call path | a different architecture (canvas, input, timers move); the storage part itself is simple | small | a large amount of new kernel code (format, allocator, fsck, journal) |
| Two tabs | must be prevented (one writer): Web Locks | OPFS sync handles are exclusive per file, which prevents it by itself | last writer wins, silently | must be prevented |

Why (a) was built, as an assumption the owners can overturn: it fits the current
architecture (kernel on the main thread, decision 5), keeps JavaScript out of the system
call path, gives `fsync` a real meaning with the existing park-and-retry mechanism, and the
boot time does not grow with the stored data. (b) is the better answer if the owners move
the machine into a Worker for other reasons; the kernel half of (a) would carry over (the
host half would write synchronously). (c) loses too much and does not scale past tens of
MiB. (d) is the most work and buys little inside a browser, whose storage is already a
key/value or file store.

## 3. What was built (design a)

### Kernel: `kernel/store.c`

- A stored layer over the boot image, as key/value records: `A sid` anchor (this inode is
  the image's inode named NAME in directory PARENT; written once when an image inode
  first changes), `M sid` metadata (mode, owner, times, size, link target, flags), `D dirsid
  name` directory entry (child sid, or 0 = whiteout), `B sid n` 64 KiB of data.
- Hooks in `vfs.c`: `store_ns` (create, unlink, link), `store_rename`, `vfs_changed`
  (any metadata change), `store_dirty` (data range written). A write to a regular file that
  is not in the persistent part costs one test of `i->sflags` (zero when no store is
  attached). The first change after a drain calls the host once (`store_kick`); later
  changes call nothing.
- Dirty tracking per inode: one byte range per file (`struct sdirty`), widened by each
  write. The first change of an image file copies all of it (the store then holds the
  whole file). A rename into the persistent part from outside (for example from `/tmp`)
  stores the whole subtree.
- Draining (`store_drain`): one batch holds every pending namespace record, the metadata
  of every dirty inode, and file data up to 8 MiB. Data beyond the budget goes in the next
  batch; until a file's data is complete, its stored size stays at the size whose blocks
  are all correct, so a crash shows an older size, never a size without data.
- `fsync`, `fdatasync`, `syncfs`, `sync` wait (park and retry) until a batch that covered
  every change made before the call is reported written (`store_done`). They return `EIO`
  once the store has failed. `fsync(fd)` waits for everything, not only that file.
- Boot (`store_apply`, called from `kmain` after the image, before init): anchors are
  resolved in the image, nodes made, metadata laid over, then directory entries applied
  (all removals first, then all additions). File contents are not read: a file with stored
  data is marked lazy; the first `open` (or `spawn`, `truncate`) asks the host
  (`store_fetch`) and parks until `store_filled`.
- What persists comes from a table: built in, everything except `/tmp`, `/run`, `/proc`,
  `/dev`; an image can ship `/etc/ucos/persist.conf` with lines `skip PATH` and
  `persist PATH` to change it (for example `skip /` then `persist /home`).
- `/proc/ucos-store`: status (state, quota, usage, pending changes, batches, bytes, files
  loaded). Writing `wipe` empties the store (the running system keeps going, nothing more
  is stored, the next boot starts from the image); writing `sync` is `sync()`.

### Host: `boot/store.mjs`

- One interface, two backends. `idb`: IndexedDB database `ucos-store`, object stores
  `meta` and `blocks`, binary keys; one transaction per batch (durability `strict` when a
  process waits in `fsync`, `relaxed` otherwise). `file` (Node): one append-only log file;
  a batch is a transaction record followed by a commit record, written and `fsync`ed; at
  load, a transaction without its commit record is cut off. The log is compacted (live
  records rewritten to a new file, renamed over the old one) when it is over 64 MiB and
  more than twice the live data.
- Write-behind delay 500 ms; a waiting `fsync` drains at once. Nothing runs while nothing
  changes: no timer is set until the kernel calls `store_kick`.
- Schema: record `I` holds format 1. A store of another format is left untouched and the
  system boots without it (message on the boot log). There is no migration code yet.
- `boot/host.mjs`: 6 lines (imports and two calls around `kmain`). `boot/page.mjs`: Web
  Lock `ucos-store` held for the life of the tab; a second tab boots from the image without
  a store and says so; `?reset` deletes the database first; `?nostore` boots without one;
  `persist()` is requested; `visibilitychange` (hidden) and `pagehide` start a drain.
  `tools/run.mjs`: `--store=FILE`, `--store-crash=N` (test mode: stop in the middle of
  storage write N), `--store-flags=N`.

## 4. Measurements

All on 2026-11-26. Node 26 unless marked Chromium 158 (headless). Load average at the time is
given where it mattered; the machine was shared.

**Backend choice (Chromium, main thread, random data)**, `node tools/browser.mjs --build=B
--query=nostore --script=tools/tests/store-bench-browser.mjs`:

| Operation | IndexedDB | OPFS (`createWritable`) |
|---|---|---|
| 20,000 small records / files | 2.4 s (20 transactions of 1,000) | 11.6 s for 2,000 files (20,000 not run: about 2 min by extrapolation) |
| one 100 MiB file in 64 KiB pieces | 1.7 s | 3.2 s |
| rewrite one 64 KiB block of that file | 3.2 ms | 722 ms (the whole file goes through a swap file) |
| read the 100 MiB back | 1.8 s (1,600 blocks) | 0.17 s |
| read every key of 20,002 (boot) | 90 ms | (no equivalent) |

IndexedDB was chosen: small files and in-place block rewrites are what the store does all
the time; OPFS only wins at reading one large file back.

**Dirty tracking, "append 100 bytes to a 50 MiB file"** (`--store-flags=4` switches to
whole-file tracking): per-range tracking writes a 205-byte batch; whole-file tracking
writes 52.4 MB. Writing the 50 MiB file the first time stored 52.4 MB (1.0x).

**20,000 small files and one 100 MiB file** (`node tools/tests/persist.mjs --small=20000
--big=100`, load 2.9): creating and syncing all of it took 8.1 s; the store file is 106.1 MiB
for about 103.5 MiB of file data (2.5% overhead); the next boot applied 40,119 records in
58 ms, and verifying every byte and attribute passed (the whole boot plus verify: 6.1 s,
which includes reading all 103 MiB back through lazy loading).

**Boot time against stored size** (2,000 small files plus one big file; `sh
build/persist-test/bootseries.sh MB`; Node, load below 3; a boot without a store takes
88 to 129 ms to run /bin/true and halt):

| Stored | Host opens the store | Kernel applies records | Whole boot to /bin/true | Notes |
|---|---|---|---|---|
| 11.8 MiB | 63 ms | 2.3 ms (4,119 records) | 176 ms | verify passed |
| 103 MiB | 154 to 186 ms | 2.4 to 7.7 ms | 284 to 384 ms | verify passed |
| 1,009 MiB | 2,933 ms cold, about 900 ms warm | 7.3 ms | 1,024 ms | verify not run to the end (reading 1 GiB back exceeded the 90 s limit of the run) |

The kernel's part does not grow with the data (records are metadata only); the host's part
does, because the Node backend walks its log file and skips over block data. The browser
backend reads only the `meta` store at boot (90 ms for 20,000 keys above).

**Chromium end to end** (`tools/tests/browser-persist.mjs`, 2,000 files plus 8 MiB): sync
1.07 s; after a reload 4,121 records applied in 4.4 ms, first frame 378 ms after navigation.
`navigator.storage.estimate()` reported 1.55 MB used for this data right after the fill
(Chrome compresses IndexedDB values and the estimate lags; not investigated further).

**Crash points** (`tools/tests/persist-crash.mjs`): 79 points (storage writes 1 to 950 of a
200-change workload with a sync every 20 changes and two pauses that let a background drain
happen), each followed by a boot and a model check: every recovered tree was exactly the
state after some change at or after the last completed sync; one point recovered a state
between syncs (change 110, sync 100). Three stops in the middle of a 48 MiB file: the file
came back with an older, consistent size (0 bytes: the new data spanned several batches).

**Idle**: no timer runs while nothing changes (the host sets one only after the kernel's
`store_kick`); the kernel does nothing per frame. Not measured with a profiler.

**System call cost**: see section 7.

## 5. Open decisions for the owners

| Topic | Options |
|---|---|
| Design | (a) as built; (b) move the machine into a Worker and write synchronously; (c) snapshots; (d) a block device and on-disk format |
| What persists | overlay of the whole tree on the image (built: a newer image updates files the user has not changed); only chosen directories (`/etc/ucos/persist.conf` with `skip /` and `persist` lines; costs nothing more); everything with no overlay (the image used only on first boot: not built; the kernel would skip the image's contents when a store exists, and an image update would never reach existing users) |
| Overlay rules when the image changes under a file the user changed | user wins (built: the stored copy replaces the image's file); image wins for chosen trees (for example `/usr` belongs to the image; would need a per-path rule in the config); ask (no UI for it) |
| Deleted image files | whiteouts persist, so a deleted image file stays deleted after an image update (built); or deletions of image files are forgotten when the image changes |
| Backend | IndexedDB (built; the numbers are in section 4); OPFS from the main thread (each file write goes through a swap file); OPFS synchronous handles (needs design b) |
| Two tabs | second tab boots from the image and keeps nothing (built); second tab read-only on a snapshot of the store (its files can change under it while the first tab writes); one tab takes over from the other (the first must stop and flush; needs a message channel) |
| Reset | `?reset` on the page (built); `/proc/ucos-store` + `echo wipe` (built); a command (proposed name `persist`, not built) |
| Storage full | built: the failing batch stops persistence, `fsync`/`sync` return `EIO`, `statfs` reports 0 free, `/proc/ucos-store` says why, writes keep working in memory. Alternatives: return `ENOSPC` from `write` once usage approaches the quota (needs a usage estimate the kernel trusts); keep retrying the batch |
| Eviction | ask `persist()` at boot (built, silently decided by Chrome and Safari, a prompt in Firefox); ask only after the user stored something; never ask |
| atime | relatime without the 24-hour rule (built): reading a changed file stores its access time once |

## 6. Known limits

- A single change bigger than 8 MiB of data is split across batches. A crash in between
  leaves the file at its older stored size (new files: size 0) with consistent contents.
  An overwrite in place that spans batches can leave some blocks new and some old (like
  ext4 in writeback mode); appends and new files cannot.
- Whiteouts in image directories are never removed (a few dozen bytes each).
- Hard links between an image file and a name created later are stored by anchor; if the
  newer image no longer has the anchored file, it is recreated where it was.
- Boundaries in `persist.conf` must exist at boot; a directory created later is decided by
  its parent.
- Firefox and Safari were not run (not installed here).

## 7. System call cost (A/B against the kernel before this stream)

`node tools/bench/ab.mjs --a=.:build-before --b=.:build --runs=4 --only=getppid,read,write,open_close,stat`,
run twice (8 runs per side), alternating, Node, load average 2.8 to 3.0, after the
optimisations listed below. Medians in ns, A = before, B = this stream, no store attached:

| Call | A, run 1 | B, run 1 | A, run 2 | B, run 2 |
|---|---|---|---|---|
| getppid | 11.1 | 12.7 | 12.7 | 12.6 |
| read 64 B | 52.4 | 55.1 | 51.1 | 50.8 |
| write 64 B | 20.4 | 22.7 | 21.8 | 21.2 |
| open + close | 171 | 220 | 181 | 209 |
| stat | 150 | 179 | 160 | 164 |

read, write and getppid are within the noise (getppid's path did not change, and still moved
by up to 14% between rounds). **open + close is 15 to 29% slower and stat 3 to 19% slower:
the requirement "not slower beyond noise" is not met for these two.** The profile (V8
`--cpu-prof`) puts the kernel's share in `vfs_walk`, the two-level dispatch and the path
copy; allocation is under 1%. Already done to narrow it: the name hash is computed in the
same pass as the component scan, openat/newfstatat/fstat are dispatched directly from
`syscall_slow`, `fill_stat` has no 64-bit division, the lazy-data and lock tests are inline
(before these: +32 to 40% and +12 to 17%). Not tried: a cache of recent path lookups;
merging `path_from_user` with the walk; trimming `struct nameidata`.

Writes to a stored file (`fsbench calls`, a store attached): pwrite of 64 bytes 147 ns in
/home/user against 127 to 141 ns in /tmp (not stored) and 117 ns with no store at all, so
about 10 to 30 ns per write to a stored file.
