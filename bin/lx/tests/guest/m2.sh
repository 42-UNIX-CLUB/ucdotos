# SPDX-License-Identifier: GPL-2.0-or-later
# M2: Debian's busybox-static running a script. Run as: busybox sh m2.sh BUSYBOX TMPDIR
# Every check prints "ok" or "FAIL" and the script ends with a count.
BB=${1:-/bin/busybox}
T=${2:-/tmp}
pass=0; fail=0
check() { # name expected actual
  if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1: expected [$2] got [$3]"; fi
}
check "arith" 42 "$((6*7))"
check "subst" sub "$(echo sub)"
x=0; for i in 1 2 3 4 5; do x=$((x+i)); done
check "loop" 15 "$x"
f() { echo "f:$1"; }
check "function" "f:a" "$(f a)"
(exit 3); check "subshell status" 3 "$?"
check "pipe" b "$(echo a | $BB tr a b)"
check "case" yes "$(case foo in f*) echo yes;; *) echo no;; esac)"
printf 'one\ntwo\nthree\n' > $T/lx-m2.txt
check "cat" "one two three" "$($BB cat $T/lx-m2.txt | $BB tr '\n' ' ' | $BB sed 's/ $//')"
check "wc" 3 "$($BB wc -l < $T/lx-m2.txt)"
check "grep" two "$($BB grep tw $T/lx-m2.txt)"
check "grep -c" 2 "$($BB grep -c o $T/lx-m2.txt)"
check "sort" "one three two" "$($BB sort $T/lx-m2.txt | $BB tr '\n' ' ' | $BB sed 's/ $//')"
check "sort -r" "two" "$($BB sort -r $T/lx-m2.txt | $BB head -1)"
check "sed" "ONE" "$($BB sed -n '1s/one/ONE/p' $T/lx-m2.txt)"
check "awk" "3 11" "$($BB awk '{n++; c+=length($0)} END {print n, c}' $T/lx-m2.txt)"
check "awk fields" "b" "$(echo 'a b c' | $BB awk '{print $2}')"
check "sha256sum" "2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae" "$(printf foo | $BB sha256sum | $BB cut -d' ' -f1)"
check "md5sum" "acbd18db4cc2f85cedef654fccc4a4d8" "$(printf foo | $BB md5sum | $BB cut -d' ' -f1)"
$BB seq 1 2000 > $T/lx-m2-big.txt
check "seq" 2000 "$($BB wc -l < $T/lx-m2-big.txt)"
$BB gzip -c $T/lx-m2-big.txt > $T/lx-m2-big.gz
check "gzip round trip" "$($BB sha256sum < $T/lx-m2-big.txt)" "$($BB gzip -dc $T/lx-m2-big.gz | $BB sha256sum)"
cd $T && $BB tar -cf lx-m2.tar lx-m2.txt lx-m2-big.txt && cd - > /dev/null
check "tar list" "lx-m2.txt lx-m2-big.txt" "$($BB tar -tf $T/lx-m2.tar | $BB tr '\n' ' ' | $BB sed 's/ $//')"
check "tar extract" "two" "$(cd $T && $BB tar -xOf lx-m2.tar lx-m2.txt | $BB sed -n 2p)"
check "ls" "lx-m2.txt" "$($BB ls $T | $BB grep '^lx-m2.txt$')"
check "expr" 6 "$($BB expr 2 \* 3)"
check "printf" "  7|x" "$($BB printf '%3d|%s' 7 x)"
check "od" "0000000 141 142" "$(printf ab | $BB od -b | $BB head -1 | $BB sed 's/ *$//')"
check "test" yes "$([ -f $T/lx-m2.txt ] && echo yes)"
check "dc" 1024 "$(echo '2 10 ^ p' | $BB dc)"
check "date" 1970 "$($BB date -u -d @0 +%Y)"
check "basename" c "$($BB basename /a/b/c)"
check "env" "LXV=1" "$(LXV=1 $BB env | $BB grep '^LXV=')"
check "xargs" "1 2 3" "$(printf '1\n2\n3\n' | $BB xargs echo)"
check "find" "$T/lx-m2.txt" "$($BB find $T -name lx-m2.txt)"
check "cut" "b" "$(echo a:b:c | $BB cut -d: -f2)"
check "uniq" "a b" "$(printf 'a\na\nb\n' | $BB uniq | $BB tr '\n' ' ' | $BB sed 's/ $//')"
check "head/tail" "1999" "$($BB tail -2 $T/lx-m2-big.txt | $BB head -1)"
check "rm" "gone" "$($BB rm $T/lx-m2.tar && [ ! -e $T/lx-m2.tar ] && echo gone)"
echo "m2: $((pass+fail)) checks, $pass passed, $fail failed"
