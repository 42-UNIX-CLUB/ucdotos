# od
%setup
printf 'Hello, world!\n' > h
printf '\000\001\002\003\177\200\376\377' > bin
printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab' > rep
printf 'abc' > odd
: > empty
%cases
od h
od -c h
od -b h
od -d h
od -o h
od -s h
od -x h
od -i h
od -a bin
od -c bin
od -An -tx1 h
od -An -tx1 bin
od -t x1 h
od -t x2 h
od -t x4 h
od -t x8 h
od -t d1 bin
od -t d2 bin
od -t d4 bin
od -t d8 bin
od -t u1 bin
od -t u2 bin
od -t u4 bin
od -t o1 bin
od -t o4 bin
od -t xC h
od -t xS h
od -t dI bin
od -t x1 -t c h
od -t x2 -t d1 h
od -t c -t x1 -t o1 bin
od -tx1z h
od -A d h
od -A x h
od -A n h
od -A x -t x1 rep
od -v -A x -t x1 rep
od -c rep
od -w8 -c h
od -w4 -t x1 h
od -w h
od -j 7 -c h
od -N 5 -c h
od -j 2 -N 3 -t x1 h
od -j 100 h; echo "st=$?"
od odd
od -t d2 odd
od -t x4 odd
od empty
od -c empty
od -c h odd
od -c - < odd
od -An -c < h
od h missing; echo "st=$?"
od -t q h; echo "st=$?"
od -t x3 h; echo "st=$?"
od -A z h; echo "st=$?"
od -w3 -t x2 h; echo "st=$?"
od -Q h; echo "st=$?"
od -t f4 h; echo "st=$?"
printf '\000\000\200\077\000\000\000\000\000\000\360\077' | od -t f4 -t f8
od -t fD -t x1 h
seq 1 300 | od -c | tail -3
od -j 0x4 -N 010 -c h
od --format=x1 --address-radix=d --read-bytes=4 h
