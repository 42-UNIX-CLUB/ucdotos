# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for /bin/sh and the core utilities of UC.OS.
# Run:  sh /usr/share/tests/sh-tests.sh
# Every check prints "ok" or "FAIL"; the last line is the count.

pass=0
fail=0
check() { # check NAME EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
    echo "ok    $1"
  else
    fail=$((fail + 1))
    echo "FAIL  $1"
    echo "      expected: [$2]"
    echo "      got:      [$3]"
  fi
}
T=/tmp/sh-tests.$$
mkdir -p $T
cd $T

echo "--- quoting and expansion"
check "single quotes" 'a  $HOME b' "$(echo 'a  $HOME b')"
check "double quotes keep spaces" 'x   y' "$(echo "x   y")"
check "backslash escapes" 'a$b"c' "$(echo a\$b\"c)"
check "escape in double quotes" 'q"$' "$(echo "q\"\$")"
v=hello
check "simple variable" hello "$(echo $v)"
check "braced variable" hello_world "$(echo ${v}_world)"
check "default value" fallback "$(echo ${nope:-fallback})"
check "assign default" set "$(echo ${newvar:=set}; echo $newvar | cat >/dev/null)"
check "alternate value" alt "$(echo ${v:+alt})"
check "string length" 5 "${#v}"
f=archive.tar.gz
check "remove suffix" archive.tar "${f%.*}"
check "remove long suffix" archive "${f%%.*}"
check "remove prefix" tar.gz "${f#*.}"
check "remove long prefix" gz "${f##*.}"
check "tilde" /home/user "$(HOME=/home/user; echo ~)"
check "tilde path" /home/user/x "$(HOME=/home/user; echo ~/x)"
check "pid is a number" yes "$(case $$ in *[!0-9]*) echo no;; *) echo yes;; esac)"
false
check "status of false" 1 "$?"
true
check "status of true" 0 "$?"
check "arithmetic" 17 "$((2 + 3 * 5))"
i=5
check "arithmetic with variables" 30 "$((i * (i + 1)))"
check "arithmetic compare" 1 "$((3 < 4 && 2 >= 2))"
check "arithmetic ternary" 7 "$((i > 3 ? 7 : 9))"
: $((i += 2))
check "arithmetic assignment" 7 "$i"
check "hex and modulo" 3 "$((0x1f % 4))"
c=visible # a comment; c=hidden
check "comments" "visible" "$c"
check "empty quotes are an argument" 3 "$(set -- a '' b; echo $#)"

echo "--- field splitting and \$@"
words="one two   three"
set -- $words
check "splitting unquoted" 3 "$#"
set -- "$words"
check "no splitting quoted" 1 "$#"
set -- a "b c" d
check "positional count" 3 "$#"
check "positional 2" "b c" "$2"
n=0; for x in "$@"; do n=$((n + 1)); done
check '"$@" keeps arguments' 3 "$n"
n=0; for x in $@; do n=$((n + 1)); done
check '$@ unquoted splits' 4 "$n"
check '"$*" joins' "a b c d" "$*"
shift
check "shift" "b c" "$1"
IFS=:
set -- $(echo a:b:c)
unset IFS
check "custom IFS" "3 b" "$# $2"

echo "--- command substitution"
check "dollar paren" "abc" "$(echo abc)"
check "backquotes" "def" "`echo def`"
check "nested substitution" "inner" "$(echo $(echo inner))"
check "substitution strips newlines" "x" "$(printf 'x\n\n\n')"
check "substitution with pipeline" "3" "$(printf 'a\nb\nc\n' | wc -l)"
check "big substitution" "20000" "$(seq 1 20000 | tail -1)"
check "substitution of a builtin with lots of output" 100000 "$(i=0; while [ $i -lt 10000 ]; do echo 123456789; i=$((i + 1)); done | wc -c)"
x=$(false)
check "status of substitution" 1 "$?"

