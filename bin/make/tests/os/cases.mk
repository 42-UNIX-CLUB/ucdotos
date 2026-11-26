##### basic
--- Makefile
all: b
	@echo all $@ from $^
b:
	echo making b
	touch b
--- run
make
make
make -n all

##### variables
--- Makefile
A = $(B)
B = late
C := now $(B)
D ?= dflt
E = one
E += two
F != echo shell
all: ; @echo "[$(A)] [$(C)] [$(D)] [$(E)] [$(F)]"
--- run
make
make D=cmd

##### functions
--- Makefile
SRC = a.c b.c sub/c.c
all:
	@echo $(SRC:.c=.o) $(patsubst %.c,%.h,$(SRC))
	@echo $(notdir $(SRC)) $(dir $(SRC)) $(basename $(SRC)) $(suffix $(SRC))
	@echo $(words $(SRC)) $(word 2,$(SRC)) $(firstword $(SRC)) $(lastword $(SRC))
	@echo $(filter %.c,a.c b.h) $(filter-out %.c,a.c b.h) $(sort z a m a)
	@echo $(subst a,A,banana) $(strip   x   y  ) $(findstring an,banana)
	@echo $(addprefix p/,x y) $(addsuffix .z,x y) $(join a b,1 2)
	@echo '$(foreach x,1 2 3,<$(x)>)' $(if $(SRC),yes,no) $(or ,b) $(and a,b)
	@echo $(call rev,1,2) $(origin SRC) $(flavor SRC) '$(value rev)'
rev = $(2) $(1)
--- run
make

##### conditionals
--- Makefile
X = 1
ifeq ($(X),1)
R = one
else ifeq ($(X),2)
R = two
else
R = other
endif
ifdef X
D = defined
endif
ifndef NOPE
N = undefined
endif
all: ; @echo $(R) $(D) $(N)
--- run
make
make X=2
make X=3

##### pattern-rules
--- Makefile
all: a.out b.out
%.out: %.in
	@echo build $@ from $< stem $*
	@cat $< > $@
--- file a.in
A
--- file b.in
B
--- run
make
cat a.out b.out
make

##### rebuild-on-change
--- Makefile
out: in
	@echo rebuild
	@cat in > out
--- file in
v1
--- run
make
make
sleep 1
echo v2 > in
make
cat out

##### static-pattern
--- Makefile
OBJS = x.o y.o
all: $(OBJS)
$(OBJS): %.o: %.c
	@echo $@ from $<
	@touch $@
--- file x.c
--- file y.c
--- run
make

##### double-colon
--- Makefile
log:: a
	@echo from a
log:: b
	@echo from b
a b: ; @touch $@
--- run
make

##### order-only
--- Makefile
out: in | dir
	@echo build out
	@touch out
dir: ; @mkdir dir; echo made dir
--- file in
--- run
make
make

##### phony
--- Makefile
.PHONY: clean
clean: ; @echo cleaning
--- run
touch clean
make clean

##### automatic-vars
--- Makefile
out.txt: a.txt b.txt a.txt | dir.txt
	@echo '@=$@ <=$< ^=$^ +=$+ |=$| *=$* @D=$(@D) @F=$(@F)'
a.txt b.txt dir.txt: ; @touch $@
--- run
make

##### target-specific
--- Makefile
V = global
all: one two
one: V = one
one two: ; @echo $@ $(V)
--- run
make

##### define-eval
--- Makefile
define RULE
$(1): ; @echo rule for $(1)
endef
$(foreach t,p q,$(eval $(call RULE,$(t))))
define TWO
@echo first
@echo second
endef
all: p q ; $(TWO)
--- run
make

##### dry-run-silent
--- Makefile
all: ; echo hi
--- run
make -n
make -s
make

##### keep-going
--- Makefile
all: bad good ; @echo all
bad: ; @false
good: ; @echo good
--- run
make
echo status $?
make -k
echo status $?

##### ignore-errors
--- Makefile
all:
	-false
	@echo after
--- run
make
echo status $?
make -i
echo status $?

##### no-rule
--- Makefile
all: missing
--- run
make
echo status $?

##### question-touch
--- Makefile
out: in ; @cat in > out
--- file in
--- run
make -q
echo status $?
make -t
make -q
echo status $?

##### always-make
--- Makefile
out: ; @echo remake; touch out
--- run
make
make
make -B

##### recursive
--- Makefile
all:
	@echo top
	@$(MAKE) -C sub
--- file sub/Makefile
all: ; @echo sub level $(MAKELEVEL)
--- run
make --no-print-directory
make -s

##### parallel
--- Makefile
all: a b c ; @cat a b c
a b c: ; @echo $@ > $@
--- run
make -j2
make -j2

##### include-remake
--- Makefile
include gen.mk
all: ; @echo V=$(V)
gen.mk: ; @echo generating; echo 'V = gen' > gen.mk
--- run
make
make

##### dash-include
--- Makefile
-include nope.mk
all: ; @echo fine
--- run
make

##### intermediate
--- Makefile
all: x.c
%.c: %.b ; @cat $< > $@
%.b: %.a ; @cat $< > $@
--- file x.a
src
--- run
make
echo *

##### delete-on-error
--- Makefile
.DELETE_ON_ERROR:
out: ; @echo part > out; false
--- run
make
echo *

##### shell-function
--- Makefile
X := $(shell echo one; echo two)
Y := $(shell cat data)
all: ; @echo "[$(X)] [$(Y)] [$(.SHELLSTATUS)]"
--- file data
from file
--- run
make

##### wildcard-vpath
--- Makefile
VPATH = src
all: $(notdir $(wildcard src/*.c)) ; @echo $^
--- file src/p.c
--- file src/q.c
--- run
make

##### errors
--- Makefile
all: ; @echo $(word x,a)
--- run
make
echo status $?

##### makeflags-env
--- Makefile
all: ; echo hi
--- run
MAKEFLAGS=s make

##### error-format
--- Makefile
all:
	@echo before
	@exit 3
--- run
make
echo status $?

##### command-not-found
--- Makefile
all: ; no-such-cmd-here
--- run
make
echo status $?

##### shell-loops
--- Makefile
all:
	@for i in a b; do echo $$i; done
	@if [ -f Makefile ]; then echo has makefile; fi
	@x=1 && echo x=$$x || echo no
--- run
make
