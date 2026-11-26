# sed: the stream editor
%setup
printf 'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n' > n
printf 'hello world\nfoo bar baz\n\nHello World\nthe end\n' > t
printf 'no newline' > nonl
printf 'a\nb' > nonl2
printf 'key1=value1\n# comment\nkey2 = value2\n\nkey3=a=b\n' > conf
printf 'x\ny\n' > small
printf 'A\nB\n' > other
printf 'line with\ttab\nback\\slash\n\001\002\377\n' > odd
printf 's/o/0/g\n/^t/d\n' > script.sed
printf '#n\n/two/p\n' > quiet.sed
%cases
# basics
sed '' n
sed p small
sed -n p small
sed d n
sed 3d n
sed '$d' n
sed '1d;$d' n
sed -n '3p' n
sed -n '$p' n
sed -n '2,4p' n
sed -n '4,2p' n
sed -n '/three/,/six/p' n
sed -n '/three/,+2p' n
sed -n '/two/,~4p' n
sed -n '0,/o/p' n
sed -n '1,/o/p' n
sed -n '2~3p' n
sed -n '0~4p' n
sed -n '5~0p' n
sed -n '$!p' small
sed '2,8!d' n
sed -n '/e$/p' n
sed -n '/^t/!p' n
sed -n '\,o,p' n
sed -n '\|e|p' n
sed -n '/HELLO/Ip' t
sed -n '/o/,/o/p' n
sed -n '/seven/,3p' n
sed -n '8,$p' n
sed -n '$=' n
sed = small
sed -n '/e/=' n
# s command
sed s/o/0/ n
sed s/o/0/g n
sed s/e/E/2 n
sed s/e/E/2g n
sed 's/[aeiou]/(&)/g' n
sed 's/\(.\)\(.\)/\2\1/' n
sed -E 's/(.)(.)/\2\1/' n
sed -E 's/(o+)|(e+)/[\1|\2]/g' n
sed 's/o\+/O/g' t
sed 's/l\{2\}/LL/' t
sed -E 's/l{2}/LL/' t
sed 's/x*/-/g' small
sed 's/o*/X/g' t
sed 's/$/;/' n
sed 's/^/> /' n
sed 's/^$/EMPTY/' t
sed 's/.*/[&]/' t
sed 's/\&/and/' t
sed 's/o/\&/g' t
sed 's/o/\\/g' t
sed 's|/|:|g' conf
sed 's,=,: ,' conf
sed 's/=/\n/' conf
sed 's/ /\t/g' t
sed 's/\t/TAB/' odd
sed 's/\\/BS/' odd
sed 's/[[:space:]]*=[[:space:]]*/=/' conf
sed -n 's/^\([^=]*\)=\(.*\)$/\2=\1/p' conf
sed 's/[[:upper:]]/\l&/g' t
sed 's/\w\+/\u&/g' t
sed -E 's/(\w+) (\w+)/\U\1\E \2/' t
sed 's/.*/\U&/' t
sed 's/.*/\L&/' t
sed -E 's/(h)(e)/\u\1\U\2x\Ey/' t
sed 's/o/0/gp' t
sed -n 's/o/0/gp' t
sed 's/O/0/gI' t
sed 's/hello/bye/i' t
sed 's/a\|e/_/g' t
sed -E 's/a|e/_/g' t
sed 's/\<the\>/THE/' t
sed 's/\bfoo\b/F/' t
sed 's/o\B/0/g' t
sed 's/^\(.*\)\1$/dup:\1/' t
sed -E 's/(.)\1/<\1\1>/g' n
sed 's/a.b/X/' t
sed 's.o\.r.X.' t
@fresh sed 's/x/y/w out' small; cat out
@fresh sed -n 's/o/0/w out' n; cat out
sed 's/e/E/3' n
sed 's/[^aeiou]//g' n
sed 's/[]]/X/' t
sed 's/[a-]/X/g' t
sed 's/\x6f/0/g' t
sed 's/\o157/0/g' t
sed 's/\d111/0/g' t
sed 's/x/\x41/' small
sed 's/./\cA/' small | od -c
sed 's/o/\n/g' t
sed 'N;s/\n/+/' n
sed '$!N;s/\n/ /' n
sed ':a;N;$!ba;s/\n/,/g' n
sed -e :a -e '$!N;s/\n//;ta' small
sed 's/e/E/;s/E/F/' n
sed -e 's/o/0/' -e 's/e/3/' n
sed -f script.sed n
sed -f quiet.sed n
sed --expression=s/o/0/ n
# y
sed y/abc/xyz/ t
sed 'y/elo/ELO/' t
sed 'y/\n/ /' n
sed 'N;y/\n/|/' n
sed 'y/abc/xy/' t
sed 'y,a\,b,x;y,' t
# a i c
sed '2a\
> appended' small
sed '2i\
> inserted' small
sed '1c\
> changed' small
sed 'a hello' small
sed 'i\  indented' small
sed '$a\
> end' n
sed '1i\
> first\
> second' small
sed '/two/,/four/c\
> gone' n
sed '2,4!c X' n
sed '$!c X' small
sed -n '2{p;a\
> after
> }' n
sed '1a one' -e '1a two' small
sed -e '1a\' -e 'joined' small
sed 'a\' small
sed 'a foo\tbar' small
sed '2q;a x' n
# hold space
sed -n 'h;n;G;p' n
sed -n '1!G;h;$p' n
sed 'x;$!d' n
sed '1!G;h;$!d' n
sed -n 'H;${x;s/\n/,/g;p}' n
sed 'G' small
sed 'g' small
sed -n '/two/{x;p;x;p}' n
sed '1h;2,$H;$!d;g' small
# n N D P
sed n n
sed -n n small
sed 'n;d' n
sed '$!N;P;D' n
sed 'N;P;D' n
sed 'N;D' n
sed 'N;N;s/\n/+/g' n
sed N small
sed -n 'N;l' small
sed '$!N;/^\(.*\)\n\1$/!P;D' small
printf 'a\na\nb\nb\nb\nc\n' | sed '$!N;/^\(.*\)\n\1$/!P;D'
sed 'P;d' small
sed -n 'N;P' n
# branches
sed ':a;s/^.\{1,7\}$/ &/;ta' n
sed -e ':x' -e 's/o/0/' -e 'tx' t
sed 's/o/0/;T;s/$/ (changed)/' t
sed 's/zzz//;t;s/$/!/' small
sed 'b end;s/./X/;:end' small
sed -n '/two/b skip;p;:skip' n
sed '/one/b;s/$/ x/' n
sed ':a;$q;N;4,$D;ba' n
# q Q
sed 3q n
sed 3Q n
sed -n '3{p;q}' n
sed '2q7' n; echo "status=$?"
sed '2Q3' n; echo "status=$?"
sed q nonl
sed '1q' small other
# l w r R W F z
sed -n l odd
sed -n 'l 5' n
sed -n 'l 0' t
sed -l 8 -n l t
@fresh sed -n '/two/w out' n; cat out
@fresh sed 'w out' small > /dev/null; cat out
@fresh sed -n 'W out' small; cat out
sed '1r other' small
sed 'r other' small
sed '$r missing' small
sed '/x/R other' small
sed 'R other' n
sed F small
sed -n '$F' small other
sed z small
sed 'z;s/^$/empty/' small
sed 'w /dev/stdout' small
# blocks and negation
sed -n '/t/{/e/p}' n
sed -n '/t/{/e/!p}' n
sed '2,4{s/^/[/;s/$/]/}' n
sed -n '1!{$!p}' small
sed -n '/one/,/three/{/two/!p}' n
sed '{p};{p}' small
sed -n '2{p};4{p}' n
sed '1 ! d' n
sed -n '/x/ { p ; p }' small
sed -n '3,${p;}' n
# options
sed -n -e 2p -e 4p n
sed -s -n '$p' small other
sed -n '$p' small other
sed -s 1d small other
sed '1d' small other
sed -E 's/(x|y)+/Z/' small
sed -r 's/(x|y)+/Z/' small
sed --regexp-extended 's/(x)/[\1]/' small
sed --quiet 1p small
sed -z 's/\n/,/g' n
printf 'a\000b\000' | sed -z 's/^/>/' | od -c
sed --posix -n 1p small
sed -u p small
sed --separate -n 1p small other
# in place
@fresh sed -i 's/x/X/' small; cat small
@fresh sed -i.bak 's/x/X/' small; cat small small.bak
@fresh sed -i -e 1d small other; cat small other
@fresh sed --in-place=.orig 2d n; cat n.orig | wc -l; wc -l < n
@fresh sed -n -i 2p small; cat small
sed -i 'bak*' 1d small; ls; cat baksmall
@fresh mkdir d; cp small d/f; sed -i'old_*' 's/y/Y/' d/f; cat d/f d/old_f
@fresh sed -i '$a end' nonl; cat nonl
@fresh sed -i 1q n; cat n
@fresh sed -i s/a/b/ missing small; echo "status=$?"; cat small
@fresh sed -s -i '1i top' small other; cat small other
# missing newline at end
sed s/no/yes/ nonl
sed p nonl
sed -n p nonl2
sed 'a end' nonl
sed '$!d' nonl2
sed G nonl
sed '' nonl nonl2
sed 'N;N' nonl2
sed = nonl
# odd input
sed s/o/0/ odd
sed 's/[^[:print:]]/?/g' odd
sed -n '/\x01/p' odd | od -c
: | sed p
printf '\n\n\n' | sed -n '$='
printf '\n' | sed 's/^$/E/'
seq 1 5000 | sed -n '$p'
seq 1 5000 | sed -n '/^49.*9$/p' | wc -l
seq 1 2000 | tr -d '\n' | sed 's/9/N/g' | wc -c
awk 'BEGIN{for(i=0;i<3000;i++)printf "ab";print ""}' | sed 's/\(ab\)*/X/'
sed -n '/one/p' - < n
sed 1d - small < other
# errors
sed
sed 'k' n
sed 's/a/b' n
sed 's/a/b/q' n
sed 's/\(a/b/' n
sed 's/a/\1/' n
sed -E 's/(a)/\2/' n
sed 'y/ab/c/' n
sed '3,4{p' n
sed '}' n
sed 'b nolabel' n
sed ':a;:a' n
sed '1,2,3p' n
sed '0p' n
sed '0,5p' n
sed '1,2q' n
sed 'p x' n
sed -n p missing; echo "status=$?"
sed -n p missing small; echo "status=$?"
sed -f missing.sed n
sed -i p
sed --bogus p n
sed -X p n
sed 's/[/x/' n
sed 's/a\{3,1\}/x/' n
sed '/a/I' n
sed 'a' n
sed '1~' n
sed -n '/[[:nope:]]/p' n
sed 's/x/y/gg' small
sed 's/x/y/0' small
sed '1#comment' small
sed '#comment
> p' small
sed -n '1{p
> }
> 2p' small
sed -n '/one/,/two/ {
>   s/o/0/
>   p
> }' n
sed '1{
> a\
> multi\
> line
> }' small
sed '/x/{
> s/x/1/
> b
> }
> s/$/ not x/' small
# idioms from configure scripts, Makefiles and Debian maintainer scripts
printf 'VERSION = 1.2.3\nNAME=foo\n' | sed -n 's/^VERSION *= *//p'
printf '@prefix@/bin\n@VERSION@\n' | sed -e 's|@prefix@|/usr|g' -e 's|@VERSION@|1.0|g'
printf 'foo.c bar.c\n' | sed 's/\.c/.o/g'
printf 'a/b/c.txt\n' | sed 's,.*/,,'
printf 'a/b/c.txt\n' | sed 's,/[^/]*$,,'
printf '  lead and trail  \n' | sed 's/^[ \t]*//;s/[ \t]*$//'
printf 'x\n\n\ny\n\n' | sed '/^$/d'
printf 'x\n\n\n\ny\n' | sed '/^$/N;/\n$/D'
printf '# c\nreal\n  # indented\n' | sed -e '/^[[:space:]]*#/d'
printf 'Package: foo\nVersion: 1.0-2\nDepends: libc6\n' | sed -n 's/^Version: //p'
printf 'Package: foo\nStatus: install ok installed\n\nPackage: bar\n' | sed -n '/^Package: foo$/,/^$/p'
printf 'conffile /etc/foo\n' | sed -e 's/^conffile //'
printf 'CFLAGS=-O2\n' | sed 's/^CFLAGS=.*/CFLAGS=-O0 -g/'
printf 'a\nb\nc\n' | sed -n '$!{h;d};x;G;p'
printf 'line1\nline2\n' | sed = | sed 'N;s/\n/\t/'
printf 'one two three\n' | sed 's/[^ ]* //2'
printf 'aaa bbb ccc\n' | sed 's/\([a-z]*\) \([a-z]*\)/\2 \1/'
printf 'hello\n' | sed 'y/abcdefghijklmnopqrstuvwxyz/ABCDEFGHIJKLMNOPQRSTUVWXYZ/'
printf 'path:/usr/bin:/bin\n' | sed 's/:/\n/g' | sed -n '2,$p'
printf '1\n2\n3\n4\n5\n6\n' | sed -n 'p;n'
printf '1\n2\n3\n4\n5\n6\n' | sed -n 'n;p'
printf '1\n2\n3\n4\n5\n6\n' | sed '1~2d'
printf '1\n2\n3\n4\n5\n6\n' | sed 'n;n;s/$/ third/'
printf '1\n2\n3\n4\n5\n' | sed -e :a -e '$q;N;3,$D;ba'
printf '1\n2\n3\n4\n5\n' | sed '$!d'
printf '1\n2\n3\n4\n5\n' | sed -n '2{n;p}'
printf 'abc\n' | sed 's/b*/x/2'
printf 'abc\n' | sed 's/b*/x/3'
printf 'aaa\n' | sed 's/a/b/3'
printf 'aaa\n' | sed 's/a/b/4'
printf 'foo123bar\n' | sed -E 's/[0-9]+/(&)/'
printf 'foo123bar\n' | sed 's/[0-9]\{2\}/<&>/g'
printf 'x=1\n' | sed "s/x=\(.*\)/\1/"
printf 'a.b.c\n' | sed 's/\./\//g'
printf 'tab\there\n' | sed 's/\t/\\t/'
printf 'A-B\n' | sed 's/-/\
> /'
printf 'one\n' | sed 's/one/a\
> b\
> c/'
printf 'ab\n' | sed 's/\(a\)\|b/[\1]/g'
printf 'ab\n' | sed -E 's/(a)|b/[\1]/g'
printf 'aXbXc\n' | sed 's/X/\n/2'
printf 'abc\n' | sed -E 's/()/x/g'
printf 'abc\n' | sed -E 's/b?/x/g'
printf 'abcabc\n' | sed 's/\(abc\)\1/twice/'
printf 'xyzxyz\n' | sed -E 's/(x(y)z)\1/[\2]/'
printf 'word word\n' | sed -E 's/\b(\w+) \1\b/\1/'
printf 'CamelCaseName\n' | sed 's/\([A-Z]\)/_\l\1/g;s/^_//'
printf 'snake_case_name\n' | sed -E 's/_(.)/\u\1/g'
printf 'mixed Case\n' | sed 's/\w\+/\L\u&/g'
printf 'x\n' | sed 's/x/\U\x61b\E c/'
printf 'aaa\n' | sed 's/a/b/2g'
printf 'hello\n' | sed 's/./&&/g'
printf 'hello\n' | sed 's/l*/<&>/g'
printf 'a b\n' | sed 's/ */_/g'
printf ' \n' | sed 's/ *$//' | od -c
printf 'x\n' | sed -n '/x/{s//y/;p}'
printf 'x1\nx2\n' | sed -n '/x/{//p}'
printf 'abc\n' | sed '/b/s//B/'
printf 'abc\n' | sed 's/b/B/;s//X/'
printf 'one\ntwo\n' | sed '/one/{n;s/two/2/}'
printf 'a\nb\nc\nd\n' | sed '/b/,/c/{/b/n;s/^/>/}'
printf 'a\nb\nc\nd\n' | sed -n '/a/,/b/{/b/!p}'
printf 'a\nb\na\nb\n' | sed -n '/a/,/b/p'
printf 'a\nb\na\nb\n' | sed '/a/,/b/d'
printf 'start\nx\nend\nstart\ny\n' | sed -n '/start/,/end/p'
printf 'a\nb\nc\n' | sed -n '2,/b/p'
printf 'a\nb\nc\n' | sed -n '2,/c/p'
printf 'a\nb\nc\n' | sed -n '/a/,1p'
printf 'a\nb\nc\nd\ne\n' | sed -n '2,+0p'
printf 'a\nb\nc\nd\ne\n' | sed -n '/b/,~2p'
printf 'a\nb\nc\nd\ne\n' | sed -n '/c/,~2p'
printf 'a\nb\nc\nd\ne\n' | sed '2,3!s/$/!/'
printf 'a\nb\nc\nd\ne\n' | sed -n '2,4{=;p}'
printf 'a\nb\n' | sed '1!G;$!{h;d}'
printf 'a\nb\nc\n' | sed '/b/{x;p;x}'
printf 'a\nb\nc\n' | sed '/b/{x;p;x;}'
printf 'a\nb\nc\n' | sed -n '/b/{g;p}'
printf 'a\nb\nc\n' | sed '1{h;d};G'
printf 'a\nb\nc\n' | sed 'x'
printf 'a\nb\nc\n' | sed '$!x'
printf 'a\nb\nc\n' | sed -n 'H;$x;$s/\n//;$p'
printf 'k: v\n' | sed 's/^\([^:]*\): *\(.*\)$/\2 <- \1/'
printf 'https://example.org/path?q=1\n' | sed -E 's#^([a-z]+)://([^/]+)(/.*)?$#\2#'
printf '2026-11-26\n' | sed -E 's/([0-9]{4})-([0-9]{2})-([0-9]{2})/\3.\2.\1/'
printf 'a,b,,d\n' | sed 's/,,/,-,/'
printf 'a,b,c\n' | sed 's/[^,]*$/LAST/'
printf 'a  b\t\tc\n' | sed 's/[[:blank:]]\{1,\}/ /g'
printf 'ABC\n' | sed 's/[[:lower:]]/x/g'
printf 'aBc\n' | sed 's/[[:lower:]]/x/gI'
printf 'a]b\n' | sed 's/[]]/X/'
printf 'a^b\n' | sed 's/\^/X/'
printf 'a^b\n' | sed 's/a^b/X/'
printf 'a$b\n' | sed 's/a$b/X/'
printf 'a$b\n' | sed 's/\$/X/'
printf 'a*b\n' | sed 's/*/X/'
printf 'a*b\n' | sed 's/a\*/X/'
printf 'a{1}\n' | sed 's/a{1}/X/'
printf 'aa\n' | sed -E 's/a{1}/X/'
printf 'a+b\n' | sed 's/a+b/X/'
printf 'aab\n' | sed 's/a\+b/X/'
printf 'a?b\n' | sed 's/a?b/X/'
printf 'a|b\n' | sed 's/a|b/X/'
printf 'a|b\n' | sed -E 's/a\|b/X/'
printf '(a)\n' | sed 's/(a)/X/'
printf '(a)\n' | sed -E 's/\(a\)/X/'
printf 'a.b\n' | sed 's/a\.b/X/'
printf 'a\\b\n' | sed 's/a\\b/X/'
printf 'a/b\n' | sed 's/a\/b/X/'
printf 'a/b\n' | sed 's|a/b|X|'
printf 'a|b\n' | sed 's|a\|b|X|'
printf 'a,b\n' | sed 's,a\,b,X,'
printf 'axb\n' | sed 'sxa\xbxXx'
printf 'a\n' | sed 's/a/[&]/;s/\[/{/'
printf 'A\n' | sed 's/a/x/I'
printf 'line\n' | sed -n '/LINE/Ip'
printf 'a\nb\n' | sed 'N;s/^b/X/M'
printf 'a\nb\n' | sed 'N;s/a$/X/M'
printf 'a\nb\n' | sed 'N;s/a$/X/'
printf 'a\nb\n' | sed 'N;s/\`a/X/M'
printf 'a\nb\n' | sed "N;s/b\\'/X/"
printf 'a\nb\n' | sed 'N;s/.*/[&]/'
printf 'a\nb\n' | sed 'N;s/a.b/X/'
printf 'a\nb\n' | sed 'N;s/[^x]*/X/'
printf 'one\n' | sed -e 'a\' -e 'two' -e 'i\' -e 'zero'
printf 'one\n' | sed '1i\' 
printf 'one\ntwo\n' | sed '1~2a --'
printf 'one\ntwo\n' | sed 'a\
> x\
> y'
printf 'one\n' | sed 'c\'
printf 'one\ntwo\n' | sed '$!{$!a\
> mid
> }'
printf 'a\n' | sed 'a\\'
printf 'a\n' | sed 'a\\x'
printf 'a\n' | sed 'a  \  two spaces'
printf 'a\n' | sed 'i\
> \  keep'
printf 'a\n' | sed '1{a b
> }'
printf 'a\nb\n' | sed '1{N;c\
> both
> }'
printf 'a\nb\nc\n' | sed '2c\
> X' -s
printf 'a\nb\nc\n' | sed '/b/{i\
> before
> a\
> after
> }'
printf 'a\nb\n' | sed -n '/a/{p;q}'
printf 'a\nb\n' | sed '/a/q'
printf 'a\nb\n' | sed '/b/Q'
printf 'a\nb\n' | sed '1{a appended
> q}'
printf 'a\nb\n' | sed -n '1{a appended
> q}'
printf 'a\nb\n' | sed '1{i inserted
> Q}'
printf 'a\n' | sed 'q5'; echo "status=$?"
printf 'a\n' | sed -n '$='
: | sed -n '$='
printf 'a\nb\nc\n' | sed -n '2{=;l}'
printf '\033[1m\n' | sed -n l
printf '%%s\n' x | sed -n l
printf 'aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeeeffffffffffgggggggggghhhhhhhhhh\n' | sed -n l
printf 'aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeeeffffffffffggggggggg\001\n' | sed -n l
printf 'abcdefgh\n' | sed -n 'l 4'
printf 'abcdefgh\n' | sed -n 'l 1'
printf 'abcdefgh\n' | sed -n 'l 2'
printf 'a\tb\n' | sed -n 'l 3'
printf 'x\n' | sed 'p;p;p'
printf 'x\n' | sed -n '/x/{p;/x/p}'
printf 'x\ny\n' | sed '$!{N};P;D'
printf 'a b c\n' | sed 's/ /\n/g' | sed -n '$p'
printf 'a\nb\nc\nd\n' | sed 'N;N;N;s/\n/ /g;P;D'
printf 'a\nb\nc\nd\ne\n' | sed '1!{N};$!{$!D}'
printf 'aaa\n' | sed ':a;s/a/b/;ta'
printf 'aaa\n' | sed -n ':a;s/a/b/p;ta'
printf 'aaa\n' | sed 's/a/b/;:a;s/x/y/;ta;s/$/!/'
printf 'ab\n' | sed 's/a/A/;tl;s/b/B/;:l;s/$/./'
printf 'ab\n' | sed 's/q/A/;tl;s/b/B/;:l;s/$/./'
printf 'ab\n' | sed 's/a/A/;Tl;s/b/B/;:l;s/$/./'
printf '1\n22\n333\n' | sed -E ':a;s/^.{1,4}$/ &/;ta'
printf '1234567\n' | sed -E ':a;s/([0-9])([0-9]{3})($|,)/\1,\2\3/;ta'
printf 'a b\n' | sed 's/\(a\) \(b\)/\2\n\1/'
printf 'x\n' | sed 's/x/a\tb/'
printf 'x\n' | sed 's/x/a\\tb/'
printf 'x\n' | sed 's/x/&\&&/'
printf 'abc\n' | sed 's/\(a\)\(b\)\(c\)/\3\2\1\0/'
printf 'abcdefghij\n' | sed -E 's/(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)/\9\8\7\6\5\4\3\2\1/'
printf 'abc\n' | sed -E 's/(a)(x)?(c)?/[\1|\2|\3]/'
printf 'aaa\n' | sed -E 's/(a)*/<\1>/'
printf 'ab\n' | sed -E 's/(a|ab)(b?)/[\1|\2]/'
printf 'xyz\n' | sed -E 's/(x|xy)(z|yz)/[\1|\2]/'
printf 'aaaa\n' | sed -E 's/(a*)(a)/[\1|\2]/'
printf 'aaaa\n' | sed -E 's/(a*?)/x/' 
printf 'aaaa\n' | sed 's/\(a*\)*/[\1]/'
printf 'b\n' | sed 's/\(a*\)*/[\1]/'
printf 'ab\n' | sed -E 's/(a*)+/[\1]/'
printf 'aaa\n' | sed 's/a**/X/'
printf 'a{1}\n' | sed 's/\{1\}a/X/'
printf 'aa\n' | sed 's/a\{1\}\{2\}/X/'
printf 'ab\n' | sed 's/a\{1\}*b/X/'
printf '*a\n' | sed 's/*a/X/'
printf '*a\n' | sed 's/^*a/X/'
printf 'a*\n' | sed 's/\(*\)/X/'
printf 'xay\n' | sed -E 's/a{0}y/X/'
printf 'aaaaa\n' | sed -E 's/a{2,3}/X/g'
printf 'aaaaa\n' | sed 's/a\{2,3\}/X/g'
printf 'aaaaa\n' | sed -E 's/a{,2}/X/g'
printf 'aaa\n' | sed -E 's/a{1,}/X/'
printf 'x\n' | sed -E 's/a{1,2}{3}/X/'
printf 'abc\n' | sed -E 's/a|b|c/X/2'
printf 'abc\n' | sed 's/[[:alpha:]]/X/3'
printf 'a.b\n' | sed 's/[.]/X/'
printf 'a-b\n' | sed 's/[a-]/X/g'
printf 'a-b\n' | sed 's/[-b]/X/g'
printf 'a\\b\n' | sed 's/[\\]/X/'
printf 'a\\nb\n' | sed 's/[\n]/X/'
printf 'a\nb\n' | sed 'N;s/[\n]/X/'
printf 'a\tb\n' | sed 's/[\t]/X/'
printf 'a:b\n' | sed 's/[[:punct:]]/X/'
printf 'a=b\n' | sed 's/[[=a=]]/X/'
printf 'a.b\n' | sed 's/[[.-.]a]/X/'
printf 'ab\n' | sed 's/[^[:alpha:]]/X/'
printf 'a1\n' | sed 's/[^[:alpha:]]/X/'
printf 'x\n' | sed 's/[[:alpha:]-]/X/'
