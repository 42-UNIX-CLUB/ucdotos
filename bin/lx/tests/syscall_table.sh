#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the table "program x system call x count" from real Debian programs run by the
# engine (natively). Each program runs with LX_STATS=1; the counts of every engine
# process it starts (fork children included) are summed.
#   LX=/path/to/lx LX_SCRATCH=/dir sh syscall_table.sh > table.md
set -u
R=$LX_SCRATCH/root
HERE=$(cd "$(dirname "$0")" && pwd)
cp "$HERE/guest/m2.sh" "$R/tmp/lx-m2.sh"
OUT=$(mktemp -d)
run() { # name, then the command
  n=$1; shift
  LX_STATS=1 LANG=C timeout 60 nice -n 19 "$LX" --root="$R" "$@" > /dev/null 2> "$OUT/$n.err" < /dev/null
}
run busybox-m2 /bin/busybox sh /tmp/lx-m2.sh /bin/busybox /tmp
run dash /bin/dash -c 'for i in 1 2 3; do echo $i; done; ls / > /dev/null'
run bash /bin/bash -c 'a=(1 2); echo ${a[1]}; ls / > /dev/null'
run ls /bin/ls -la /usr/bin
run dpkg /usr/bin/dpkg --version
run apt-get /usr/bin/apt-get --version
run perl /usr/bin/perl -e 'print 42'
run python3 /usr/bin/python3.13 -c 'print(1)'
# merge: one row per call, one column per program
awk -v progs="busybox-m2 dash bash ls dpkg apt-get perl python3" '
  FNR == 1 { split(FILENAME, p, "/"); prog = p[length(p)]; sub(/\.err$/, "", prog) }
  /^  [a-z_0-9]+ +[0-9]+ +[0-9]+$/ { c[$1][prog] += $3; nr[$1] = $2; tot[$1] += $3 }
  END {
    n = split(progs, P, " ");
    printf "| call (nr) |"; for (i = 1; i <= n; i++) printf " %s |", P[i]; print "";
    printf "|---|"; for (i = 1; i <= n; i++) printf "---|"; print "";
    PROCINFO["sorted_in"] = "@ind_str_asc";
    for (k in c) { printf "| %s (%d) |", k, nr[k]; for (i = 1; i <= n; i++) printf " %s |", (P[i] in c[k]) ? c[k][P[i]] : ""; print "" }
  }' "$OUT"/*.err
grep -h "returned -ENOSYS\|ENOSYS" "$OUT"/*.err 2>/dev/null | sort | uniq -c | sort -rn | head -20
