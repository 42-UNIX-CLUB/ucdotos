##### subst
$(subst)
--- Makefile
all:
	@echo "$(subst ee,EE,feet on the street)"
	@echo "[$(subst a,,banana)]"
	@echo "[$(subst ,x,abc)]"
	@echo "[$(subst a,b,)]"
--- run
make

##### patsubst
$(patsubst)
--- Makefile
all:
	@echo $(patsubst %.c,%.o,x.c.c bar.c baz.h)
	@echo $(patsubst x%,y%,xa xb ab)
	@echo $(patsubst foo,bar,foo food foo)
	@echo $(patsubst %,[%],a b  c)
	@echo $(patsubst %.c,obj,a.c b.h)
	@echo $(patsubst \%%,P%,%x %y)
--- run
make

##### strip
$(strip)
--- Makefile
X =    a    b   c
all: ; @echo "[$(strip $(X)  )]" "[$(strip )]"
--- run
make

##### findstring
$(findstring)
--- Makefile
all: ; @echo "[$(findstring a,a b c)] [$(findstring a,b c)] [$(findstring ,abc)]"
--- run
make

##### filter
$(filter) and $(filter-out)
--- Makefile
SRC = foo.c bar.c baz.s ugh.h
all:
	@echo $(filter %.c %.s,$(SRC))
	@echo $(filter-out %.c,$(SRC))
	@echo $(filter foo.c ugh.h,$(SRC))
	@echo "[$(filter %.x,$(SRC))]"
	@echo $(filter-out foo.c bar.c,$(SRC))
--- run
make

##### sort
$(sort)
--- Makefile
all: ; @echo $(sort foo bar lose foo  bar zz Aa a 10 9)
--- run
make

##### word-functions
word, wordlist, words, firstword, lastword
--- Makefile
L = a b c d e
all:
	@echo $(word 2,$(L)) "[$(word 9,$(L))]"
	@echo $(wordlist 2,4,$(L)) "[$(wordlist 4,2,$(L))]" $(wordlist 3,99,$(L))
	@echo $(words $(L)) $(words ) $(firstword $(L)) $(lastword $(L)) "[$(firstword )]"
--- run
make

##### word-errors
Bad arguments to word.
--- Makefile
all: ; @echo $(word x,a b)
z: ; @echo $(word 0,a b)
--- run
make; echo "status $?"
make z; echo "status $?"

##### dir-notdir
$(dir) $(notdir)
--- Makefile
all:
	@echo $(dir src/foo.c hacks /abs/x dir/)
	@echo $(notdir src/foo.c hacks /abs/x dir/)
--- run
make

##### suffix-basename
$(suffix) $(basename)
--- Makefile
all:
	@echo $(suffix src/foo.c src-1.0/bar.c hacks a.b.c .rc d.x/y)
	@echo $(basename src/foo.c src-1.0/bar hacks a.b.c .rc d.x/y)
--- run
make

##### add-join
addsuffix, addprefix, join
--- Makefile
all:
	@echo $(addsuffix .c,foo bar) $(addprefix src/,foo bar)
	@echo $(join a b,.c .o) $(join a b c,1) $(join a,1 2 3)
--- run
make

