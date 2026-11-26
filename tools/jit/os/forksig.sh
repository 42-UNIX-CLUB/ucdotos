# SPDX-License-Identifier: GPL-2.0-or-later
# Inside UC.OS, after inst.sh: fork, exec and signals with guest programs (the integration
# engineer's forkt.sh and sig.sh checks, plus a guest that forks 100 times). Output in /tmp/out.
mkdir -p /tmp/out
export PATH=$PATH:/debian/usr/bin
{
n=0; f=0
t() { if [ "$2" = "$3" ]; then n=$((n+1)); echo "ok   $1"; else f=$((f+1)); echo "FAIL $1: [$2] wanted [$3]"; fi; }
t "pipeline of three" "$(echo hello | cat | cat)" hello
t "sort in a pipeline" "$(printf '3\n1\n2\n' | sort | busybox tr '\n' ' ')" "1 2 3 "
t "nested command substitution" "$(echo $(echo inner))" inner
t "subshell into a pipe" "$( (echo a; echo b) | busybox tr '\n' ' ')" "a b "
busybox sleep 0.2 & p=$!; wait $p; t "background job and wait" $? 0
( exit 7 ); t "subshell exit status" $? 7
busybox sleep 0.3 & busybox sleep 0.3 & wait; t "two background jobs" $? 0
t "300 KB through a pipe (more than the 64 KiB buffer)" "$(busybox seq 1 50000 | busybox wc -l)" 50000
t "producer and consumer both alive" "$( { echo early; busybox sleep 0.2; echo late; } | busybox tr '\n' ' ')" "early late "
x=$(busybox sh -c 'echo $PPID'); t "a child's parent is this shell" "$x" "$$"
echo "forkt: $((n+f)) checks, $n passed, $f failed"
} > /tmp/out/forkt 2>&1
lx /bin/bash -c 'n=0; for i in $(/usr/bin/seq 1 100); do ( exit $((i % 7)) ); n=$((n + $?)); done; echo "forks done, sum $n"; x=$(for i in 1 2 3; do echo $i; done | /usr/bin/wc -l); echo "subst $x"' > /tmp/out/fork100 2>&1
lx /bin/bash -c '/usr/bin/echo exec ok; exec /usr/bin/echo exec replaced the shell' > /tmp/out/exec 2>&1
{
echo "--- 1 guest signals itself"
lx /bin/bash -c 'trap "echo got TERM" TERM; kill -TERM $$; echo after'
echo "--- 3 SIGCHLD trap in the guest"
lx /bin/bash -c 'trap "echo got CHLD" CHLD; busybox true; busybox sleep 0.2; echo end'
echo "--- 4 SIGINT to a guest from kill -INT"
lx /bin/bash -c 'trap "echo got INT; exit 4" INT; busybox sleep 2; echo slept' &
sleep 1; kill -INT $!; wait $!; echo "status $?"
echo "--- 2 kernel SIGTERM to a guest with a TERM trap"
lx /bin/bash -c 'trap "echo got TERM; exit 3" TERM; echo ready; busybox sleep 2; echo slept' &
sleep 1; kill -TERM $!; wait $!; echo "status $?"
} > /tmp/out/sig 2>&1
cp $(find /repo -name 'hello_*.deb' | head -1) /debian/tmp/hello.deb
lx /usr/bin/dpkg-deb -c /tmp/hello.deb > /tmp/out/debc 2>&1; echo "dpkg-deb -c exit $?" >> /tmp/out/debc
