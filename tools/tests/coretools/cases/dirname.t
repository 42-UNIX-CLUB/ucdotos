# dirname
%setup
%cases
dirname /usr/bin/sort
dirname /usr/bin/
dirname /usr
dirname /
dirname //
dirname ///
dirname //a//b//
dirname a
dirname a/
dirname ./a
dirname ../a/b
dirname ''
dirname a b/c /d/e/f
dirname -z a/b c/d | od -c
dirname
dirname -- -a/b
dirname -x a
