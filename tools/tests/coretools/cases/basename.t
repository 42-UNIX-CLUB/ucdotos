# basename and dirname
%setup
%cases
basename /usr/bin/sort
basename /usr/bin/
basename /usr//bin//
basename /
basename //
basename ///
basename ''
basename a
basename a.txt .txt
basename .txt .txt
basename a.txt txt
basename a.txt a.txt
basename dir/a.c .c
basename dir/a.c/ .c
basename -a a/b c/d /e
basename -s .c x.c y.c z.h
basename --suffix=.c x.c
basename -as .c x.c y.c
basename --multiple a/b c/
basename -z a/b | od -c
basename a b c
basename
basename -q x
basename -- -x
basename -s