##### wildcard
$(wildcard)
--- Makefile
all:
	@echo $(wildcard *.c)
	@echo $(wildcard sub/*.h nope*.x)
	@echo $(wildcard sub/*)
	@echo "[$(wildcard *.none)]"
	@echo $(wildcard [ab].c) $(wildcard ?.c) $(wildcard sub)
--- file b.c
--- file a.c
--- file cc.c
--- file sub/x.h
--- file sub/y.h
--- run
make

##### realpath-abspath
$(realpath) and $(abspath)
--- Makefile
all:
	@echo $(patsubst $(CURDIR)/%,%,$(abspath a/../b ./c //d/./e/.. x/))
	@echo $(patsubst $(CURDIR)/%,%,$(realpath Makefile nope sub/../Makefile))
--- file sub/f
--- run
make

##### if
$(if)
--- Makefile
E =
F = x
all:
	@echo $(if $(F),yes,no) $(if $(E),yes,no) "[$(if $(E),yes)]" $(if  ,a,b,c)
	@echo $(if x,$(info then-branch)ok,$(info else-branch))
--- run
make

##### or-and
$(or) and $(and)
--- Makefile
all:
	@echo "[$(or ,,b,c)] [$(or ,)] [$(and a,b,c)] [$(and a,,c)]"
	@echo "[$(or $(info o1),x,$(info never))]"
--- run
make

##### intcmp
$(intcmp)
--- Makefile
all:
	@echo "[$(intcmp 1,2,lt,eq,gt)] [$(intcmp 2,2,lt,eq,gt)] [$(intcmp 3,2,lt,eq,gt)]"
	@echo "[$(intcmp 3,2,lt,eq)] [$(intcmp 3,2,lt)] [$(intcmp 5,5)] [$(intcmp 5,6)]"
--- run
make

##### foreach
$(foreach)
--- Makefile
dirs := a b c
X = outer
all:
	@echo $(foreach d,$(dirs),$(d)/file)
	@echo "[$(foreach d,,x)]" $(foreach X,1 2,$(X)) $(X)
	@echo $(foreach i,1 2,$(foreach j,a b,$(i)$(j)))
	@echo "[$(foreach x,a b c,)]"
--- run
make

##### let
$(let)
--- Makefile
all: ; @echo $(let a b c,1 2 3 4 5,[$(a)] [$(b)] [$(c)])
--- run
make

##### call
$(call)
--- Makefile
reverse = $(2) $(1)
pathsearch = $(firstword $(wildcard $(addsuffix /$(1),$(subst :, ,$(PATH)))))
map = $(foreach a,$(2),$(call $(1),$(a)))
quote = '$(1)'
nested = $(call inner,x) [$(1)] [$(2)]
inner = in:$(1):$(2)
all:
	@echo $(call reverse,a,b)
	@echo $(call map,quote,x y)
	@echo $(call nested,A,B)
	@echo "[$(call undefined,x)]" "[$(call reverse)]"
	@echo $(call info,from call)
	@echo $(call 0name) $(call reverse,a,b,c)
--- run
make

##### call-zero
$(0) is the variable name.
--- Makefile
f = $(0):$(1)
all: ; @echo $(call f,arg)
--- run
make

##### eval
$(eval) creates rules and variables.
--- Makefile
PROGS = foo bar
define RULE
$(1): ; @echo building $(1) with $$@
$(1)_VAR = value-$(1)
endef
$(foreach p,$(PROGS),$(eval $(call RULE,$(p))))
all: $(PROGS) ; @echo $(foo_VAR) $(bar_VAR)
--- run
make all

##### eval-in-recipe
$(eval) while a recipe expands.
--- Makefile
all:
	$(eval X := set-in-recipe)
	@echo $(X)
--- run
make

##### shell
$(shell)
--- Makefile
X := $(shell echo hello; echo there)
Y := $(shell exit 3)
S := $(.SHELLSTATUS)
Z := $(shell printf 'a\n\nb\n\n')
all: ; @echo "[$(X)] [$(Y)] [$(S)] [$(Z)]"
--- run
make

##### shell-export
Exported variables reach $(shell).
--- Makefile
export EXPV = exported
X := $(shell echo "[$$EXPV]")
all: ; @echo $(X)
--- run
make

##### info-warning-error
$(info) $(warning) $(error)
--- Makefile
$(info info line)
$(warning warning line)
all:
	@echo recipe
	$(warning in recipe)
$(error stop here)
--- run
make; echo "status $?"

##### error-in-recipe
$(error) while a recipe expands.
--- Makefile
all:
	@echo first
	@echo $(error recipe error)
--- run
make; echo "status $?"

##### file-function
$(file)
--- Makefile
$(file >out.txt,line one)
$(file >>out.txt,line two)
$(file >empty.txt)
X := $(file <out.txt)
all: ; @echo "[$(X)]" "[$(file <nope)]"
--- run
make

##### function-commas
Commas and parentheses in arguments.
--- Makefile
comma := ,
space := $(subst ,, )
all:
	@echo $(subst $(comma),;,a,b,c)
	@echo "$(subst $(space),_,a b c)"
	@echo $(info a,b,c)
	@echo $(patsubst (%),[%],(x) (y))
	@echo $(if x,a,b,c)
--- run
make

##### function-nesting
Nested calls with both kinds of parentheses.
--- Makefile
all:
	@echo ${subst a,b,$(subst x,a,xyz)}
	@echo $(words ${wildcard nothing*} a b)
--- run
make

##### missing-args
Too few arguments.
--- Makefile
all: ; @echo $(subst a)
--- run
make; echo "status $?"

##### unterminated
An unterminated reference.
--- Makefile
X = $(foo
all: ; @echo $(X)
--- run
make; echo "status $?"

##### hash-in-function
# inside a function call is not a comment.
--- Makefile
X := $(subst :,#,a:b)
all: ; @echo '$(X)'
--- run
make
