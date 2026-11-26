# tr: translate, delete, squeeze
%setup
printf 'Hello, World!\nfoo  bar   baz\n\ttabs\there\n' > f
printf 'aaabbbcccdddeee\n' > rep
printf 'no newline at end' > nonl
printf '\000\001\002abc\377\376\n' > bin
%cases
tr a-z A-Z < f
tr A-Z a-z < f
tr '[:lower:]' '[:upper:]' < f
tr '[:upper:]' '[:lower:]' < f
tr -d '[:space:]' < f
tr -d '[:punct:]' < f
tr -d aeiou < f
tr -s ' ' < f
tr -s '[:space:]' < f
tr -s a-z < rep
tr -s abc < rep
tr -s ab xy < rep
tr abc xyz < rep
tr abcde xy < rep
tr -t abcde xy < rep
tr -c a-z X < f
tr -cd a-z < f
tr -cs a-zA-Z '\n' < f
tr -C '[:alnum:]' _ < f
tr -ds a b < rep
tr -d '\n' < f
tr '\n' ' ' < f
tr '\t' '>' < f
tr '\101-\132' x < f
tr -d '\000-\037' < bin | od -c
tr '\000' Z < bin | od -c
tr '\377' Y < bin | od -c
tr -d '\200-\377' < bin | od -c
tr a-c '[x*]' < rep
tr a-e '[x*2]yz' < rep
tr a-e 'v[x*]z' < rep
tr a-e '[x*010]' < rep
tr '[=a=]' A < rep
tr -d '[=b=]' < rep
tr '[:digit:]' D < f
echo 123abc | tr '[:digit:]' '[:alpha:]'
echo abc | tr '[c*]' x
echo abc | tr z-a x
echo abc | tr
echo abc | tr a
echo abc | tr -d
echo abc | tr -d a b
echo abc | tr a b c
echo abc | tr a ''
echo abc | tr -t a ''
echo abc | tr '[:foo:]' x
echo 'a-b-c' | tr - _
echo 'a-b-c' | tr 'a-' 'x_'
echo 'a\b' | tr '\\' /
echo 'abc' | tr 'a\-c' 'xyz'
echo 'a]b[c' | tr '[]' '()'
echo 'abc' | tr -s ''
tr x y < nonl
tr -s o < nonl
: | tr a b
tr '[:xdigit:]' '*' < f
tr '[:blank:]' . < f
tr '[:cntrl:]' . < bin | od -c
tr -c '[:print:]' . < bin
tr '[:graph:]' g < f
echo hello | tr el ip
echo hello | tr -- -e x
echo AbC | tr -c '[:upper:]' '*'
seq 1 2000 | tr -d '\n' | wc -c
seq 1 2000 | tr 0-9 a-j | tail -3
echo abc | tr --delete b
echo aabbcc | tr --squeeze-repeats abc
echo abc | tr --complement a x
echo abcd | tr -t abcd xy
