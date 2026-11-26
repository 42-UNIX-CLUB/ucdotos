# nl
%setup
printf 'one\ntwo\n\nfour\n\n\nseven\n' > f
printf '\\:\\:\\:\nhead\n\\:\\:\nbody1\nbody2\n\\:\nfoot\n\\:\\:\\:\nhead2\n\\:\\:\nbody3\n' > pages
printf 'last' > nonl
%cases
nl f
nl -b a f
nl -b n f
nl -b t f
nl -b 'pe$' f
nl -b p^t f
nl -v 10 f
nl -i 5 f
nl -v 0 -i -1 f
nl -w 3 f
nl -w 1 f
nl -n ln f
nl -n rz f
nl -n rn f
nl -s ': ' f
nl -s '' f
nl -b a -l 2 f
nl -b a -l 3 f
nl pages
nl -p pages
nl -h a -f a pages
nl -d '#' f
nl nonl
nl - < f
nl f f
nl missing; echo "st=$?"
nl -b x f; echo "st=$?"
nl -n xx f; echo "st=$?"
nl -w 0 f; echo "st=$?"
nl -v x f; echo "st=$?"
nl -Z f; echo "st=$?"
: | nl
seq 1 12 | nl -w 2 | tail -3
nl --body-numbering=a --number-width=2 --number-separator=. f
