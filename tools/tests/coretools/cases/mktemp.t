# mktemp (names are random: the cases check their shape and what was created)
%setup
mkdir t
%cases
@fresh f=$(mktemp -p t); case $f in t/tmp.??????????) echo shape-ok;; *) echo "bad $f";; esac; test -f "$f" && echo is-file
@fresh f=$(mktemp -d -p t); test -d "$f" && echo is-dir
@fresh f=$(mktemp t/fooXXXXXX); case $f in t/foo??????) echo ok;; esac; test -f "$f" && echo file
@fresh f=$(mktemp t/a.XXX); case $f in t/a.???) echo ok;; esac
@fresh f=$(mktemp t/XXXXXX.txt); case $f in t/??????.txt) echo ok;; esac; test -f "$f" && echo file
@fresh f=$(mktemp --suffix=.c t/xXXXX); case $f in t/x????.c) echo ok;; esac
f=$(mktemp -u t/uXXXXXX); case $f in t/u??????) echo ok;; esac; test -e "$f" || echo not-created
@fresh f=$(TMPDIR=$PWD/t mktemp); case $f in "$PWD"/t/tmp.*) echo in-tmpdir;; esac
@fresh f=$(TMPDIR=$PWD/t mktemp -t zzXXXX); case $f in "$PWD"/t/zz????) echo t-ok;; esac
@fresh f=$(mktemp --tmpdir=t qXXXXXX); case $f in t/q??????) echo ok;; esac
@fresh a=$(mktemp -p t); b=$(mktemp -p t); test "$a" != "$b" && echo different
mktemp t/XX; echo "st=$?"
@fresh f=$(mktemp t/aXXXb); echo "st=$?"; case $f in t/a???b) echo ok;; esac
mktemp -q t/XX; echo "st=$?"
mktemp nodir/XXXXXX; echo "st=$?"
mktemp -t a/XXXXXX; echo "st=$?"
mktemp a b; echo "st=$?"
mktemp -z; echo "st=$?"
@fresh f=$(mktemp -d t/dXXXXXX); ls -ld "$f" | cut -c1-10
@fresh f=$(mktemp t/mXXXXXX); ls -l "$f" | cut -c1-10
