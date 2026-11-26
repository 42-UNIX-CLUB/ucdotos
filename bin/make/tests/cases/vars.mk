##### recursive-vs-simple
= expands late, := expands now.
--- Makefile
A = $(B)
C := $(B)
B = later
all: ; @echo "[$(A)] [$(C)]"
--- run
make

##### double-colon-equals
::= is simple too.
--- Makefile
B = one
A ::= $(B)
B = two
all: ; @echo $(A)
--- run
make

##### triple-colon-equals
:::= expands now and escapes dollars.
--- Makefile
B = x$$y
A :::= $(B)
all: ; @echo '$(value A)' '$(A)'
--- run
make

##### conditional-assign
?= only assigns undefined variables.
--- Makefile
A = set
A ?= other
B ?= fresh
C =
C ?= notused
all: ; @echo "[$(A)] [$(B)] [$(C)]"
--- run
make
make B=cmd

##### append
+= on recursive, simple and undefined variables.
--- Makefile
R = a
R += $(X)
S := a
S += $(X)
U += new
E =
E += x
X = later
all: ; @echo "[$(R)] [$(S)] [$(U)] [$(E)]"
--- run
make

##### shell-assign
!= runs the shell.
--- Makefile
A != echo hello; echo world
B != printf 'a\nb\n\n'
all: ; @echo "[$(A)] [$(B)] [$(.SHELLSTATUS)]"
--- run
make

##### value-trailing-space
Trailing spaces before a comment are kept.
--- Makefile
A = word   # comment
B =    lead
all: ; @echo "[$(A)][$(B)]"
--- run
make

##### computed-names
Variable names built from variables.
--- Makefile
x = y
y = z
z = final
a_1 = one
n = 1
all: ; @echo $($(x)) $($($(x))) $(a_$(n))
--- run
make

##### single-char-vars
$X and $(X) are the same.
--- Makefile
X = ex
all: ; @echo $X $(X) ${X}
--- run
make

##### subst-refs
Substitution references.
--- Makefile
SRC = a.c b.c dir/c.c
all:
	@echo $(SRC:.c=.o)
	@echo $(SRC:%.c=obj/%.o)
	@echo ${SRC:c=x}
	@echo $(SRC:.c=)
--- run
make

##### override
override beats the command line.
--- Makefile
override A = file
B = file
override C += more
all: ; @echo "[$(A)] [$(B)] [$(C)]"
--- run
make
make A=cmd B=cmd C=cmd

##### cmdline-beats-file
Command-line variables win over the makefile.
--- Makefile
V = file
V += appended
all: ; @echo $(V) $(origin V)
--- run
make
make V=cmd
make V+=cmdappend

##### environment
Environment variables and -e.
--- Makefile
ENVV = file
all: ; @echo $(ENVV) $(origin ENVV) $(origin HOME); echo $$ENVV
--- run
make
ENVV=env make
ENVV=env make -e

##### env-default-export
Variables from the environment are exported; file ones are not.
--- Makefile
FILEV = f
all: ; @echo "[$$FILEV] [$$EV]"
--- run
EV=e make
EV=e make EV=c

##### export
export and unexport.
--- Makefile
export A = a
B = b
export B
C = c
export D E
D = d
unexport HOME2
all: ; @echo "[$$A] [$$B] [$$C] [$$D] [$$E] [$$HOME2]"
--- run
HOME2=h make

##### export-all
export with no names.
--- Makefile
export
A = a
all: ; @echo "[$$A]"
--- run
make

##### export-all-variables
.EXPORT_ALL_VARIABLES
--- Makefile
.EXPORT_ALL_VARIABLES:
A = a
all: ; @echo "[$$A]"
--- run
make

##### unexport-env
unexport hides an environment variable.
--- Makefile
unexport SECRET
all: ; @echo "[$$SECRET]"
--- run
SECRET=s make

##### define
define and endef.
--- Makefile
define TWO
echo one
echo two
endef
define SIMPLE :=
$(X)
endef
X = x
define NESTED
define INNER
inner
endef
endef
all:
	@$(TWO)
	@echo "[$(SIMPLE)]"
	@echo '$(subst $(newline), ,$(NESTED))'
