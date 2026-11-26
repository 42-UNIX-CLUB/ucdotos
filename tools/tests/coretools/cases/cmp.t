# cmp
%setup
printf 'abc\ndef\n' > a
printf 'abc\ndef\n' > a2
printf 'abc\ndxf\n' > b
printf 'abc\nde' > short
: > empty
printf 'xbc\ndef\nghi\n' > c
printf '\000\001\377\n' > bin1
printf '\000\002\376\n' > bin2
%cases
cmp a a2; echo "st=$?"
cmp a b; echo "st=$?"
cmp a short; echo "st=$?"
cmp short a; echo "st=$?"
cmp a empty; echo "st=$?"
cmp empty a; echo "st=$?"
cmp empty empty; echo "st=$?"
cmp -s a b; echo "st=$?"
cmp -s a a2; echo "st=$?"
cmp -s a short; echo "st=$?"
cmp -l a b; echo "st=$?"
cmp -l a c; echo "st=$?"
cmp -l a short; echo "st=$?"
cmp -l bin1 bin2; echo "st=$?"
cmp bin1 bin2; echo "st=$?"
cmp -b a b; echo "st=$?"
cmp -n 5 a b; echo "st=$?"
cmp -n 6 a b; echo "st=$?"
cmp -i 4 a b; echo "st=$?"
cmp -i 1:1 a c; echo "st=$?"
cmp a b 4 4; echo "st=$?"
cmp a c 1 1; echo "st=$?"
cmp - a < a; echo "st=$?"
cmp a - < b; echo "st=$?"
cmp a < b; echo "st=$?"
cmp a missing; echo "st=$?"
cmp missing a; echo "st=$?"
cmp; echo "st=$?"
cmp -l -s a b; echo "st=$?"
cmp a b c d e; echo "st=$?"
cmp -x a b; echo "st=$?"
@fresh seq 1 10000 > big1; seq 1 10000 | sed 's/^9999$/X/' > big2; cmp big1 big2; echo "st=$?"
@fresh seq 1 10000 > big1; cmp big1 big1; echo "st=$?"
