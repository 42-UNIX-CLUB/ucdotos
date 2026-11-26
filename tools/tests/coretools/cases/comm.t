# comm
%setup
printf 'apple\nbanana\ncherry\ndate\n' > a
printf 'banana\ncherry\nfig\n' > b
printf 'b\na\n' > unsorted
: > empty
printf 'x\ny' > nonl
%cases
comm a b
comm -1 a b
comm -2 a b
comm -3 a b
comm -12 a b
comm -13 a b
comm -23 a b
comm -123 a b
comm a empty
comm empty b
comm --output-delimiter=: a b
comm --total a b
comm --total -12 a b
comm a a
comm - b < a
comm a nonl
comm unsorted a; echo "st=$?"
comm --nocheck-order unsorted a; echo "st=$?"
comm a; echo "st=$?"
comm a b a; echo "st=$?"
comm a missing; echo "st=$?"
comm -4 a b; echo "st=$?"
comm -z a b | od -c | head -3
@fresh seq 1 100 | sort > s1; seq 50 150 | sort > s2; comm -12 s1 s2 | wc -l
