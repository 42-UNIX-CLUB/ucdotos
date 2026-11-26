##### j-basic
-j runs independent recipes; the results are the same.
--- Makefile
all: a b c d ; @cat a b c d
a b c d: ; @echo $@ > $@
--- run
make -j4
make -j4

##### j-order
Dependencies are respected with -j.
--- Makefile
all: final
final: mid1 mid2 ; @cat mid1 mid2 > final; echo final
mid1: base ; @cat base > mid1
mid2: base ; @cat base > mid2
base: ; @echo base > base
--- run
make -j8 | sort
cat final

##### j-error
An error with -j waits for the others.
--- Makefile
all: bad slow
bad: ; @exit 1
slow: ; @sleep 0.2; echo slow done
--- run
make -j2 2>&1 | sort; echo "status $?"

##### j-keep-going
-k with -j.
--- Makefile
all: bad good1 good2
bad: ; @exit 1
good1 good2: ; @echo $@
--- run
make -k -j3 2>&1 | sort

##### j-notparallel
.NOTPARALLEL runs one at a time.
--- Makefile
.NOTPARALLEL:
all: a b
a: ; @sleep 0.1; echo a
b: ; @echo b
--- run
make -j4

##### j-unlimited
-j with no number.
--- Makefile
all: x1 x2 x3 x4 x5 x6 ; @echo $^
x%: ; @touch $@
--- run
make -j

##### j-many
Many targets with -j3.
--- Makefile
N := 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
all: $(addprefix t,$(N)) ; @cat $^ | wc -l
t%: ; @echo $* > $@
--- run
make -j3

##### j-recursive
-j passes to sub-makes.
--- Makefile
all: ; @$(MAKE) -s -C sub
--- file sub/Makefile
all: a b ; @cat a b
a b: ; @echo $@ > $@
--- run
make -j2

##### j-grouped
Grouped targets with -j.
--- Makefile
all: x y z
x y &: ; @echo once; touch x y
z: ; @touch z
--- run
make -j3 | sort

##### j-wait
.WAIT orders groups.
--- Makefile
all: a b .WAIT c
a: ; @sleep 0.1; echo a
b: ; @sleep 0.1; echo b
c: ; @echo c
--- run
make -j4 | tail -1