--- run
make

##### define-append
define with += and ?=.
--- Makefile
A = first
define A +=
second
endef
define B ?=
bee
endef
all: ; @echo "$(A)" "$(B)"
--- run
make

##### canned-recipe
A define used as a recipe; each line gets its own prefix.
--- Makefile
define RUN
@echo step one
-false
@echo step three
endef
all:
	$(RUN)
--- run
make

##### undefine
undefine removes a variable.
--- Makefile
A = a
undefine A
all: ; @echo "[$(A)] $(origin A) $(flavor A)"
--- run
make

##### origin
$(origin) for every kind.
--- Makefile
F = f
override O = o
all:
	@echo $(origin F) $(origin O) $(origin CC) $(origin PATH) $(origin NOPE) $(origin C) $(origin @)
--- run
make C=1

##### flavor
$(flavor)
--- Makefile
R = r
S := s
all: ; @echo $(flavor R) $(flavor S) $(flavor NOPE)
--- run
make

##### value
$(value) does not expand.
--- Makefile
A = $(B) and $$C
B = bee
all: ; @echo '$(value A)'
--- run
make

##### recursion-error
A variable that refers to itself.
--- Makefile
A = $(A) x
all: ; @echo $(A)
--- run
make; echo "status $?"

##### recursion-append-ok
A = $(A) with := is fine.
--- Makefile
A := a
A := $(A) b
all: ; @echo $(A)
--- run
make

##### target-specific
Target-specific variables.
--- Makefile
V = global
all: one two
one: V = one
one: ; @echo one $(V)
two: ; @echo two $(V)
--- run
make

##### target-specific-inherit
Prerequisites inherit target-specific values.
--- Makefile
V = global
all: V = from-all
all: dep ; @echo all $(V)
dep: ; @echo dep $(V)
other: dep
--- run
make
make dep

##### target-specific-append
+= in a target-specific variable.
--- Makefile
CFLAGS = -O2
debug: CFLAGS += -g
debug: ; @echo $(CFLAGS)
rel: ; @echo $(CFLAGS)
--- run
make debug rel

##### target-specific-private
private is not inherited.
--- Makefile
all: private V = secret
all: dep ; @echo "all [$(V)]"
dep: ; @echo "dep [$(V)]"
--- run
make

##### target-specific-override
Command line beats target-specific unless override.
--- Makefile
t1: V = file
t1: ; @echo t1 $(V)
t2: override V = file
t2: ; @echo t2 $(V)
--- run
make t1 t2 V=cmd

##### target-specific-export
export in a target-specific variable.
--- Makefile
t: export TV = tv
t: ; @echo "[$$TV]"
u: ; @echo "[$$TV]"
--- run
make t u

##### pattern-specific
Pattern-specific variables, shorter stem wins.
--- Makefile
all: a.o lib/b.o
%.o: V = generic
lib/%.o: V = lib
a.o lib/b.o: ; @echo $@ $(V)
--- run
make

##### target-specific-simple
:= in a target-specific variable.
--- Makefile
X = early
t: Y := $(X)
X = late
t: ; @echo $(Y)
--- run
make

##### target-specific-cond
?= in a target-specific variable.
--- Makefile
A = global
t: A ?= target
t: B ?= target
t: ; @echo $(A) $(B)
--- run
make

##### multiple-target-specific
Several targets share a target-specific variable.
--- Makefile
a b: V = shared
a b c: ; @echo $@ $(V)
--- run
make a b c

##### makelevel
MAKELEVEL in the makefile and in the environment.
--- Makefile
all: ; @echo $(MAKELEVEL) $$MAKELEVEL
--- run
make

##### curdir
CURDIR
--- Makefile
all: ; @echo $(notdir $(CURDIR)) $(origin CURDIR)
--- file sub/Makefile
all: ; @echo $(notdir $(CURDIR))
--- run
make -s
make -s -C sub

