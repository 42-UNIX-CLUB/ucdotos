# SPDX-License-Identifier: GPL-2.0-or-later
# M3/M4 inside a guest root: dynamically linked Debian programs (glibc, ld.so), dash and
# bash running commands. Run as: dash m34.sh   (inside the engine, root = a Debian root)
pass=0; fail=0
check() { if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1: expected [$2] got [$3]"; fi; }
check "dash arith" 42 "$((6*7))"
check "dash subst" sub "$(echo sub)"
check "coreutils echo (dynamic)" hi "$(/usr/bin/echo hi)"
check "coreutils ls (dynamic)" /usr/bin "$(/usr/bin/ls -d /usr/bin)"
check "cat" "x" "$(echo x | /usr/bin/cat)"
check "sort" "a b c " "$(printf 'c\nb\na\n' | /usr/bin/sort | /usr/bin/busybox tr '\n' ' ')"
check "bash -c" "3 z" "$(/usr/bin/bash -c 'a=(x y z); echo ${#a[@]} ${a[2]}')"
check "bash function and pipe" "6" "$(/usr/bin/bash -c 'f() { echo $(($1*2)); }; f 3' | /usr/bin/cat)"
check "exit status" 7 "$(/usr/bin/bash -c 'exit 7'; echo $?)"
check "busybox from dash" "B" "$(echo b | /usr/bin/busybox tr b B)"
echo "m34: $((pass+fail)) checks, $pass passed, $fail failed"
