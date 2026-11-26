# Services: a manager that reads systemd unit files

Design and state of the service manager of UC.OS. This is the svc stream's own document
until it is handed over; facts here were measured on 2026-11-26 in the worktree
`.worktrees/svc` unless marked otherwise. Nothing here is merged.

The owner's decision this builds on: "A manager that reads a subset of systemd unit files,
so Debian services work as shipped." Everything else in this file is either a fact, a
mechanical choice inside that decision (marked **assumed**, with how to reverse it), or an
**open** question with its options. Names of things users or scripts do not type
(`svcd`, its socket path, its configuration keys) are placeholders: see section 14.

Contents: 1 what exists, 2 the survey, 3 the supported subset (the specification),
4 architecture options, 5 where units come from and how programs run, 6 the desktop,
7 talking to the manager, 8 the journal, 9 readiness, 10 enabled state and persistence,
11 process control, 12 memory and efficiency, 13 the rest of the system, 14 open
questions, 15 tests and measurements, 16 files.

## 1. What exists

| Piece | Where | State |
|---|---|---|
| unit file library (parser, values, Exec lines, specifiers, calendar, names, links, lookup, enable logic) | `libsvc/` | built, native tests 67/67 |
| the manager (`svcd`, placeholder name) | `bin/svcd/` (`core.c` plain C, `journal.c`, `main.c` the UC.OS/POSIX layer) | built, native core tests 114/114, journal stress test, host integration 28/28, in-OS tests below |
| `systemctl`, `journalctl` | `bin/systemctl/`, `bin/journalctl/` | built |
| `deb-systemd-helper`, `deb-systemd-invoke`, `systemd-tmpfiles`, `systemd-sysusers`, `systemd-notify` | `bin/<name>/` | built |
| standard targets, desktop units | `rootfs/usr/lib/systemd/system/`, `rootfs/etc/systemd/system/display-manager.service` | shipped in the image, used only when the manager is pid 1 |
| in-OS tests | `rootfs/usr/share/tests/svc/`, `bin/svctest/` (stand-in daemon) | 81/81, 4/4 (1,000 cycles), 31/31 (real Debian units) |

The default boot is unchanged: `/bin/init` (bin/init.c, not modified) still starts the
desktop. The manager runs only when the kernel command line says `init=/bin/svcd`
(page: `?cmdline=init=/bin/svcd`; runners: `--init=/bin/svcd`).

## 2. The survey

Method: 58 Debian trixie packages (amd64 index of deb.debian.org, 2026-11-26) fetched
with the host's curl, unpacked with ar and tar into `build-scratch/` of the worktree
(git-ignored, never committed). Picked for having long-lived services, timers, sockets
and path units: openssh-server nginx cron rsyslog dbus-daemon dbus-system-bus-common
apache2 postgresql-common redis-server avahi-daemon cups-daemon ntpsec
systemd-timesyncd chrony exim4-base logrotate man-db apt e2fsprogs util-linux sysstat
unattended-upgrades smartmontools haveged irqbalance lighttpd mariadb-server bind9
dnsmasq postfix dovecot-core vsftpd rpcbind nfs-kernel-server samba snmpd openvpn
wpasupplicant udisks2 polkitd anacron at fail2ban uuid-runtime pcscd lvm2 mdadm plocate
accountsservice rtkit memcached tor squid tftpd-hpa clamav-freshclam certbot
prometheus-node-exporter cockpit-ws. Scripts: `build-scratch/scripts/survey.py` and
`scripts-survey.py`; raw output: `build-scratch/survey.txt`, `scripts-survey.txt`.

Unit files: 56 of the 58 packages ship some. 162 files: 114 services, 24 timers, 18
sockets, 3 path units, 1 target, 1 slice, 1 drop-in. 37 are templates (`@`). Specifiers
used: `%i` 81 times, `%I` 43, `%f` 4, `%N` 2, `%t` 1. No package ships a `.mount`,
`.device` or `.swap` unit, but units refer to them (`proc-fs-nfsd.mount`,
`sys-subsystem-net-devices-%i.device`).

`Type=` of the 114 services: oneshot 41, notify 21, forking 15, simple 9 (explicit) plus
23 with no `Type=` (simple by default), dbus 5, notify-reload 1. `idle` and `exec`: 0.

Maintainer scripts (preinst, postinst, prerm, postrm of the same 58 packages), lines
that call:

| Command | Lines | Packages | Main subcommands |
|---|---|---|---|
| `deb-systemd-helper` | 509 | 54 | unmask 91, was-enabled 88, enable 86, update-state 86, purge 71, debian-installed 9, disable 2 |
| `systemctl` | 137 | 52 | `--system daemon-reload` 127; start, stop, mask, unmask, enable, disable, reload, is-active once or twice each |
| `deb-systemd-invoke` | 123 | 51 | stop 63, `$_dh_action` (start or restart) 51, start 6, try-restart 2 |
| `invoke-rc.d`, `update-rc.d` | 91, 87 | 38, 40 | sysvinit compatibility (out of scope here, see 5.4) |
| `systemd-tmpfiles` | 42 | 19 | `--create NAME.conf`, once with `--exclude-prefix` and `--root` |
| `systemd-sysusers` | 7 | 5 | |
| test for `/run/systemd/system` | 205 | 52 | the scripts' test for "systemd is running" |

