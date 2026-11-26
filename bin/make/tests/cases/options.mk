##### dry-run
-n prints and does not run.
--- Makefile
all: out
out: in
	@echo making out
	cp in out
--- file in
--- run
make -n
make --just-print
make --dry-run
make --recon
ls

##### dry-run-chain
-n follows the chain: dependents of a would-be-remade file are printed too.
--- Makefile
c: b ; cp b c
b: a ; cp a b
--- run
touch -d '2001-01-01' b c
touch a
make -n

##### dry-run-make-runs
-n still runs lines with $(MAKE) and +.
--- Makefile
all:
	@echo plain
	+@echo plus-line
	$(MAKE) -s -f sub.mk
--- file sub.mk
x: ; @echo sub ran
--- run
make -n --no-print-directory

##### touch
-t touches instead of running.
--- Makefile
out: in ; @echo should not run
--- run
touch -d '2001-01-01' out
touch in
make -t
make -t
make
make -t -s

##### touch-missing
-t creates missing targets.
--- Makefile
all: a b
a b: ; echo never
--- run
make -t
ls

##### question
-q exit status.
--- Makefile
out: in ; @cp in out
--- file in
--- run
make -q; echo "status $?"
make
make -q; echo "status $?"
make --question; echo "status $?"

##### question-phony
-q on a phony target.
--- Makefile
.PHONY: all
all: ; @echo x
--- run
make -q; echo "status $?"

##### always-make
-B remakes everything.
--- Makefile
out: in ; @echo remade
--- file in
--- run
touch out
make
make -B
make --always-make

##### old-file
-o treats a file as very old.
--- Makefile
out: mid ; @echo out
mid: in ; @echo mid
--- run
touch -d '2001-01-01' out
touch -d '2001-01-02' mid
touch in
make
make -o mid
make --old-file=mid
make --assume-old=mid

##### what-if
-W treats a file as new.
--- Makefile
out: in ; @echo out
--- run
touch in
sleep 0.01
touch out
make
make -W in
make -n -W in
make --what-if=in -n

##### silent
-s and --quiet.
--- Makefile
all: ; echo loud
--- run
make -s
make --silent
make --quiet
make -s --no-silent

##### keep-going-stop
-S cancels -k.
--- Makefile
all: a b ; @echo all
a: ; @false
b: ; @echo b
--- run
make -k -S; echo "status $?"

##### environment-overrides
-e
--- Makefile
X = file
all: ; @echo $(X)
--- run
X=env make
X=env make -e
X=env make --environment-overrides

##### no-builtin-rules
-r
--- Makefile
all: x.o
--- file x.c
--- run
make -n
make -r; echo "status $?"
make --no-builtin-rules -n; echo "status $?"

##### no-builtin-variables
-R
--- Makefile
all: ; @echo "[$(CC)] [$(MAKE)] [$(RM)]"
--- run
make
make -R
make --no-builtin-variables

##### eval-option
--eval
--- Makefile
all: ; @echo $(X)
--- run
make --eval='X = from-eval'
make -E 'X = short'
make --eval='extra: ; @echo extra' extra

##### jobs-arg-forms
-j forms.
--- Makefile
all: ; @echo "[$(MAKEFLAGS)]"
--- run
make -j1
make -j2
make -j 3
make --jobs=4
make -j
make -kj2

##### print-directory
-w and --no-print-directory.
--- Makefile
all: ; @echo hi
--- run
make -w
make -w --no-print-directory
make --print-directory -s

##### version-help
--version and --help exist.
--- Makefile
all: ;
--- run
make --version > /dev/null; echo "status $?"
make --help > /dev/null; echo "status $?"
make -v > /dev/null; echo "status $?"

##### variable-and-goal-mix
Variables and goals in any order.
--- Makefile
a b: ; @echo $@ $(V) $(W)
--- run
make V=1 a W=2 b
make -- V=3 a

##### long-option-prefix
Unique prefixes of long options work.
--- Makefile
all: ; echo hi
--- run
make --sil
make --dry

##### ignore-errors-long
--ignore-errors and --keep-going.
--- Makefile
all: a b ; @echo all
a: ; @false
b: ; @echo b
--- run
make --ignore-errors; echo "status $?"
make --keep-going; echo "status $?"

##### trace
--trace says why each target is remade.
--- Makefile
all: a b
	@echo all
a: c
	@echo a; touch a
b:
	@echo b
c:
	touch c
.PHONY: b
%.x: %.y
	@cp $< $@
--- run
make --trace
touch -d '2001-01-01' a
make --trace a
echo z > q.y
make --trace q.x

##### debug-accepted
-d and --debug are accepted.
--- Makefile
all: ; @true
--- run
make -d > /dev/null; echo "status $?"
make --debug=b > /dev/null; echo "status $?"

##### print-database-runs
-p prints and still runs.
--- Makefile
all: ; @echo ran
--- run
make -p | grep -c '^all:'
make -p -q > /dev/null; echo "status $?"
