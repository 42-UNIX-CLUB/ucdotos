##### c-object
The built-in %.o: %.c rule (dry run).
--- file foo.c
int f(void) { return 1; }
--- run
make -n foo.o
make -n foo.o CC=mycc CFLAGS=-O2 CPPFLAGS=-DX TARGET_ARCH=-m

##### c-program
The built-in rule that links a program from one source.
--- file hello.c
int main(void) { return 0; }
--- run
make -n hello
make -n hello LDFLAGS=-L. LDLIBS=-lm

##### c-program-from-object
A program from its object and other objects.
--- Makefile
prog: prog.o util.o
--- file prog.c
int main(void) { return 0; }
--- file util.c
int u(void) { return 0; }
--- run
make -n

##### c-real-build
A real compile and link with the host cc.
--- Makefile
CFLAGS = -O1
prog: prog.o util.o
prog.o: util.h
--- file prog.c
#include "util.h"
int main(void) { return u(); }
--- file util.c
int u(void) { return 0; }
--- file util.h
int u(void);
--- run
make
./prog; echo "prog exit $?"
make
touch util.h
make

##### cxx-object
C++ rules.
--- file a.cc
--- file b.cpp
--- file c.C
--- run
make -n a.o b.o c.o

##### asm-object
Assembler rules.
--- file a.s
--- file b.S
--- run
make -n a.o b.o

##### yacc-lex-chain
A chain through yacc.
--- file p.y
--- run
make -n p.o

##### lex-chain
A chain through lex.
--- file l.l
--- run
make -n l.c

##### shell-script-rule
The %: %.sh rule.
--- file tool.sh
echo tool
--- run
make tool
./tool

##### builtin-vars
Values of built-in variables.
--- Makefile
all:
	@echo "CC=$(CC) CXX=$(CXX) AR=$(AR) ARFLAGS=$(ARFLAGS) RM=$(RM)"
	@echo "COMPILE.c=$(value COMPILE.c)"
	@echo "LINK.c=$(value LINK.c)"
	@echo "LINK.o=$(value LINK.o)"
	@echo "OUTPUT_OPTION=$(value OUTPUT_OPTION) CPP=$(value CPP)"
	@echo "origin=$(origin CC) $(origin CFLAGS)"
--- run
make

##### override-builtin-rule
A user rule replaces the built-in one.
--- Makefile
%.o: %.c ; @echo custom $@
all: x.o
--- file x.c
--- run
make

##### cancel-builtin
A pattern rule with no recipe cancels a built-in one.
--- Makefile
%.o: %.c
all: x.o
--- file x.c
--- run
make; echo "status $?"

##### suffix-overrides-builtin
A user suffix rule replaces the built-in one.
--- Makefile
.c.o: ; @echo suffix $@ from $<
all: x.o
--- file x.c
--- run
make

##### no-makefile-builtin
make with no makefile can still use built-in rules.
--- file prog.c
int main(void) { return 0; }
--- run
make -n prog

##### archive-rule
The (%): % archive rule.
--- Makefile
lib.a: lib.a(x.o)
--- file x.o
--- run
make -n

##### dot-default-suffixes
$(SUFFIXES) holds the default list.
--- Makefile
all: ; @echo $(words $(SUFFIXES)) $(firstword $(SUFFIXES))
--- run
make
make -r
