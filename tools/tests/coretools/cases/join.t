# join
%setup
printf '1 apple red\n2 banana yellow\n3 cherry red\n5 fig purple\n' > a
printf '1 one\n2 two\n4 four\n5 five\n5 cinq\n' > b
printf 'a:1:x\nb:2:y\nc:3:z\n' > c1
printf 'a:10\nc:30\nd:40\n' > c2
printf 'k v1\nk v2\nm v3\n' > dup1
printf 'k w1\nk w2\nn w3\n' > dup2
printf '  1   spaced   out\n2 x\n' > sp
printf 'A 1\nb 2\n' > upper
printf 'a X\nB Y\n' > lower
printf 'id name\n1 x\n2 y\n' > h1
printf 'id val\n1 p\n3 q\n' > h2
: > empty
%cases
join a b
join b a
join -a 1 a b
join -a 2 a b
join -a 1 -a 2 a b
join -v 1 a b
join -v 2 a b
join -v 1 -v 2 a b
join -o 1.2,2.2 a b
join -o 0,2.2,1.3 a b
join -o 1.1,1.2,2.2 -a 1 -e NONE a b
join -a 2 -o 0,1.2,2.2 -e - a b
join -t: c1 c2
join -t: -a1 -a2 c1 c2
join -t: -o 1.3,2.2 c1 c2
join -t: -1 1 -2 1 c1 c2
join -1 2 -2 1 a b
join dup1 dup2
join -a1 -a2 dup1 dup2
join sp b
join -i upper lower
join upper lower
join --header h1 h2
join a empty
join empty b
join -a1 a empty
join -j 1 a b
join -t '' a b
join - b < a
join -o auto -a1 a b
join a; echo "st=$?"
join a b c; echo "st=$?"
join -1 0 a b; echo "st=$?"
join -a 3 a b; echo "st=$?"
join -o 3.1 a b; echo "st=$?"
join -t ab a b; echo "st=$?"
join a missing; echo "st=$?"
@fresh printf 'b 1\na 2\n' > uns; join uns b; echo "st=$?"
@fresh printf 'b 1\na 2\n' > uns; join --nocheck-order uns b; echo "st=$?"