echo "--- globbing"
mkdir g
touch g/a.txt g/b.txt g/c.md g/.hidden
check "star" "g/a.txt g/b.txt" "$(echo g/*.txt)"
check "question mark" "g/a.txt g/b.txt" "$(echo g/?.txt)"
check "brackets" "g/a.txt g/c.md" "$(echo g/[ac].*)"
check "negated brackets" "g/b.txt" "$(echo g/[!ac].*)"
check "no match stays literal" "g/*.zip" "$(echo g/*.zip)"
check "quoted star is literal" "g/*" "$(echo "g/*")"
check "hidden files need a dot" "g/.hidden" "$(echo g/.h*)"
cd g
check "glob in cwd" "a.txt b.txt c.md" "$(echo *)"
cd ..

echo "--- pipelines and redirections"
check "two stages" "1" "$(echo hello | grep -c hello)"
check "three stages" "c" "$(printf 'b\nc\na\n' | sort | tail -1)"
check "four stages" "2" "$(printf 'x\ny\nx\nz\n' | sort | uniq | grep -v z | wc -l)"
check "large data through pipes" "20000 19999 19998" "$(seq 1 20000 | sort -rn | head -3 | { read a; read b; read c; echo "$a $b $c"; })"
check "five stages, large data" "6878" "$(seq 1 20000 | grep 7 | sort | uniq | wc -l)"
echo one > out.txt
echo two >> out.txt
check "> and >>" "one two" "$(cat out.txt | { read a; read b; echo "$a $b"; })"
check "< input" "one" "$(head -1 < out.txt)"
ls /nonexistent 2> err.txt
check "2> captures errors" 1 "$(grep -c nonexistent err.txt)"
check "2>&1 into a pipe" 1 "$(ls /nonexistent 2>&1 | grep -c nonexistent)"
{ echo out; ls /nonexistent; } &> both.txt
check "&> both streams" 2 "$(wc -l < both.txt)"
check "redirect a builtin" "builtin" "$(echo builtin > b.txt; cat b.txt)"
check "stdout restored after builtin redirect" "after" "$(echo x > /dev/null; echo after)"
check "redirect a compound command" "1 2" "$(for i in 1 2; do echo $i; done > loop.txt; cat loop.txt | { read a; read b; echo "$a $b"; })"
check "while read from a file" "one+two+" "$(while read l; do printf '%s+' $l; done < out.txt)"
check "here-document" "hi there" "$(cat <<EOF
hi ${v2}there
EOF
)"
v2=""
check "here-document expansion" "x=5" "$(x=5; cat <<EOF
x=$x
EOF
)"
check "quoted here-document" 'x=$x' "$(cat <<'EOF'
x=$x
EOF
)"
check "exec redirection persists" "persist" "$( (exec > ex.txt; echo persist); cat ex.txt)"
check "pipeline status is the last stage" 0 "$(false | true; echo $?)"
check "negated pipeline" 0 "$(! false; echo $?)"

echo "--- lists"
check "and" "yes" "$(true && echo yes)"
check "and short-circuit" "" "$(false && echo yes)"
check "or" "no" "$(false || echo no)"
check "or short-circuit" "" "$(true || echo no)"
check "sequence" "$(printf 'a\nb')" "$(echo a; echo b)"
check "group" "x y" "$({ echo x; echo y; } | { read a; read b; echo "$a $b"; })"
check "group keeps variables" "set" "$({ gv=set; }; echo $gv)"
check "subshell keeps cwd" "$T" "$( (cd /; true); pwd)"
sleep 0.2 &
bgpid=$!
check "background pid" yes "$(case $bgpid in [0-9]*) echo yes;; esac)"
wait
check "wait for background" 0 "$?"

echo "--- control flow"
if true; then r=then; else r=else; fi
check "if then" then "$r"
if false; then r=then; elif true; then r=elif; else r=else; fi
check "elif" elif "$r"
if false; then r=then; else r=else; fi
check "else" else "$r"
i=0; while [ $i -lt 5 ]; do i=$((i + 1)); done
check "while" 5 "$i"
i=0; until [ $i -ge 3 ]; do i=$((i + 1)); done
check "until" 3 "$i"
s=""; for w in a b c; do s="$s$w"; done
check "for" abc "$s"
s=""; for w in 1 2 3 4 5; do [ $w = 4 ] && break; s="$s$w"; done
check "break" 123 "$s"
s=""; for w in 1 2 3 4; do [ $w = 2 ] && continue; s="$s$w"; done
check "continue" 134 "$s"
s=""; for a in 1 2; do for b in x y; do [ $b = y ] && continue 2; s="$s$a$b"; done; done
check "continue 2" 1x2x "$s"
s=""; for a in 1 2; do for b in x y; do s="$s$a$b"; break 2; done; done
check "break 2" 1x "$s"
classify() {
  case $1 in
    [0-9]*) echo number ;;
    *.txt|*.md) echo document ;;
    "") echo empty ;;
    *) echo other ;;
  esac
}
check "case number" number "$(classify 42)"
check "case alternatives" document "$(classify notes.md)"
check "case default" other "$(classify zzz)"
check "case empty" empty "$(classify '')"
multi=$(for i in 1 2 3
do
  echo $i
done | wc -l)
check "multi-line loop" 3 "$multi"

echo "--- functions"
greet() { echo "hello $1"; }
check "function call" "hello world" "$(greet world)"
add() { return $(($1 + $2)); }
add 2 3
check "return status" 5 "$?"
count_args() { echo $#; }
check "function arguments" 4 "$(count_args a b c d)"
outer=global
scope() { local outer=local; echo $outer; }
check "local variable" local "$(scope)"
scope >/dev/null
check "local restored" global "$outer"
fact() { if [ $1 -le 1 ]; then echo 1; else echo $(($1 * $(fact $(($1 - 1))))); fi; }
check "recursion" 120 "$(fact 5)"
early() { echo before; return 3; echo after; }
check "return stops the function" before "$(early)"
check "positional parameters restored" "a" "$1"
check "function in a pipeline" "x-hello" "$(greet x | while read a b; do echo "$b-$a"; done)"

echo "--- builtins"
check "echo -n" "ab" "$(echo -n a; echo b)"
check "echo -e" "a	b" "$(echo -e 'a\tb')"
check "printf" "x=  42|str|ff" "$(printf 'x=%4d|%s|%x' 42 str 255)"
check "pwd" "$T" "$(pwd)"
check "cd and pwd" "/tmp" "$(cd /tmp && pwd)"
check "cd -" "$T" "$(cd /; cd - > /dev/null; pwd)"
check "cd to HOME" "/home/user" "$(HOME=/home/user; cd; pwd)"
export EXPORTED=yes
check "export reaches children" yes "$(sh -c 'echo $EXPORTED')"
NOTEXP=no
check "unexported stays private" "" "$(sh -c 'echo $NOTEXP')"
check "prefix assignment" here "$(PREFIX=here sh -c 'echo $PREFIX')"
check "prefix assignment is temporary" "" "$PREFIX"
unset v
check "unset" "" "$v"
check "set lists variables" 1 "$(set | grep -c '^EXPORTED=')"
alias greetme='echo hi from alias'
check "alias" "hi from alias" "$(eval greetme)"
unalias greetme
check "unalias" 127 "$(eval greetme 2>/dev/null; echo $?)"
echo 'sourced=yes; srcarg=$1' > src.sh
. ./src.sh
check "source with ." yes "$sourced"
source ./src.sh arg1
check "source with arguments" arg1 "$srcarg"
read a b <<EOF
first second third
EOF
check "read splits" "first|second third" "$a|$b"
echo 'raw\line' > raw.txt
read -r line < raw.txt
check "read -r" 'raw\line' "$line"
check "read at end of file" 1 "$(read x < /dev/null; echo $?)"
check "test -f" 0 "$(test -f out.txt; echo $?)"
check "test -d" 0 "$([ -d g ]; echo $?)"
check "test -e missing" 1 "$([ -e nothing ]; echo $?)"
check "test string =" 0 "$([ abc = abc ]; echo $?)"
check "test string !=" 0 "$([ abc != abd ]; echo $?)"
check "test -z -n" "0 0" "$([ -z '' ]; echo $?) $([ -n x ]; echo $?)"
check "test numbers" "0 1 0" "$([ 3 -lt 10 ]; echo $?) $([ 3 -gt 10 ]; echo $?) $([ 7 -eq 7 ]; echo $?)"
check "test ! -a -o" "0 1 0" "$([ ! -e nothing ]; echo $?) $([ 1 = 1 -a 1 = 2 ]; echo $?) $([ 1 = 2 -o 2 = 2 ]; echo $?)"
check "test parentheses" 0 "$([ \( 1 = 1 \) ]; echo $?)"
check "type builtin" "cd is a shell builtin" "$(type cd)"
check "type program" "ls is /bin/ls" "$(type ls)"
check "type function" "greet is a function" "$(type greet)"
check "exec replaces the shell" "x" "$(exec echo x; echo not-reached)"
check "eval" "6" "$(eval 'x=3; echo $((x * 2))')"
check "command not found" 127 "$(nosuchcommand 2>/dev/null; echo $?)"
check "sh -c with arguments" "b 2" "$(sh -c 'echo $2 $#' zero a b)"
check "exit status of sh -c" 7 "$(sh -c 'exit 7'; echo $?)"
echo 'echo script $1; exit 4' > s.sh
check "sh file args" "script x" "$(sh s.sh x)"
check "script exit status" 4 "$(sh s.sh >/dev/null; echo $?)"
check "script without the execute bit is refused (126)" 126 "$(./s.sh y 2>/dev/null; echo $?)"
chmod +x s.sh
check "script without sh" "script y" "$(./s.sh y)"
check "script on stdin" "piped 3" "$(echo 'echo piped $((1+2))' | sh)"

echo "--- utilities"
check "ls" "a.txt b.txt c.md" "$(ls g | { read a; read b; read c; echo "$a $b $c"; })"
check "ls -a shows dotfiles" 1 "$(ls -a g | grep -c '^\.hidden$')"
check "ls -1 count" 3 "$(ls -1 g | wc -l)"
check "ls -l format" 1 "$(ls -l g | grep -c '^-rw.* a\.txt$')"
check "cat -n" "     1	one" "$(cat -n out.txt | head -1)"
check "head -n" "1 2" "$(seq 1 10 | head -n 2 | { read a; read b; echo "$a $b"; })"
check "tail -n" "10" "$(seq 1 10 | tail -n 1)"
check "tail -n +N" "9 10" "$(seq 1 10 | tail -n +9 | { read a; read b; echo "$a $b"; })"
check "wc -l -w -c" "2 2 8" "$(printf 'one\ntwo\n' | wc -l) $(printf 'one\ntwo\n' | wc -w) $(printf 'one\ntwo\n' | wc -c)"
check "grep basic regex" "2" "$(printf 'cat\ncot\ncut\ndog\n' | grep -c 'c[ao]t')"
check "grep anchors" "dog" "$(printf 'cat\ndog\nhotdog\n' | grep '^d.*g$')"
check "grep star" "3" "$(printf 'ac\nabc\nabbbc\nabd\n' | grep -c 'ab*c')"
check "grep -i" 2 "$(printf 'Hello\nhello\nbye\n' | grep -ci hello)"
check "grep -v" "bye" "$(printf 'Hello\nhello\nbye\n' | grep -vi hello)"
check "grep -n" "2:b" "$(printf 'a\nb\n' | grep -n b)"
check "grep -E" 2 "$(printf 'color\ncolour\ncolr\n' | grep -cE 'colou?r$' )"
check "grep -E alternation" 2 "$(printf 'cat\ndog\nbird\n' | grep -cE 'cat|dog')"
mkdir r r/sub; echo needle > r/sub/n.txt; echo hay > r/h.txt
check "grep -r" "r/sub/n.txt:needle" "$(grep -r needle r)"
check "sort" "a b c" "$(printf 'c\na\nb\n' | sort | { read x; read y; read z; echo "$x $y $z"; })"
check "sort -r" "c" "$(printf 'c\na\nb\n' | sort -r | head -1)"
check "sort -n" "2 10 100" "$(printf '100\n2\n10\n' | sort -n | { read x; read y; read z; echo "$x $y $z"; })"
check "sort -u" 2 "$(printf 'a\na\nb\n' | sort -u | wc -l)"
check "uniq -c" "      2 a" "$(printf 'a\na\nb\n' | uniq -c | head -1)"
mkdir -p deep/er/est
check "mkdir -p" 0 "$([ -d deep/er/est ]; echo $?)"
echo data > deep/er/est/file
cp -r deep copy
check "cp -r" data "$(cat copy/er/est/file)"
cp out.txt out2.txt
check "cp file" "one" "$(head -1 out2.txt)"
mv out2.txt moved.txt
check "mv" "0 1" "$([ -f moved.txt ]; echo $?) $([ -f out2.txt ]; echo $?)"
rm moved.txt
check "rm" 1 "$([ -f moved.txt ]; echo $?)"
rm -r copy
check "rm -r" 1 "$([ -d copy ]; echo $?)"
rm -f nothing-here
check "rm -f quiet" 0 "$?"
rm deep/er/est/file
rmdir deep/er/est
check "rmdir" 1 "$([ -d deep/er/est ]; echo $?)"
touch newfile
check "touch" 0 "$([ -f newfile ]; echo $?)"
check "tee" "t t" "$(echo t | tee tee.txt) $(cat tee.txt)"
check "seq" "1 2 3" "$(seq 3 | { read a; read b; read c; echo "$a $b $c"; })"
check "seq first incr last" "10 7 4" "$(seq -s ' ' 10 -3 4)"
check "yes | head" "y y" "$(yes | head -2 | { read a; read b; echo "$a $b"; })"
check "echo program" "prog" "$(/bin/echo prog)"
check "true and false programs" "0 1" "$(/bin/true; echo $?) $(/bin/false; echo $?)"
check "pwd program" "$T" "$(/bin/pwd)"
check "env" 1 "$(env | grep -c '^EXPORTED=yes$')"
check "env VAR=x cmd" "x" "$(env FOO=x sh -c 'echo $FOO')"
check "which" "/bin/ls" "$(which ls)"
check "uname" "UC.OS" "$(uname)"
check "uname -a has the machine" 1 "$(uname -a | grep -c wasm32)"
check "date +FORMAT" "1970-01-01 00:00:00 Thu" "$(date -d @0 '+%Y-%m-%d %H:%M:%S %a')"
check "date epoch" "Thu Jan  1 00:00:00 UTC 1970" "$(date -u -d @0)"
check "date 2038" "2038-01-19 03:14:07" "$(date -d @2147483647 '+%F %T')"
check "date now is after 2020" 1 "$([ $(date +%Y) -ge 2020 ] && echo 1)"
check "sleep 0.1" 0 "$(sleep 0.1; echo $?)"
check "ps lists itself" 1 "$(ps | grep -c ' ps$')"
check "ps header" "PID PPID STATE MEM SYSCALLS TIME NAME" "$(ps | head -1 | { read a b c d e f g; echo "$a $b $c $d $e $f $g"; })"
sleep 5 &
sp=$!
kill $sp
wait $sp
check "kill" 143 "$?"
check "hexdump -C" "00000000  68 69 0a                                          |hi.|" "$(echo hi | hexdump -C | head -1)"
check "clear" "1b5b481b5b324a" "$(clear | hexdump -C | head -1 | { read off a b c d e f g rest; echo "$a$b$c$d$e$f$g"; })"
check "man" 1 "$(man sh | grep -c '^NAME')"
check "man section" 1 "$(man 7 unix-club | grep -c '^UNIX-CLUB(7) ')"
check "man missing" 16 "$(man nothing-at-all 2>/dev/null; echo $?)"

echo "--- long command lines"
check "echo /bin/* (long glob)" "$(ls /bin | wc -l)" "$(echo /bin/* | wc -w)"
check "long glob to a program" "$(ls /bin | wc -l)" "$(/bin/echo /bin/* | wc -w)"
check "5000 arguments to a program" 5000 "$(set -- $(seq 1 5000); /bin/echo "$@" | wc -w)"
check "5000 arguments to a builtin" 5000 "$(echo $(seq 1 5000) | wc -w)"
check "5000-argument for loop" 5000 "$(n=0; for i in $(seq 1 5000); do n=$((n + 1)); done; echo $n)"
touch tt; touch tt
check "touch an existing file" 0 "$?"
sleep 1 &
sleep 0.3
check "ps shows a sleeping process as S" 1 "$(ps | grep -c ' S .* sleep$')"
wait

echo "--- bc"
check "bc 2^200" "1606938044258990275541962092341162602522202993782792835301376" "$(echo '2^200' | bc)"
check "bc scale=50 1/7" ".14285714285714285714285714285714285714285714285714" "$(echo 'scale=50; 1/7' | bc)"
check "bc precedence" "7" "$(echo '1+2*3' | bc)"
check "bc integer division" "3" "$(echo '10/3' | bc)"
check "bc modulo" "1" "$(echo '7 % 3' | bc)"
check "bc negative" "-6" "$(echo '-2*3' | bc)"
check "bc sqrt" "1.414213562373095048801688724209" "$(echo 'scale=30; sqrt(2)' | bc)"
check "bc big product" "121932631137021795226185032733622923332237463801111263526900" "$(echo '123456789012345678901234567890 * 987654321098765432109876543210' | bc)"
check "bc obase=16" "FF" "$(echo 'obase=16; 255' | bc)"
check "bc obase=2" "1010" "$(echo 'obase=2; 10' | bc)"
check "bc ibase=16" "255" "$(echo 'ibase=16; FF' | bc)"
check "bc ibase=8" "8" "$(echo 'ibase=8; 10' | bc)"
check "bc variables" "15" "$(echo 'a=5; b=a*3; b' | bc)"
check "bc comparison" "1 0" "$(echo '3 < 4; 4 == 5' | bc | { read a; read b; echo "$a $b"; })"
check "bc logic" "1 0 1" "$(echo '!0; 1 && 0; 0 || 2' | bc | { read a; read b; read c; echo "$a $b $c"; })"
check "bc if" "yes" "$(echo 'if (2 > 1) "yes"' | bc)"
check "bc while" "1024" "$(echo 'x = 1; while (x < 1000) x = x * 2; x' | bc)"
check "bc for" "5050" "$(echo 's = 0; for (i = 1; i <= 100; i++) s += i; s' | bc)"
check "bc function" "265252859812191058636308480000000" "$(printf 'define f(n) {\n  if (n <= 1) return (1)\n  return (n * f(n - 1))\n}\nf(30)\n' | bc)"
check "bc auto variables" "10 1" "$(printf 'define g(x) { auto y; y = x * 2; return (y) }\ny = 1\ng(5)\ny\n' | bc | { read a; read b; echo "$a $b"; })"
check "bc arrays" "14" "$(echo 'a[3] = 7; a[3] * 2' | bc)"
check "bc 2^1000 wraps lines" "5" "$(echo '2^1000' | bc | wc -l)"
check "bc 2^1000 line width" "71" "$(echo '2^1000' | bc | head -1 | wc -c)"
check "bc scale of fractions" "3.1428571428" "$(echo 'scale=10; 22/7' | bc)"
check "bc -l e(1)" "2.71828182845904523536" "$(echo 'e(1)' | bc -l)"
check "bc -l pi" "3.14159265358979323844" "$(echo '4*a(1)' | bc -l)"
check "bc quit" "1" "$(printf '1\nquit\n2\n' | bc)"

# SIGPIPE, as on Linux: a writer whose reader is gone is killed (status 141); the shell
# itself ignores SIGPIPE but gives the programs it starts the default
check "yes | head -1 ends (yes is killed by SIGPIPE)" "y" "$(yes | head -1)"
check "seq 100000 | head -3" "1 2 3" "$(seq 100000 | head -3 | { read a; read b; read c; echo $a $b $c; })"
seq 50000 > big.txt
check "cat bigfile | true" "0" "$(cat big.txt | true; echo $?)"
check "pipefail: a stage killed by SIGPIPE gives 141" "141" "$(sh -c 'set -o pipefail; yes | head -1 >/dev/null; echo $?')"
check "without pipefail the last stage decides" "0" "$(sh -c 'yes | head -1 >/dev/null; echo $?')"
check "a builtin writing to a closed pipe: the loop stops, the shell lives" "y y alive" "$(sh -c 'while :; do echo y; done | head -2; echo alive' | { read a; read b; read c; echo $a $b $c; })"
check "trap '' PIPE is inherited: yes gets EPIPE and exits 1" "1" "$(sh -c "set -o pipefail; trap '' PIPE; yes | head -1 >/dev/null; echo \$?")"
check "a shell started with SIGPIPE ignored keeps it ignored for its children" "1" "$(trap '' PIPE; sh -c 'set -o pipefail; yes | head -1 >/dev/null; echo $?')"
check "trap - PIPE does not make the shell killable by its own writes" "alive" "$(sh -c 'trap - PIPE; while :; do echo y; done | head -1 >/dev/null; echo alive')"
check "exec of a writer: SIGPIPE ends it" "y" "$(sh -c 'exec yes' | head -1)"
check "a background writer is killed by SIGPIPE too" "y" "$(sh -c '{ yes | head -1; } & wait')"

cd /
rm -r $T
echo "sh-tests: $((pass + fail)) checks, $pass passed, $fail failed"
