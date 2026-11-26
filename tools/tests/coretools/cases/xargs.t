# xargs
%setup
printf 'a b c\nd e\nf\n' > words
printf 'one\ntwo\nthree\n' > lines
printf 'x\000y z\000w\000' > nul
printf "a 'b c' \"d e\" f\\\\ g\n" > quoted
printf '  lead\ntwo words\n\n' > blanks
mkdir sub; : > sub/f1; : > sub/f2
%cases
xargs < words
xargs echo < words
xargs -n 2 < words
xargs -n 1 < words
xargs -n 4 echo X < words
xargs -L 1 < words
xargs -L 2 < words
xargs -I {} echo '[{}]' < lines
xargs -I @ echo @ and @ < lines
xargs -I {} echo {} < blanks
xargs -i echo '<{}>' < lines
xargs -0 < nul
xargs -0 -n 1 < nul
xargs -d , < /dev/null; printf 'a,b,c' | xargs -d , -n 1
printf 'a\nb c\n' | xargs -d '\n' -n 1 echo
xargs < quoted
xargs -n 1 < quoted
printf "unterminated 'quote\n" | xargs; echo "st=$?"
xargs < /dev/null; echo "st=$?"
xargs -r echo X < /dev/null; echo "st=$?"
xargs --no-run-if-empty echo X < /dev/null; echo "st=$?"
xargs -t echo < lines
xargs -E two < lines
xargs -E one < lines
printf '1\n2\n3\n' | xargs sh -c 'echo $#: "$@"' sh
printf '1\n2\n3\n' | xargs -n 2 sh -c 'echo $#: "$@"' sh
printf 'a\n' | xargs false; echo "st=$?"
printf 'a\n' | xargs sh -c 'exit 3'; echo "st=$?"
printf 'a\n' | xargs sh -c 'exit 255'; echo "st=$?"
printf 'a\n' | xargs nonexistent-command-xyz; echo "st=$?"
printf 'a\n' | xargs -n 0; echo "st=$?"
printf 'a\n' | xargs -L x; echo "st=$?"
printf 'a\n' | xargs -Q; echo "st=$?"
seq 1 1000 | xargs | wc -c
seq 1 1000 | xargs -n 100 | wc -l
seq 1 20 | xargs -s 30
seq 1 5 | xargs -s 4; echo "st=$?"
printf 'a b \nc\nd\n' | xargs -L 1
printf '\n\n  \n' | xargs echo X
cd sub && printf 'f1\nf2\n' | xargs ls
printf 'a\\nb\n' | xargs
printf 'x y\n' | xargs -I{} sh -c 'echo "{}"'
printf 'a\nb\n' | xargs -P 1 -n 1 echo
printf 'a b\n' | xargs --max-args=1 echo
printf 'a b\nc\n' | xargs --max-lines=1 echo
printf 'a\nb\n' | xargs --replace=X echo X X
printf 'a\000b\000' | xargs --null echo
