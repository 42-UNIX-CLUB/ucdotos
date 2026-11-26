# paste
%setup
printf '1\n2\n3\n' > n
printf 'a\nb\n' > l
printf 'x\ny\nz\nw\n' > m
printf 'no\nnewline' > nonl
: > empty
%cases
paste n l
paste l n
paste n l m
paste -d, n l
paste -d ',;' n l m
paste -d '' n l
paste -d '\n' n l
paste -d '\t-' n l m
paste -d '\0' n l
paste -d '\\' n l
paste -s n
paste -s n l
paste -s -d, n l m
paste -s -d ',;' m
paste -sd '\0' m
paste n empty
paste empty n
paste -s empty
paste nonl l
paste -s nonl
paste - - < m
paste - - - < m
paste -s - < n
paste n - < l
paste --serial --delimiters=: n
paste n missing; echo "st=$?"
paste -x n; echo "st=$?"
paste -d; echo "st=$?"
paste -z n | od -c
printf 'a\000b\000' | paste -z -s -d, | od -c
seq 1 10 | paste - - - - -
seq 1 10 | paste -sd+ | sed 's/+/ + /g'
