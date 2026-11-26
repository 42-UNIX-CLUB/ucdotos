# svc: service manager reading a subset of systemd unit files, systemctl, journalctl, Debian's helpers

- **Branch / worktree:** none (detached worktree `.worktrees/svc`, all uncommitted, nothing staged)
- **Owner:** agent of uc-os-dev
- **State:** ready for review (owners' questions open: names, default boot, desktop option; see docs/SERVICES.md section 14)
- **Updated:** 2026-11-26 10:50
- **Next step:** uc-os-dev re-runs the suites below on a fresh build; owners answer section 14 of docs/SERVICES.md
- **Waiting on:** owners (decisions); for the parts that need them: fork (signals), vfs (symlinks, persistence), lx (Debian daemons), mem (policy)

## Done so far
- Survey: 58 Debian trixie packages, 162 unit files, maintainer scripts of all 58 (`build-scratch/survey.txt`, `scripts-survey.txt` in the worktree, git-ignored)
- `libsvc/`: parser, values, Exec lines, specifiers, calendar, names, links (symlink or link file), unit lookup in UC.OS's root and the Debian root, enable/disable/mask logic. Native: 67/67; all 162 sample units and 548 host units parse; Exec reading equal to the host's `systemctl show` on 1,275 (unit, key) pairs (313 with commands)
- `bin/svcd/` (manager, placeholder name): deps, ordering, jobs, cycles (reported as systemd does), simple/exec/oneshot/notify/forking/idle (dbus as simple), restart with backoff and start limit, timeouts, conditions, timers (incl. OnCalendar), sockets (AF_UNIX, LISTEN_FDS, Accept=yes), path units (polled), journal ring, memory hooks (MemoryHigh logged, MemoryMax kills). Native core 114/114 (ASan+UBSan), journal ring stress, host integration 28/28
- `systemctl`, `journalctl`, `deb-systemd-helper`, `deb-systemd-invoke`, `systemd-tmpfiles`, `systemd-sysusers` (accept and ignore), `systemd-notify`, test stand-in `svctest`
- In the OS (manager as pid 1, Node): svc-tests 81/81; 1,000 start/stop cycles 4/4 (1,000 starts counted, 1,344 ms, manager 28 pages before and after); real Debian units (openssh-server, cron, rsyslog, logrotate) with native stand-ins 31/31
- Default boot unchanged; system suites on my build: ktest 109/0, libctest 290/0, sh-tests 238/238; desktop script: 6 screenshots, default boot and manager as pid 1, looked at
- Design document: docs/SERVICES.md in the worktree (the subset table is the specification)

## Needs from others
- **lx / uc-os-dev (engine, finished):** (1) guest maintainer scripts must reach the native `systemctl`, `deb-systemd-helper`, `deb-systemd-invoke`, `systemd-tmpfiles`, `systemd-sysusers` by those names: copies at /debian/usr/bin/NAME or a guest-to-native path map in lx (owners choose; lx already runs wasm files natively); (2) guest `/run` is translated into the root: the manager creates /debian/run/systemd/system too; passing /run through would make one directory serve both; (3) libsystemd daemons: NOTIFY_SOCKET here is a stream socket and needs `SENDER_PID=`; LISTEN_PID cannot be known before spawn on main: a translation in lx (or kernel datagrams + exec) is needed for Type=notify and socket-activated guest daemons; (4) a guest killed by a signal should end the engine with that signal. Test prepared: `libsvc/tests/mkdebian.sh` + `rootfs/usr/share/tests/svc/deb-tests.sh` with real binaries instead of stand-ins.
- **fork stream:** nothing required. Used when present (detected at run time, raw call numbers, no weak symbols): signal delivery makes stop send real SIGTERM then SIGKILL after TimeoutStopSec, and reload by `kill -HUP $MAINPID` stops being turned into a restart. Not used yet: SIGCHLD (would remove the 100 ms / 1 s polling tick), kill(-pgid), execve (LISTEN_PID helper), pmigrate (memory policy). Not tested in the fork tree.
- **vfs stream:** symlinks: `systemctl enable` makes real symlinks as soon as symlinkat works (link files otherwise; both read). Persistence: the enabled set is just files under /etc, nothing else to persist. procinfo has no pgid: the manager calls getpgid per pid (64 calls per scan); a pgid (and sid) field in `struct procinfo` would halve the scan.
- **mem stream:** the hook is `core_measure()` in bin/svcd/core.c (every 5 s, pages per unit from procinfo); MemoryHigh=/MemoryMax= parsed. Policy is theirs.
- **sh stream:** `sh -c '...; exec prog'` fails today (no `exec` builtin); service ExecStart lines often use it (Debian and own units). Not blocking: units here avoid it.
- **kernel (owners' call):** a per-process tag inherited across spawn (cgroup in miniature) for exact tracking (docs/SERVICES.md 11.2).

## Documentation notes
For uc-os-docs. Code: libsvc/, bin/svcd/, bin/systemctl/, bin/journalctl/, bin/deb-systemd-*/, bin/systemd-*/, rootfs/usr/lib/systemd/system/, rootfs/usr/share/tests/svc/. Verified by the suites listed above.
- ARCHITECTURE section 11 (desktop stack): with `init=/bin/svcd` the chain is svcd (pid 1) -> graphical.target -> display-manager.service (a link to wm.service) -> wm -> bar, term (wm's autostart unchanged). Default boot still bin/init.c.
- ARCHITECTURE new section "Services": see docs/SERVICES.md sections 4 to 13 (manager, unit search path /etc, /run, /usr/local/lib, /usr/lib, /lib under / and under the Debian root /debian; native or guest decided by the program's first bytes; control socket /run/svcd/control with a line protocol; journal ring 256 KiB default, RuntimeMaxUse= in /etc/systemd/journald.conf; NOTIFY_SOCKET=/run/systemd/notify is a stream socket and needs SENDER_PID=; link files stand in for symlinks; process groups track a service's processes; polling tick 100 ms busy, 1 s idle).
- ARCHITECTURE section 14 / limits: manager tables 768 units, 256 jobs, 12 clients, 40 output streams, 4 listening descriptors passed (3..6).
- DEVELOPMENT section 4 (tests): rows for `sh libsvc/tests/run.sh` (67 + 114 + journal), `sh libsvc/tests/host-integration.sh` (28), in-OS `node tools/run.mjs --build=B --init=/bin/svcd --args="--unit-path=/usr/share/tests/svc --unit=svc-selftest.target"` (81), `--unit=svc-cycles.target` (4), Debian units `sh libsvc/tests/mkdebian.sh B build-scratch/x B-deb` then `--unit=svc-debian.target` (31). A shell under the manager for debugging: `--unit=svc-shell.service --input='...'`.
- DEVELOPMENT trap: libsvc sources are compiled into programs through their CFLAGS files, so make does not see a change in libsvc/ alone (fresh B= or touch the program).
- DEVELOPMENT "what a program is": nothing new; programs with a CFLAGS file listing extra sources is a pattern now (bin/svcd/CFLAGS etc.).
- STATUS: what works and what does not (docs/SERVICES.md sections 1, 3, 15); Type=dbus runs as simple; IP sockets, datagram sockets, mounts and devices refused; no persistence of enable across reload on main.
- DECISIONS candidates (assumptions made, owners to confirm): see docs/SERVICES.md section 14 and the **assumed** marks (native root before Debian root; not virtualised; soft reboot; PATH and HOME given to services; one journal budget).
- Manual pages (text below) for rootfs/usr/share/man/: systemctl(1), journalctl(1). The format of the existing pages should be followed; this is the content.

### systemctl(1), content
NAME: systemctl - control the service manager

SYNOPSIS: systemctl [OPTIONS...] COMMAND [UNIT...]

DESCRIPTION: Asks the service manager (svcd) to start, stop and inspect services and
other units, and changes which units start at boot. Unit files are read from
/etc/systemd/system, /run/systemd/system, /usr/local/lib/systemd/system,
/usr/lib/systemd/system and /lib/systemd/system, and the same directories under the
Debian root (/debian). A name without a suffix means NAME.service. Commands that change
or read unit files (enable, disable, reenable, mask, unmask, is-enabled, cat,
list-unit-files, get-default, set-default) work even when the manager is not running.
The manager runs only when the system was started with init=/bin/svcd.

UNIT COMMANDS: list-units [--all] [--type=T] [--state=S]; list-timers; list-jobs;
status [UNIT...] (state, main process, memory, processes and the last 10 journal lines;
-n N for more); show [UNIT...] [-p NAME] [--value]; cat UNIT...; is-active UNIT...;
is-failed UNIT...; start, stop, restart, try-restart, reload, reload-or-restart,
try-reload-or-restart UNIT... (wait until done unless --no-block); isolate TARGET;
kill UNIT [--signal=SIG] [--kill-whom=main|all]; reset-failed [UNIT].

UNIT FILE COMMANDS: list-unit-files; enable, disable, reenable UNIT... [--now];
mask, unmask UNIT... [--now]; is-enabled UNIT...; get-default; set-default TARGET.

MANAGER COMMANDS: daemon-reload; daemon-reexec (same as daemon-reload);
is-system-running; poweroff, halt (stop everything; the tab then shows the last frame:
reload the page to start again); reboot, soft-reboot (stop everything and start the
default target again in the same tab).

OPTIONS: -a --all, -q --quiet, --no-block, --now, -p --property=NAME, --value,
-t --type=TYPE, --state=STATE, --no-legend, -n --lines=N, -s --signal=SIG,
--kill-whom=main|all, --job-mode=replace|fail|isolate, --root=DIR, --no-pager (no
effect), --system (no effect), --version. --user is refused: there is one user and one
manager.

EXIT STATUS: 0 success. status: 0 active, 3 not active or failed, 4 no such unit.
is-active: 0 if at least one unit is active, 3 otherwise. is-failed: 0 if at least one
failed, 1 otherwise. is-enabled: 0 for enabled, static, indirect or alias, 1 otherwise.
start and friends: 1 if a job failed (the message names the reason and says where to
look), 5 for a unit that does not exist.

ON UC.OS: there are no signals on the kernel yet: stop ends a service at once, and a
reload done with "kill -HUP $MAINPID" becomes a restart (said in the journal).
Enabling makes link files where systemd makes symlinks, until the kernel has symlinks.
Nothing persists across a reload of the page. D-Bus does not exist: Type=dbus services
start as simple ones. See /usr/share/doc or docs/SERVICES.md for the supported keys.

FILES: /etc/systemd/system (admin's units and links), /usr/lib/systemd/system (the
system's), /etc/systemd/system.conf, /run/svcd/control (the manager's socket).

SEE ALSO: journalctl(1), deb-systemd-helper, deb-systemd-invoke, systemd.unit(5) and
systemd.service(5) (systemd's documentation; UC.OS reads a subset).

### journalctl(1), content
NAME: journalctl - show the service manager's journal

SYNOPSIS: journalctl [OPTIONS...]

DESCRIPTION: Shows what services printed and what the manager logged about them. The
journal is kept in the manager's memory, in a ring of fixed size (256 KiB unless
RuntimeMaxUse= in /etc/systemd/journald.conf says otherwise): when it is full the
oldest lines go. It holds only the current boot; nothing is written to a file.

OPTIONS: -u --unit=UNIT (several allowed; a name without suffix means .service),
-n --lines[=N] (the last N, 10 without a number), -f --follow (keep showing new lines;
Ctrl-C ends it), -e (the end: last 1000), -r --reverse, -b --boot (only 0, the current
boot, exists), --list-boots, -p --priority=PRIO (0 to 7 or emerg..debug), -t
--identifier=NAME (the program's name), -o --output=short|short-iso|cat, -k (kernel
messages: there are none here), --disk-usage (how much of the ring is used), -q, -x,
--no-pager, --vacuum-*, --rotate, --flush (no effect).

OUTPUT: "Nov 26 10:15:35 42lisboa NAME[PID]: TEXT". Times are UTC. Lines from the
manager itself have NAME svcd.

EXIT STATUS: 0, or 1 if the manager is not running or an option is wrong.

SEE ALSO: systemctl(1).

## Assumptions and questions for the owners
- All in docs/SERVICES.md section 14 (worktree), with options: (1) names: manager `svcd`, socket `/run/svcd/control`, keys `GuestRoot=`/`GuestEngine=`, helper `svctest`, hidden `systemctl load-all`; (2) default boot: init.c (unchanged) or the manager, and pid 1 (A) or under init (B); (3) desktop D1 (built: only the wm is a unit) to D4; (4) reboot in a tab: soft reboot (built), page reload (boot/ change), or poweroff; (5) how guest scripts reach native helpers; (6) network targets reached with no network; (7) ConditionVirtualization; (8) kernel process tag; (9) journal budget per unit or global (built: global); (10) shims for units Debian expects from the systemd package (syslog.socket).
- Mechanical choices, reversible: native root searched before the Debian root; services get PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin, HOME=/home/user, USER=user, LANG=C.UTF-8, TERM=xterm-256color; tick 100 ms busy / 1 s idle; memory measured every 5 s; link file format (one absolute path and a newline); deb-systemd-invoke skips start of units not enabled (enabled, static, indirect, alias count) and restart of units neither enabled nor active (the man page does not say; this is a reading of the Debian tool's behaviour).

## Files left behind (git-ignored, in the worktree)
- `build-scratch/` (87 MB: Debian index, 58 .deb, unpacked units and control files, survey scripts and outputs; never to be committed: Debian's files)
- `build-svc/`, `build-svc2/`, `build-svc-deb/`, `build-svc2-deb/`, `build-svc-deball/`, `build-svc-native/` (build output; build-svc-native holds core-root-PID and host-PID scratch trees from each native test run)

## Log
### 2026-11-26 09:20
Started. Read the brief, docs, fork/mem/vfs/lx/pkg stream files. /tmp quota hit once (EDQUOT) while unpacking the index; moved everything to build-scratch/ in the worktree as uc-os-dev asked.
### 2026-11-26 09:50
Survey done; parser and core written and tested natively (67, 113).
### 2026-11-26 10:15
Daemon, systemctl, journalctl, Debian helpers built for UC.OS and the host. Host integration 28/28. In-OS suite reached 76/76 after fixing: lost notifications (connection read before the message arrived), notifications given to a dead unit whose process group number had been reused (pids are reused at once), cycle detection across separate requests (now per transaction, as systemd, plus a breaker for jobs waiting on each other), journal dumps held whole in the manager's memory (now 16 KiB at a time).
### 2026-11-26 10:30
1,000 cycles 4/4 (1,344 ms, 28 pages flat). Debian units with stand-ins 31/31. System suites 109/290/238 on my build. Desktop under the manager looks like the default boot; idle tick raised to 1 s (about 7 system calls per second idle). Measured (load 2.9 to 3.8, Node): all 151 sample units load in 56 ms, daemon-reload of 204 units 11 to 14 ms, start-up to graphical.target 7 to 10 ms.
### 2026-11-26 10:45
systemd-notify (with SENDER_PPID: the sender exits before the manager reads) and systemd-tmpfiles tested in the OS: 81/81. Design document written.
### 2026-11-26 10:50
Final reruns on a fresh build (build-svc2, load 2.4 to 4.9): ktest 109/0, libctest 290/0, sh-tests 238/238; svc-tests 81/81; cycles 4/4 (1,594 ms, 28 pages flat); Debian units 31/31; native 67 + 114 + journal; host integration 28/28; desktop script 6 + 6 screenshots (default boot and manager), looked at 02, 04, 06: same desktop; htop shows init at 0 system calls per second in the default boot and svcd at 10 per second during the theme switch. Not run: Chromium, the fork tree, the engine, the desktop fallback chain. Nothing committed.
