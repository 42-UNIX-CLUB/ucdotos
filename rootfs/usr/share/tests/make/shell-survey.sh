# SPDX-License-Identifier: GPL-2.0-or-later
# Survey of the shell features that makefile recipes commonly use. Each check
# runs a snippet with "sh -c", as make does, and compares the output with what
# a POSIX shell prints. Run:  sh /usr/share/tests/make/shell-survey.sh
pass=0
fail=0
t() { # t NAME EXPECTED SNIPPET
  got=$(sh -c "$3" 2>&1)
  if [ "$got" = "$2" ]; then pass=$((pass + 1)); echo "ok    $1"
  else fail=$((fail + 1)); echo "FAIL  $1"; echo "      snippet:  $3"; echo "      expected: [$2]"; echo "      got:      [$got]"; fi
}
cd /tmp
t "and-or"            "$(printf 'a\nc')"        'true && echo a; false && echo b; false || echo c'
t "semicolons"        "$(printf '1\n2')"        'echo 1; echo 2'
t "subshell"          "$(printf 'in\nout')"     '(cd /; echo in); echo out'
t "subshell-cd"       "/tmp"       '(cd /); pwd'
t "group"             "g"          '{ echo g; }'
t "cmd-subst"         "x y"        'echo $(echo x) `echo y`'
t "nested-subst"      "abc"        'echo $(echo a$(echo b)c)'
t "for-loop"          "1 2 3 "      'for i in 1 2 3; do printf "%s " $i; done; echo'
t "for-glob"          "ok"         'mkdir -p sv; touch sv/a.c sv/b.c; for f in sv/*.c; do :; done; echo ok'
t "while-loop"        "3"          'i=0; while [ $i -lt 3 ]; do i=$((i+1)); done; echo $i'
t "if-test"           "yes"        'if [ -d / ]; then echo yes; else echo no; fi'
t "test-builtin"      "1"          'test -f /bin/sh && echo 1'
t "test-string"       "eq"         '[ "a" = "a" ] && echo eq'
t "test-not"          "not"        '[ ! -f /nope ] && echo not'
t "test-z-n"          "zn"         '[ -z "" ] && [ -n x ] && echo zn'
t "case"              "c-file"     'case x.c in *.c) echo c-file;; *) echo other;; esac'
t "heredoc"           "line1"      'cat <<EOF
line1
EOF'
t "heredoc-var"       "v=5"        'v=5; cat <<EOF
v=$v
EOF'
t "heredoc-quoted"    'v=$v'       'cat <<"EOF"
v=$v
EOF'
t "set-e"             "a"          'set -e; echo a; false; echo b'
t "set-x"             "$(printf '+ echo x\nx')"          'set -x; echo x 2>/dev/null'
t "redirect-out"      "hi"         'echo hi > sv/r; cat sv/r'
t "redirect-append"   "1 2"        'echo 1 > sv/a; echo 2 >> sv/a; echo $(cat sv/a)'
t "redirect-err"      ""           'ls /nope 2>/dev/null'
t "redirect-both"     "e"          'sh -c "echo e >&2" 2>&1'
t "redirect-in"       "hi"         'cat < sv/r'
t "pipe"              "c"          'printf "a\nb\nc\n" | tail -1'
t "pipe-status"       "1"          'true | false; echo $?'
t "glob"              "sv/a.c sv/b.c" 'echo sv/*.c'
t "glob-nomatch"      "sv/*.zz"    'echo sv/*.zz'
t "dollar-var"        "5"          'x=5; echo $x'
t "braces"            "5x"         'x=5; echo ${x}x'
t "default"           "d"          'echo ${nope:-d}'
t "strip-suffix"      "file"       'f=file.c; echo ${f%.c}'
t "arith"             "7"          'echo $((3 + 4))'
t "exit-status"       "3"          'sh -c "exit 3"; echo $?'
t "export-env"        "v"          'export V=v; sh -c "echo \$V"'
t "assign-prefix"     "w"          'W=w sh -c "echo \$W"'
t "cd-and"            "/"          'cd / && pwd'
t "mkdir-p-and"       "ok"         'mkdir -p sv/x/y && echo ok'
t "test-newer"        "newer"      'touch sv/old; sleep 1; touch sv/new; [ sv/new -nt sv/old ] && echo newer'
t "command-v"         "found"      'command -v sh >/dev/null && echo found'
t "printf-format"     "a-01"       'printf "%s-%02d\n" a 1'
t "echo-n"            "ab"         'echo -n a; echo b'
t "backslash-newline" "a b"        'echo a \
b'
t "single-quote-nl"   "$(printf 'a\\\nb')" "echo 'a\\
b'"
t "trap"              "bye"        'trap "echo bye" EXIT; true'
t "shift-args"        "b"          'set -- a b; shift; echo $1'
t "dollar-hash"       "2"          'set -- a b; echo $#'
t "functions"         "f1"         'f() { echo f$1; }; f 1'
t "exec"              "e"          'exec echo e'
t "colon"             "ok"         ': && echo ok'
t "not-pipeline"      "0"          '! false; echo $?'
t "or-group"          "b"          'false || { echo b; }'
t "read"              "x"          'echo x | { read v; echo $v; }'
t "tr-sed"            "B"          'echo b | tr a-z A-Z'
t "eval"              "v"          'eval "x=v"; echo $x'
t "dollar-dollar"     "ok"         'p=$$; [ -n "$p" ] && echo ok'
t "unset"             "[]"         'x=1; unset x; echo "[$x]"'
t "rm-f-missing"      "0"          'rm -f sv/none; echo $?'
t "cp-r"              "ok"         'cp -r sv sv2 && echo ok'
t "test-e"            "e"          '[ -e /bin ] && echo e'
t "multiline-if"      "y"          'if true
then echo y
fi'
echo "shell-survey: $((pass + fail)) checks, $pass passed, $fail failed"
