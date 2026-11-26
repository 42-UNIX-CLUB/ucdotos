# fold
%setup
printf 'The quick brown fox jumps over the lazy dog and keeps running far away from here\n' > long
printf 'a\tb\tc\td\te\n' > tabs
printf 'short\n\nempty above\n' > short
printf 'nonewline at the end of this rather long line of text' > nonl
printf 'abc\bd\re\n' > ctl
%cases
fold long
fold -w 20 long
fold -w 20 -s long
fold -w 1 short
fold -w 10 short
fold -w 5 tabs
fold -b -w 5 tabs
fold -w 9 tabs
fold -w 10 nonl
fold -s -w 10 nonl
fold -w 3 ctl
fold -b -w 3 ctl
fold -10 long
fold --width=15 long
fold -sw 7 long
fold -w 0 long; echo "st=$?"
fold -w x long; echo "st=$?"
fold missing; echo "st=$?"
fold -q long; echo "st=$?"
: | fold
fold -w 4 - < short
printf 'aaaaaaaaaaaaaaaaaaaaaaaaaa\n' | fold -s -w 5
printf 'a b c d e f g h\n' | fold -s -w 4
printf 'ab  cd\n' | fold -s -w 3
seq 1 200 | tr '\n' ' ' | fold -w 50 -s | wc -l