tmpfiles.d line types shipped: `d` 24 lines (15 packages), `x` 3, `L` 2, `D` 2, `C` 1,
`f` 1, `L+` 1, `p` 1. sysusers.d: `u` 4, `u!` 1.

Section and key counts (packages, files) for every key seen are in
`build-scratch/survey.txt`; the decisions for each are the table in section 3.

## 3. The supported subset (the specification)

Legend: **impl** implemented and tested; **impl-nt** implemented, not tested; **ign**
accepted and ignored, with what is lost; **refuse** the unit or setting fails, with why.
Numbers are (packages, files) from the survey.

### [Unit]

| Key | Survey | Decision |
|---|---|---|
| Description, Documentation | 56/159, 46/120 | impl (Documentation kept in the file, not shown) |
| After, Before | 37/84, 15/25 | impl; systemd's rule for start/stop pairs |
| Requires, Wants, Requisite, BindsTo, PartOf, Conflicts | 13/28, 15/33, 1/2, 2/9, 8/10, 8/14 | impl (Requisite impl-nt) |
| OnFailure | 1/1 | impl |
| ReloadPropagatedFrom, PropagatesReloadTo | 3/4, 0 | impl-nt |
| DefaultDependencies | 7/18 | impl (services, sockets, timers, paths, targets as in systemd.special(7), with the targets of 5.3) |
| ConditionPathExists, ...IsDirectory, ...ExistsGlob, FileIsExecutable, PathIsReadWrite, PathIsSymbolicLink, DirectoryNotEmpty, FileNotEmpty; `!` and `\|` | 8/12 ... | impl; globs only in the last path component (coarse: the directory must exist); FileIsExecutable means "regular file" until the kernel has modes |
| ConditionVirtualization | 5/7 | impl, **assumed**: UC.OS counts as not virtualised (`!container` holds). Open: report "vm" or a new value |
| ConditionACPower | 7/8 | impl: no power supply is known, so `true` holds and `false` fails (systemd's rule) |
| ConditionCapability | 4/10 | ign: holds (one user who may do everything) |
| ConditionFirstBoot | 1/1 | impl: true when /etc/machine-id is missing or empty; with no persistence every boot is a first boot |
| ConditionCPUs, ConditionHost | 1/1, 0 | impl (1 CPU, host 42lisboa) |
| ConditionKernelCommandLine, ConditionSecurity, ConditionNeedsUpdate, ConditionControlGroupController | 0 | impl: never hold |
| Assert... | 2/9 | impl: a failed assertion fails the start |
| RequiresMountsFor | 5/9 | ign: there are no mounts; the file system is there from the start |
| StartLimitIntervalSec, StartLimitBurst | | impl (defaults 10 s and 5, as systemd) |
| SuccessAction, FailureAction | 0 | impl for poweroff, halt, exit, reboot (used by the desktop units) |
| AllowIsolate, IgnoreOnIsolate, RefuseManualStart | 1/1, 2/2 | impl |
| Anything else | | ign silently |

### [Service]

| Key | Survey | Decision |
|---|---|---|
| Type=simple, exec, oneshot (+RemainAfterExit), notify, notify-reload, forking, idle | | impl; `exec` equals `simple` here because spawn checks the program before it returns; `idle` starts like `simple` (no delay); notify-reload treated as notify; forking: see 11.3 |
| Type=dbus, BusName | 5, 7 | ign: started as simple, with a warning in the journal. D-Bus is not built (section 7): a daemon that needs the bus will fail on its own |
| ExecStart, ExecStartPre, ExecStartPost, ExecReload, ExecStop, ExecStopPost, ExecCondition | 55/120, 14/24, 2/5, 20/32, 9/14, 1/2, 1/2 | impl, with the prefixes `-` `@` `:` (`+` `!` `!!` accepted, meaningless), quoting, escapes, `$VAR`/`${VAR}`, `;` between commands, bare names searched in PATH |
| Restart, RestartSec, RestartSteps, RestartMaxDelaySec, RestartPreventExitStatus, RestartForceExitStatus, SuccessExitStatus | 21/27, 4/6, 0, 0, 2/2, 0, 2/2 | impl (backoff is systemd's RestartSteps/RestartMaxDelaySec) |
| TimeoutStartSec, TimeoutStopSec, TimeoutSec | 5/7, 8/11 | impl |
| KillMode, KillSignal, SendSIGKILL, FinalKillSignal | 10/13, 5/7, 2/3 | impl; on main every signal ends a process (11.1) |
| GuessMainPID, PIDFile | 0, 13/18 | impl |
| NotifyAccess | 4/5 | impl: `none` turns it off; anything else accepts messages from any process of the unit (no SO_PEERCRED to check "main") |
| Environment, EnvironmentFile (`-` prefix) | 13/32, 16/22 | impl |
| WorkingDirectory (`-`, `~`) | 2/6 | impl |
| StandardOutput, StandardError: journal, null, inherit, tty, console, file:, append:, truncate:, journal+console, socket | 5/5, 2/2 | impl (tty and console mean the manager's console) |
| StandardInput: null, tty, socket | 3/3 | impl |
| RuntimeDirectory (+Preserve), StateDirectory, CacheDirectory, LogsDirectory, ConfigurationDirectory | 10/13, 3/4, 0, 1/2, 1/1 | impl: created at start, RuntimeDirectory removed at stop |
| RuntimeDirectoryMode, StateDirectoryMode, UMask, LogsDirectoryMode | 5/8, 2/3, 5/8 | ign: no modes on main |
| User, Group, DynamicUser, SupplementaryGroups, AmbientCapabilities, CapabilityBoundingSet, SecureBits, NoNewPrivileges, PermissionsStartOnly | 14/30, 7/13, 2/6, ... | ign: one user, no privileges to drop. Lost: a daemon runs with the same rights as everything else (as all of UC.OS does) |
| ProtectSystem, ProtectHome, PrivateTmp, PrivateDevices, PrivateNetwork, PrivateUsers, ProtectKernel*, ProtectControlGroups, ProtectClock, ProtectHostname, ProtectProc, ProcSubset, ReadWritePaths, ReadOnlyPaths, ...Directories, ExecPaths, NoExecPaths, RestrictAddressFamilies, RestrictNamespaces, RestrictRealtime, RestrictSUIDSGID, LockPersonality, MemoryDenyWriteExecute, SystemCallFilter, SystemCallArchitectures, SystemCallErrorNumber, RemoveIPC, IPAddressAllow/Deny, DevicePolicy, DeviceAllow, KeyringMode, PrivateIPC, PrivateMounts, AppArmorProfile | 23/33, 22/35, 19/28 ... | ign: there are no namespaces, mounts, capabilities or seccomp. Lost: all sandboxing; a unit that relies on PrivateTmp shares /tmp |
| LimitNOFILE, LimitMEMLOCK, LimitCORE, LimitNPROC, TasksMax, Nice, IOScheduling*, CPUSchedulingPolicy, OOMScoreAdjust, OOMPolicy | 8/11 ... | ign: the kernel has fixed limits (64 descriptors per process) and one scheduler |
| MemoryHigh, MemoryMax (also in [Slice]) | 1/1 | impl as hooks: measured every 5 s, MemoryHigh logged, MemoryMax kills with result `oom-kill` (section 12) |
| WatchdogSec | 1/1 | ign (no watchdog pings are checked) |
| SyslogIdentifier, SyslogFacility, IgnoreSIGPIPE, NonBlocking, Slice | | ign |

### [Install]

| Key | Survey | Decision |
|---|---|---|
| WantedBy, RequiredBy | 54/107, 1/1 | impl (`.wants/`, `.requires/` links) |
| Alias | 11/14 | impl |
| Also | 7/9 | impl |
| DefaultInstance, UpheldBy | 0 | impl (UpheldBy only makes the link; Upholds= itself is not implemented) |

### Other unit types

| Type | Survey | Decision |
|---|---|---|
| .target | 1 | impl |
| .timer: OnActiveSec, OnBootSec, OnStartupSec, OnUnitActiveSec, OnUnitInactiveSec, OnCalendar, RandomizedDelaySec, Unit | 15 OnCalendar, 13 Persistent, 12 RandomizedDelaySec, 5 AccuracySec | impl. OnCalendar: every form found in the survey (daily, weekly, `*-*-* 07..23:30`, `Sun *-*-1..7 1:00:00`, `*:00/10` ...) plus the other shorthands, in UTC (UC.OS has no time zones). Persistent: ign (nothing persists; becomes useful with persistence: a stamp file per timer). AccuracySec, FixedRandomDelay: ign (timers fire on time) |
| .socket: ListenStream=/path, Accept=no and yes, Service | 11/28 ListenStream | impl for AF_UNIX paths (section 12.3). refuse: ports and IP addresses (no network on main), abstract names `@...` (the kernel has none), ListenDatagram/SequentialPacket/FIFO/Netlink (kernel has none): the socket unit fails with a message saying so. SocketMode, SocketUser, SocketGroup, RemoveOnStop, BindIPv6Only, KeepAlive, FileDescriptorName: ign |
| .path: PathExists, PathExistsGlob, PathChanged, PathModified, DirectoryNotEmpty, MakeDirectory, Unit | 3 | impl by polling once a second (no inotify); PathChanged and PathModified compare modification time and size |
| .slice | 1 | loaded; only MemoryHigh/MemoryMax mean anything, and only on services today |
| .mount, .automount, .swap, .device, .scope | 0 shipped, referenced | refuse: treated as not found. A `Requires=` on one fails the start (as systemd does for a missing unit); `After=`/`Wants=` on one is harmless |

### Files and naming

| Feature | Decision |
|---|---|
| Comments `#` `;`, sections, continuation lines (comments inside them dropped), CRLF | impl |
| Drop-ins: `NAME.d/*.conf`, the template's `TEMPLATE.d/`, the type's `service.d/` | impl; same file name in a higher-priority directory hides a lower one; `foo-.service.d` prefix drop-ins: not implemented |
| Empty assignment resets a list | impl |
| Templates and instances, specifiers `%n %N %p %P %i %I %j %J %f %t %S %C %L %E %T %V %h %u %U %g %G %H %l %v %s %%` | impl; `%h` is /home/user, `%u` user, `%U` 1000, `%H` 42lisboa (the values UC.OS has) |
| Masking (link to /dev/null) | impl |
| `$SYSTEMD_UNIT_PATH` | one extra directory searched first (systemd's variable takes a list) |

## 4. Architecture options

Option A, the manager is pid 1: one process does init's work (reap orphans, never exit)
and supervises services. Option B, a minimal pid 1 (today's bin/init.c, or smaller)
starts the manager as its child and restarts it if it dies.

| | A: manager as pid 1 | B: separate supervisor |
|---|---|---|
| Orphans | reaped by the manager (waitpid(-1) on each turn) | reaped by init; the manager still finds them through the process group (11.2) |
| A manager crash | takes the system down (the kernel frees init's children when it exits) | init restarts it; running services keep running but the new manager does not know them (it would have to adopt them: not built) |
| Processes | 1 | 2 (one more wasm memory: about 1.5 MiB measured for the manager; init is smaller) |
| `/run/systemd/system`, socket, journal | same | same |
| Code today | built: `init=/bin/svcd` | built: the same program runs under another init (`init /bin/svcd` with bin/init.c's one-program mode, or started from a shell); a dedicated mode that restarts it is not built |

The program built supports both; which one the system boots by default is **open**
(and so is whether the default boot moves to the manager at all; today it does not).

## 5. Where units come from and how programs run

### 5.1 Search path

Highest priority first, for each root: `ROOT/etc/systemd/system`, `ROOT/run/systemd/system`,
`ROOT/usr/local/lib/systemd/system`, `ROOT/usr/lib/systemd/system`,
`ROOT/lib/systemd/system`. Root 0 is UC.OS's own (`""`); root 1 is the Debian root
(`/debian` by default if that directory exists; `--guest-root=DIR` or `GuestRoot=` in
`/etc/systemd/system.conf [Manager]` to change it). UC.OS's directories come first, so a
native unit hides a Debian unit of the same name (**assumed**; reverse by swapping the
roots in `svc_roots_default`). `.wants/` and `.requires/` directories of both roots
apply to every unit (so a Debian package enabling into `multi-user.target` works).

### 5.2 Native or guest

A unit loaded from the Debian root is a guest unit: every path in it (ExecStart and
friends, PIDFile, EnvironmentFile, Condition paths, RuntimeDirectory, ...) is a guest
path and is translated by prefixing the root. Which program runs is decided by the first
bytes of the file, not by the root:

| File starts with | Runs as |
|---|---|
| `\0asm` (a UC.OS program) | itself, with the host path (so a native stand-in can replace a Debian daemon, as the tests do) |
| `\x7fELF` | `/bin/lx --root=ROOT -- PATH ARGS...` (root `/` for a native unit) |
| `#!` in a guest unit | the engine, which reads the `#!` inside the guest root |
| `#!` in a native unit | the named interpreter with the script, as the kernel would |

The engine's path is `GuestEngine=` in system.conf (default `/bin/lx`).

### 5.3 Targets shipped

sysinit, basic, sockets, timers, paths, local-fs, local-fs-pre, remote-fs, remote-fs-pre,
network-pre, network, network-online, nss-lookup, nss-user-lookup, time-set, time-sync,
multi-user, graphical, rescue, shutdown, printer, rpcbind, getty; `default.target` is a
link to graphical.target. They are written for UC.OS (short, with comments), not copied.
`network.target` and `network-online.target` are reached at once although there is no
network: 23 surveyed units order themselves after network-online.target and 3 require it.
Whether these should instead fail until networking exists is **open**.

### 5.4 What the engine must provide (note for uc-os-dev; the engine author has finished)

Measured against `bin/lx` in `.worktrees/lx` (read only):
1. **Running native programs from a guest.** Maintainer scripts run in the guest (Debian's
   dash under lx) and call `systemctl`, `deb-systemd-helper`, `deb-systemd-invoke`,
   `systemd-tmpfiles`, `systemd-sysusers` by name. lx already runs a file with the wasm
   magic as a native program (`run_native` in sys_proc.c). What is missing is that these
   names resolve: either the package manager or the manager puts copies of the native
   programs at `/debian/usr/bin/NAME` (and `/debian/usr/sbin/policy-rc.d` is left alone),
   or lx maps a list of guest paths to native ones. **Open** which.
2. **`/run`**: lx translates every guest path into the root except /dev and /proc. The
   scripts test `/run/systemd/system`, so the manager also creates
   `/debian/run/systemd/system` (done). If lx passed `/run` through instead, one directory
   would do; then `/debian/run` and `/run` would be the same.
3. **Paths given to native helpers by a guest** are guest paths (`DPKG_ROOT`, unit
   names are fine). The helpers here look units up in both roots, so names work.
4. **LISTEN_PID** (12.3) and **NOTIFY_SOCKET** (9): guest daemons linked with
   libsystemd send datagrams and check LISTEN_PID == getpid(). Datagrams do not exist in
   the kernel; LISTEN_PID cannot be known before spawn on main. Either lx translates
   (accepts a stream NOTIFY_SOCKET and adds SENDER_PID; sets LISTEN_PID to its own pid
   when LISTEN_FDS is set) or the kernel gains datagram sockets and an exec.
5. **Exit statuses**: a guest killed by a signal should end the engine with that signal
   (so Restart=on-abnormal works); not checked.

The test to run once lx is merged (prepared: `libsvc/tests/mkdebian.sh` stages the same
units; replace the stand-ins under `/debian/usr/sbin` with the real riscv64 binaries
and their libraries from the packages, keep `deb-tests.sh`): cron.service and
ssh.service (with ssh's real `sshd -t`, Type=notify through point 4) started, reloaded,
stopped; logrotate.service run to completion; postinst of openssh-server run by the
guest's dpkg.

## 6. The desktop

Today: init starts `/bin/wm` (3 tries), then `/bin/term --fb` (3 tries), then `/bin/sh`;
the wm config's `exec bar` and `exec term -e sh /etc/welcome` start the rest.

Built as a proposal, used only with `init=/bin/svcd`: `graphical.target` wants
`display-manager.service`, which is a link (`/etc/systemd/system/display-manager.service`)
to `wm.service`: Restart=on-failure, StartLimitBurst=3, OnFailure=term-fb.service
(`term --fb`, same rules, OnFailure=rescue.service: sh on the console),
SuccessAction=poweroff (a clean `exit` of the wm ends the session, as in init.c).
Verified: `tools/tests/desktop.mjs` under the manager gives the same six screenshots as
the default boot (looked at 02-shell and 04-htop; htop shows `svcd` as pid 1, 1.8 MiB).
Not tested: the fallback chain (wm failing three times).

Options for the wm's own autostart (nothing in bin/wm was changed):

| Option | What | Trade-offs |
|---|---|---|
| D1 (built) | keep `exec bar` and `exec term ...` in the wm config; only the wm is a unit | no change to the wm or its config; bar and terminals are the wm's children, so they are part of wm.service's process group: `systemctl status wm` lists them, `systemctl stop wm` stops them; they are not restarted on their own |
| D2 | bar.service and welcome-term.service as units, `After=wm.service`, remove the exec lines | the manager restarts the bar if it dies; needs the wm to say it is ready (it does not send READY=1: an `ExecStartPre` waiting for /run/wm.sock, or a path unit on /run/wm.sock, or a change in the wm); the config change is the owners' (the dotfiles teach) |
| D3 | `exec` lines run `systemctl start NAME` | the wm stays the place users configure; units give restart and logs |
| D4 | user units (`systemctl --user`) for the session | there is one user and no per-user manager; would need a second manager instance |

## 7. Talking to the manager

An AF_UNIX stream socket (placeholder path `/run/svcd/control`) and a line protocol:
one request line of words, any number of answer lines, then `=rc N` (the exit status
for the client); lines starting with `!` are errors for stderr (`libsvc/proto.h`).
Verbs: start stop restart reload try-restart reload-or-restart try-reload-or-restart
isolate (with `--no-block`, `--job-mode=`), show, list-units, list-jobs, journal (`-u`
`-n` `-f`), daemon-reload, reset-failed, kill, poweroff, reboot, status-manager,
load-all (a measurement hook). A request waits for its jobs, as systemctl does; the
manager never blocks on a client (non-blocking sockets, at most 12 clients, a client
that stops reading is dropped after 4 MiB queued).

What leaving out D-Bus excludes: Type=dbus readiness (BusName= appearing on the bus);
any daemon that needs the system bus (avahi, polkit, udisks2, accountsservice,
systemd-timesyncd, NetworkManager, wpa_supplicant's D-Bus API); `systemctl` from a
real Debian systemd package (it talks D-Bus to /run/systemd/private); `busctl`,
`loginctl`, `hostnamectl`, `timedatectl`; dbus activation (`dbus-org.*.service` aliases).
dbus-daemon itself is a Debian program that could run under the engine later; nothing
here needs it.

## 8. The journal

One ring buffer allocated once at start (default 256 KiB; `RuntimeMaxUse=` in
`/etc/systemd/journald.conf [Journal]`, 4 KiB to 64 MiB). A record is a line plus time,
priority (`<N>` prefixes are understood), unit, identifier (program name) and pid;
about 28 bytes of header. When full, the oldest records go, whatever their unit (one
budget, as journald). Services' stdout and stderr go to a pipe per process; the manager
reads them in its loop. `journalctl` asks the manager; a dump is sent 16 KiB at a time,
so the manager never holds a dump in its own memory. Nothing survives a reload; `-b`
with any offset other than 0 says so. Measured: 10,000 lines from one service, manager
memory 28 pages before and after (1.75 MiB, includes the ring).

Options not built: a budget per unit (one chatty unit cannot evict others; costs a
little bookkeeping), rate limiting (journald's 10,000 messages per 30 s per service),
writing to a file when persistence exists.

## 9. Readiness

`NOTIFY_SOCKET=/run/systemd/notify` is set for Type=notify units (and when NotifyAccess=
is given). It is a **stream** socket: the kernel has no datagrams. A sender connects,
writes `KEY=VALUE` lines and closes; it must add `SENDER_PID=<its pid>` because the
kernel cannot tell the manager who is on the other end (`systemd-notify` also adds
`SENDER_PPID=`: it exits at once, often before the manager reads the message, so its
parent, the service's main process, identifies the unit). Understood: READY=1, STATUS=,
MAINPID=, RELOADING=1, STOPPING=1, ERRNO=. `systemd-notify` (built) and `svctest` do
this; a C helper for native daemons is two dozen lines (in `bin/svctest/main.c`).
Debian daemons linked with libsystemd under the engine will not reach it as they are:
they open a datagram socket (5.4, point 4). Their units then time out (TimeoutStartSec,
90 s by default) unless the engine translates.

## 10. Enabled state and persistence

Enabling makes links exactly where systemd puts them
(`/etc/systemd/system/multi-user.target.wants/foo.service` and so on). The kernel on
main has no symlinks, so a **link file** stands in: a regular file whose whole content
is one absolute path and a newline. Every reader (`svc_readlink`) accepts both; the
writer makes a real symlink when `symlinkat` works (detected by -ENOSYS, no weak
symbols) and a link file otherwise. So when the vfs work lands, new links are real
symlinks and old link files still read. Links in the Debian root point to guest paths
and are resolved inside that root. `default.target` and masks (link to /dev/null) work
the same way.

What survives a reload of the page: nothing on main (the image is the same each time),
so `systemctl enable` lasts until the reload. With persistence (vfs stream), the files
under /etc are what persist; no other state is needed.

`deb-systemd-helper enable UNIT` maps onto the same `svc_enable` as `systemctl enable`
(links in the unit's own root, so a Debian unit gets its links under
`/debian/etc/systemd/system`), plus Debian's state:
`ROOT/var/lib/systemd/deb-systemd-helper-enabled/UNIT.dsh-also` (the links made) and
`ROOT/var/lib/systemd/deb-systemd-helper-masked/UNIT`. `was-enabled` is true with no
state (a first install) or when every listed link exists; so an admin's `disable`
survives an upgrade (tested). The man page (deb-systemd-helper(1p), trixie) describes
the commands but not the exit statuses or the state paths; those follow what the
generated maintainer scripts expect, and are this implementation's reading.

## 11. Process control

### 11.1 What the kernel offers

On main: spawn, waitpid (pid, -1, 0, -pgid, WNOHANG), kill (always terminates; signal
0 tests; process groups not accepted), setpgid (any value, by the parent),
procinfo (pid, ppid, state, pages, name; no pgid: the manager asks getpgid per pid),
AF_UNIX stream sockets, pipes. In the fork stream's tree (not merged): signals with
handlers, kill(-pgid), SIGCHLD, fork, execve, stop/continue, pmigrate.

The manager is written for main and finds out at start what the kernel does
(`rt_sigprocmask` answering -ENOSYS means no signals; `readlinkat` -ENOSYS means no
symlinks). It needs no weak symbols: every richer call is reached by its number.
Consequences on main:
- **stop** sends KillSignal (SIGTERM), which ends the process; the SIGKILL step after
  TimeoutStopSec exists and is tested with the fake process layer and on the host, and
  in the OS only through a hanging ExecStop;
- **reload** runs ExecReload; if one of its commands is `kill ... $MAINPID` and signals
  are not delivered, the reload becomes a restart and the journal says so (sending
  SIGHUP would end the daemon; with Restart=on-failure it would not even come back,
  because SIGHUP counts as a clean exit);
- **child exits** are found by waitpid(WNOHANG) each turn of the loop, which wakes every
  100 ms while jobs run or units change state and every second otherwise (measured idle
  cost: about 7 system calls per second). With SIGCHLD the tick could go: not built.

### 11.2 All processes of a service

There are no cgroups. Each service gets its own process group (setpgid right after
spawn, before the child has run: one thread, and the parent runs on until it blocks);
its control processes join it. A unit's processes are the members of that group plus
everything whose parent is a member (a child that called setsid() leaves the group
but keeps its parent while the parent lives). Tested in the OS: a service with two
children shows three processes, stop ends all three; a forking service whose starter
exits leaves its daemon, which is found through the PID file and the group, and stop
ends it.
Holes: a grandchild that calls setsid() and whose parent then exits is lost (its
parent becomes init); a process can setpgid itself out. A kernel-side tag inherited
across spawn (a cgroup in miniature: one number per process, set by a private call,
readable in procinfo, usable by kill) would close both: **open**, a request for the
kernel if the owners want it.
Orphans: with the manager as pid 1 they come to it and are reaped; they stay counted
in their unit through the group.

### 11.3 Type=forking without fork

On main a native program cannot daemonise. The rule: the start completes when the
starter exits cleanly (main pid from PIDFile=, else the only process left in the group),
or as soon as PIDFile= names a live process. A native program that never forks and never
writes its PID file times out after TimeoutStartSec. Debian daemons under the engine use
its emulated fork (the child runs in the same process until it execs or exits), so a
daemonising guest never lets its "parent" exit: the PID-file rule is what makes those
units start (not tested: needs the engine).

## 12. Memory and efficiency

### 12.1 Per-unit limits

What can be measured today: the pages of each process (procinfo), summed over a unit's
processes every 5 s; shown as MemoryCurrent/MemoryPeak in `show` and "Memory:" in
`status`. Hooks in `core_measure()`:
- MemoryHigh= exceeded: logged once per crossing;
- MemoryMax= exceeded: the service is killed, result `oom-kill`, then its Restart= policy
  applies (tested in the OS with MemoryMax=4M and an 8 MiB allocation).
Not built (the memory stream decides the policy; see .track/streams/mem.md and
docs/MEMORY.md in .worktrees/mem): ask a service to compact (with signals, send it a
signal it handles by calling migrate(); the call exists only in the fork tree), restart
idle services, budgets across units. The natural place is `core_measure()`; what it
needs from the kernel is already there except pmigrate (fork tree) and a way to tell a
process "compact now" (a signal).

### 12.2 Idle policy

Not built. The pieces that exist: socket activation (a service starts on its first
connection and can exit when idle; the socket keeps listening) and the per-unit memory
figure. A policy "stop a service idle for N minutes" needs a notion of idle: CPU time
from procinfo (cpu_us) or no connections.

### 12.3 Socket activation

Tested in the OS: a socket unit listens on `/run/svct-echo.sock`; the first connection
starts the service, which finds the listening socket as descriptor 3 with LISTEN_FDS=1,
accepts, answers; when the service ends, the socket is watched again and a second
connection starts it again. How the descriptor gets to 3: the kernel's spawn passes
every descriptor not marked close-on-exec at the same number. The manager keeps all its
descriptors close-on-exec and parks /dev/null on 3 to 6 at start, so it can put a
listener there for the length of the spawn call. LISTEN_PID is not set: the child's pid
is not known before spawn returns, and libsystemd's sd_listen_fds() ignores LISTEN_FDS
without it (native programs here do not check it). With execve (fork tree) a tiny helper
could set it and exec the daemon. Accept=yes (one instance per connection, connection on
stdin and stdout) is implemented and tested only with the fake process layer of the
native core tests.

### 12.4 Start-up cost

Measured under Node (load about 3): reading every unit file of the sample (151 units
plus the targets they name, 204 in all) takes 56 ms the first time; `daemon-reload` of
those 204 takes 11 to 14 ms. From the manager's first instruction to graphical.target
(wm started): 7, 10, 9 ms in three boots. No compiled cache: the numbers do not need one.
Pending: the same in Chromium; the cost of compiling the manager's 134 KB module versus
init's.

### 12.5 The manager's own memory

1.5 MiB (24 pages) idle at boot; 1.75 MiB (28 pages) after the test units are loaded;
28 pages after 1,000 start and stop cycles (1,000 starts counted) and after 10,000
journal lines; 2.75 MiB (44 pages) with all 204 units of the sample loaded. The fixed
tables (up to 768 units of 4.4 KiB each as measured on the host build, allocated as they
are loaded, and 256 jobs) and the journal ring
set the ceiling; units made for one connection (Accept=yes) are freed when they end.

## 13. The rest of the system

- **Boot order** with the manager as pid 1: sysinit.target (local-fs) ... basic.target
  (sockets, timers, paths) ... multi-user.target ... graphical.target with
  display-manager.service. The kernel console shows `[  OK  ] Started ...` lines and
  "Startup finished in N ms".
- **Shutdown**: there is no power-off in a browser tab. `systemctl poweroff` (and halt)
  stops every unit in reverse order, then the manager exits; as pid 1 it prints "System
  halted. Reload the page to start again." and the tab shows the last frame.
  `systemctl reboot` is a soft reboot: everything stops, then the default target starts
  again in the same tab (**assumed**). Options: a host call to reload the page (a change
  in boot/, the owners'), or treat reboot like poweroff.
- **ps and htop** show `svcd` as pid 1 (or as a child of init), each service's processes
  with their own pids; nothing else changes.
- **User versus system services**: there is one user (uid 1000) and one manager.
  `systemctl --user` is refused with a message; `%h` and `%u` give /home/user and user;
  services get HOME=/home/user, USER=user, PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
  (**assumed**: init gives PATH=/bin; with the users of the vfs stream this becomes
  User=-dependent).

## 14. Open questions for the owners

1. Names: the manager program (`svcd` is a placeholder; not "systemd"), its control
   socket (`/run/svcd/control`), its configuration keys `GuestRoot=` and `GuestEngine=`
   in system.conf, the test helper `svctest`, the hidden `systemctl load-all`.
2. Default boot: keep bin/init.c (today) or boot the manager; A or B of section 4.
3. Desktop: D1 (built) to D4 of section 6, and whether the fallback chain stays.
4. `systemctl reboot` in a tab: soft reboot (built), page reload (needs boot/), or poweroff.
5. Native programs reachable from guest maintainer scripts: copies in /debian/usr/bin
   (manager, package manager) or a mapping in lx (5.4).
6. network.target and network-online.target reached with no network (5.3).
7. ConditionVirtualization: not virtualised (built), "vm", or a new value.
8. A process tag in the kernel for exact tracking (11.2).
9. Journal: one budget (built) or per unit, rate limiting.
10. Whether to ship shims for units Debian expects from the systemd package
    (syslog.socket: rsyslog.service Requires it and fails without it; it needs
    datagrams, so it cannot work today).

## 15. Tests and measurements

| Suite | Command | Result (2026-11-26) |
|---|---|---|
| libsvc, core, journal (native, ASan + UBSan) | `sh libsvc/tests/run.sh` | 67/67, 114/114, journal 2/2 (200,000 records through a 16 KiB ring) |
| all 162 surveyed unit files and 548 host unit files parse | `build-svc-native/test_svc parse FILES` | 0 problems (the one sysusers.d file matched by the find is not a unit) |
| Exec lines versus the host's `systemctl show` | `python3 -I libsvc/tests/compare-systemctl.py build-svc-native/test_svc FILES` | 1,275 (unit, key) pairs over 255 units equal, 313 of them with commands |
| host integration (svcd + systemctl on the host, real processes) | `sh libsvc/tests/host-integration.sh` | 28/28 |
| in the OS, manager as pid 1 | `node tools/run.mjs --build=B --init=/bin/svcd --args="--unit-path=/usr/share/tests/svc --unit=svc-selftest.target"` | 81/81 |
| 1,000 start and stop cycles | same with `--unit=svc-cycles.target` | 4/4; 1,000 starts counted; 1,344 ms; manager 28 pages before and after |
| real Debian units with native stand-ins | `sh libsvc/tests/mkdebian.sh B build-scratch/x B-deb`, then run with `--unit=svc-debian.target` | 31/31 |
| system suites, default boot | `sh tools/test.sh B` | ktest 109, libctest 290, sh-tests 238, all passed |
| desktop, default boot and manager as pid 1 | `node tools/shot.mjs --build=B --script=tools/tests/desktop.mjs [--init=/bin/svcd]` | six screenshots each, looked at |

Not run: Chromium (`tools/browser.mjs`); the fork tree (signals, SIGKILL after a
SIGTERM that is caught, kill(-pgid)); the engine; the desktop fallback chain; timing
on a quiet machine below load 3 for all figures (the load was 2.9 to 3.8).

## 16. Files

New: `libsvc/` (svc.h unitfile.c cal.c names.c os.h os.c proto.h, tests/), `bin/svcd/`,
`bin/systemctl/`, `bin/journalctl/`, `bin/deb-systemd-helper/`,
`bin/deb-systemd-invoke/`, `bin/systemd-tmpfiles/`, `bin/systemd-sysusers/`,
`bin/systemd-notify/`, `bin/svctest/`, `rootfs/usr/lib/systemd/system/*`,
`rootfs/etc/systemd/system/display-manager.service`, `rootfs/usr/share/tests/svc/`,
this file. Changed: nothing outside these (no libc change was needed: sockets are reached
by raw system calls, as libui does). The libsvc sources are compiled into each program
through its `CFLAGS` file, so `make` does not notice a change in `libsvc/` alone:
use a fresh `B=` or touch the program's main.c (a one-line Makefile rule would fix it:
the owners' call).
