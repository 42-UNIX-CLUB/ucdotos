##### error-line-in-included
Errors report the included file and line.
--- Makefile
include inc.mk
all: ;
--- file inc.mk
X = 1
bad line here
--- run
make; echo "status $?"

##### error-in-eval
An error inside $(eval).
--- Makefile
$(eval $$(error from eval))
--- run
make; echo "status $?"

##### error-in-variable
$(error) inside a variable reports where it is expanded.
--- Makefile
E = $(error deferred)

all: ; @echo $(E)
--- run
make; echo "status $?"

##### warning-in-function
$(warning) location.
--- Makefile
W = $(warning from var)
X := $(W)
all: ;
--- run
make

##### recipe-fails-second-line
The failing line number is the recipe line.
--- Makefile
all:
	@true
	@true
	@exit 2
--- run
make; echo "status $?"

##### recipe-fails-implicit
A failing implicit recipe.
--- Makefile
%.out: %.in ; @exit 1
all: x.out
--- file x.in
--- run
make; echo "status $?"

##### builtin-recipe-fails
A failing built-in recipe.
--- file bad.c
this is not C
--- run
make bad.o 2>&1 | grep -v 'error\|bad.c\|this is\|^ *|\|^ *\^\|compilation\|~'; echo done

##### keep-going-two-goals
-k with two goals.
--- Makefile
a: ; @exit 1
b: ; @echo b
--- run
make -k a b; echo "status $?"
make a b; echo "status $?"

##### keep-going-nested
-k: deep failure.
--- Makefile
all: x y ; @echo all
x: x1 ; @echo x
x1: ; @exit 1
y: ; @echo y
--- run
make -k; echo "status $?"

##### no-rule-nested
No rule for a deep prerequisite.
--- Makefile
all: x ; @echo all
x: missing ; @echo x
--- run
make; echo "status $?"
make -k; echo "status $?"

##### override-without-assign
override with nothing after it.
--- Makefile
override
all: ;
--- run
make; echo "status $?"

##### unterminated-define
A define without endef.
--- Makefile
define X
abc
--- run
make; echo "status $?"

##### extraneous-endef
An endef without define.
--- Makefile
endef
--- run
make; echo "status $?"

##### target-pattern-no-percent
A static pattern without %.
--- Makefile
a: b: c ; @echo x
--- run
make; echo "status $?"

##### ignored-error-message
The message for an ignored error.
--- Makefile
all:
	-@exit 5
	@echo next
--- run
make; echo "status $?"

##### ignore-and-keep-going
-i and -k together.
--- Makefile
all: a b ; @echo all
a: ; @exit 1
b: ; @echo b
--- run
make -i -k; echo "status $?"

##### stop-on-first-goal
Without -k the second goal is not tried.
--- Makefile
a: ; @exit 1
b: ; @echo b
--- run
make a b; echo "status $?"

##### error-function-status
$(error) exits with 2 before running anything.
--- Makefile
all: ; @echo should not run
$(if $(MISSING),,$(error MISSING is not set))
--- run
make; echo "status $?"
make MISSING=1; echo "status $?"

##### circular-chain
A longer cycle.
--- Makefile
a: b ; @echo a
b: c ; @echo b
c: a ; @echo c
--- run
make

##### empty-variable-name
An assignment with no name.
--- Makefile
 = value
all: ;
--- run
make; echo "status $?"