##### makefile-list
MAKEFILE_LIST grows as files are read.
--- Makefile
$(info [$(MAKEFILE_LIST)])
include inc.mk
$(info [$(MAKEFILE_LIST)])
all: ; @echo $(lastword $(MAKEFILE_LIST))
--- file inc.mk
$(info inc [$(MAKEFILE_LIST)])
--- run
make
make -f Makefile

##### makecmdgoals
MAKECMDGOALS
--- Makefile
all a b: ; @echo "$@ [$(MAKECMDGOALS)]"
--- run
make
make a b

##### dot-variables
.VARIABLES contains defined variables.
--- Makefile
MYVAR = 1
all: ; @echo $(sort $(filter MYVAR CC MAKE,$(.VARIABLES)))
--- run
make

##### make-variable
$(MAKE) is how make was called.
--- Makefile
all: ; @echo $(MAKE)
--- run
make

##### shell-variable
SHELL is not taken from the environment.
--- Makefile
all: ; @echo $(SHELL)
--- run
SHELL=/bin/false make

##### automatic
Automatic variables.
--- Makefile
all: out/x.o
out/x.o: src/a.c src/b.h src/a.c | order
	@echo '@=$@ <=$< ^=$^ +=$+ |=$|'
	@echo '@D=$(@D) @F=$(@F) <D=$(<D) <F=$(<F) ^D=$(^D) ^F=$(^F)'
	@echo '*=$* ?=$?'
order:
--- file src/a.c
--- file src/b.h
--- run
make

##### automatic-question
$? lists newer prerequisites.
--- Makefile
out: new old
	@echo '$?'
	@touch out
--- run
touch -d '2001-01-01' old
touch -d '2001-01-02' out
touch new
make
touch -d '2000-01-01' out
make

##### automatic-star-explicit
$* in explicit rules uses known suffixes.
--- Makefile
all: foo.c bar.zz
foo.c bar.zz: ; @echo '[$*]'
--- run
make

##### warn-undefined
--warn-undefined-variables
--- Makefile
A = $(UNDEF_ONE)
all: ; @echo $(A)$(UNDEF_TWO)
--- run
make --warn-undefined-variables 2>&1 | grep -v GNUMAKEFLAGS

##### spaces-in-names
Odd variable names.
--- Makefile
a.b = dot
a-b = dash
1 = one
all: ; @echo $(a.b) $(a-b) $(1)
--- run
make

##### newline-variable
The empty-define newline trick.
--- Makefile
define nl


endef
X = a$(nl)b
all: ; @printf '%s\n' '$(subst $(nl),|,$(X))'
--- run
make

##### cmdline-var-colon
Command-line := and spaces.
--- Makefile
all: ; @echo "[$(A)] [$(B)]"
--- run
make 'A:=x y' B=z
make "A=a b" -- B=b

##### recipe-prefix-in-makeflags
MAKEFLAGS shape.
--- Makefile
all: ; @echo "[$(MAKEFLAGS)] [$(MFLAGS)]"
--- run
make
make -k
make -k -s -i
make -n
make V=1
make -k V=1 W=2
make -r -R
make -B
make -e
make --no-print-directory
make -I inc

##### makeflags-set-in-makefile
MAKEFLAGS set in the makefile takes effect.
--- Makefile
MAKEFLAGS += -s
all: ; echo quiet
--- run
make

##### makeflags-r-in-makefile
-r from the makefile removes built-in rules.
--- Makefile
MAKEFLAGS += -r
all: foo.o
--- file foo.c
--- run
make; echo "status $?"

##### env-makeflags
MAKEFLAGS from the environment.
--- Makefile
V ?= unset
all: ; echo $(V)
--- run
MAKEFLAGS=s make
MAKEFLAGS='-- V=fromflags' make -s
MAKEFLAGS=n make

##### empty-var-ref
$() and $( ) are empty.
--- Makefile
all: ; @echo "[$()] [$( )] [x$(empty)y]"
--- run
make

##### define-continuation
Backslash-newline inside define.
--- Makefile
define D
echo one \
	  two
echo 'three \
four'
endef
X = $(D)
all:
	@$(D)
	@printf '%s\n' '$(subst $(nl),|,$(value D))'
define nl


endef
--- run
make
