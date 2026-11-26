##### deep-chain
A long chain of prerequisites.
--- Makefile
N := 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
$(foreach i,$(N),$(eval s$(i): s$(shell expr $(i) - 1) ; @echo $$@))
s0: ; @echo s0
--- run
make s20

##### many-goals-messages
Up-to-date messages for each goal.
--- Makefile
a b c: ; @touch $@
d: ;
--- run
make a b
make a b c d

##### phony-with-file
A phony target with a recipe and prerequisites.
--- Makefile
.PHONY: install
install: prog ; @echo installing $<
prog: ; @touch prog
--- run
make install
make install

##### pattern-vs-explicit
An explicit rule wins over a pattern rule.
--- Makefile
%.o: ; @echo pattern $@
special.o: ; @echo explicit $@
all: special.o other.o
--- run
make

##### stem-with-dot
Stems that contain dots and directories.
--- Makefile
all: a.b.c.out dir/x.y.out
%.out: ; @echo '$* $(*D) $(*F)'
--- run
make

##### percent-in-prereq-not-pattern
A literal % in an explicit rule.
--- Makefile
all: a%b ; @echo '$^'
a%b: ; @echo making '$@'
--- run
make

##### wildcard-sorted
$(wildcard) results are sorted.
--- Makefile
all: ; @echo $(wildcard *.x)
--- file c.x
--- file a.x
--- file b.x
--- run
make

##### wildcard-dirs
Wildcards with directories.
--- Makefile
all: ; @echo $(wildcard */*.c) | tr ' ' '\n'
--- file d1/a.c
--- file d2/b.c
--- file d2/c.h
--- run
make

##### automatic-in-pattern-multiple
$@ in a pattern rule with two targets.
--- Makefile
all: x.a x.b
%.a %.b: ; @echo '[$@] [$*]'; touch $*.a $*.b
--- run
make

##### var-expansion-in-target
Variables in target names.
--- Makefile
NAME = prog
$(NAME): ; @echo built $@
--- run
make

##### colon-in-var-value
A colon in a variable's value used in a recipe.
--- Makefile
URL = http://example.org:80/x
all: ; @echo $(URL)
--- run
make

##### semicolon-in-var
A semicolon in a variable used in a rule line.
--- Makefile
CMD = echo one; echo two
all: ; @$(CMD)
--- run
make

##### equals-in-recipe
An = in a recipe line is not an assignment.
--- Makefile
all:
	@X=1; echo $$X
	@echo a=b
--- run
make

##### target-var-expansion-time
Target-specific variables expand in the target context.
--- Makefile
X = $(Y)
t: Y = inner
t: ; @echo $(X)
Y = outer
u: ; @echo $(X)
--- run
make t u

##### makeflags-in-recipe-env
MAKEFLAGS in a sub-make started without $(MAKE).
--- Makefile
all: ; @echo "$$MAKEFLAGS"
--- run
make -k -s
make -n

##### dollar-in-target-specific
$$ in target-specific values.
--- Makefile
t: V = a$$b
t: ; @echo '$(V)'
--- run
make

##### export-target-value
An exported variable with a target-specific value.
--- Makefile
export E = global
t: E = target
t: ; @echo "$$E"
u: ; @echo "$$E"
--- run
make t u

##### else-with-text
else followed by junk.
--- Makefile
ifeq (a,b)
else junk
endif
all: ;
--- run
make; echo "status $?"

##### recursive-plain-make-warning
A sub-make started as plain make in a recipe.
--- Makefile
all: ; @make -s -f sub.mk
--- file sub.mk
x: ; @echo sub $(MAKELEVEL)
--- run
make

##### file-and-dir-same-name
A target that names a directory that exists.
--- Makefile
all: src ; @echo all
--- run
mkdir src
make

##### include-makefile-list-last
lastword of MAKEFILE_LIST inside an included file.
--- Makefile
include sub/inc.mk
all: ; @echo $(HERE)
--- file sub/inc.mk
HERE := $(dir $(lastword $(MAKEFILE_LIST)))
--- run
make

##### append-to-env-var
+= to a variable from the environment.
--- Makefile
PATHX += extra
all: ; @echo "$(PATHX) $(origin PATHX)"; echo "$$PATHX"
--- run
PATHX=env make

##### conditional-assign-env
?= does not override the environment.
--- Makefile
EV ?= default
all: ; @echo $(EV)
--- run
make
EV=env make

##### simple-var-with-dollar-paren
:= of a value with $$.
--- Makefile
A := $$(not expanded)
B = $(A)
all: ; @echo '$(B)'
--- run
make

##### join-lines-in-function
Backslash-newline inside a function call.
--- Makefile
X = $(subst a,b,\
      aaa)
all: ; @echo '[$(X)]'
--- run
make

##### dollar-paren-brace-mix
$( and ${ mixed.
--- Makefile
A = x
B = y
all: ; @echo ${A}$(B) $(A${B}) ${$(subst z,,A)}
xy = z
yx = w
--- run
make

##### order-of-goals-and-default
Goals on the command line in order.
--- Makefile
one: ; @echo one
two: ; @echo two
three: ; @echo three
--- run
make three one two

##### same-goal-twice
The same goal twice.
--- Makefile
a: ; @echo a
--- run
make a a

##### shell-in-prereq
$(shell) in a prerequisite list.
--- Makefile
all: $(shell echo x y) ; @echo $^
x y: ; @echo making $@
--- run
make

##### sort-dedup-in-prereqs
$^ removes duplicates; $+ keeps them.
--- Makefile
all: a b a c b ; @echo '$^ | $+'
a b c: ;
--- run
make

##### newer-only-question
$? when the target exists.
--- Makefile
lib: a.o b.o ; @echo 'update with $?'; touch lib
--- run
touch -d '2001-01-01' a.o
touch -d '2001-01-02' lib
touch b.o
make
