##### shell-features
Recipes that need a shell.
--- Makefile
all:
	@for i in 1 2 3; do echo $$i; done
	@if [ -f Makefile ]; then echo yes; fi
	@echo $$(echo sub) `echo back`
	@x=1 && y=2 && echo $$x$$y
	@false || echo or-branch
	@(echo subshell)
	@echo a > f.txt; cat < f.txt
	@echo *.txt
--- run
make

##### each-line-own-shell
Each line runs in its own shell.
--- Makefile
all:
	@x=1; echo "[$$x]"
	@echo "[$$x]"
	@cd /; pwd
	@pwd | sed 's,.*/,,'
--- run
make

##### oneshell
.ONESHELL runs the recipe in one shell.
--- Makefile
.ONESHELL:
all:
	@x=1
	echo "[$$x]"
	cd /
	pwd
--- run
make

##### shell-var
SHELL selects the shell.
--- Makefile
SHELL = /bin/bash
all: ; @echo $$BASH_VERSION | cut -c1-1 | grep -q . && echo bash
--- run
make

##### shellflags
.SHELLFLAGS
--- Makefile
.SHELLFLAGS = -ec
all:
	@false; echo not reached
--- run
make; echo "status $?"

##### posix
.POSIX makes the shell use -e.
--- Makefile
.POSIX:
all:
	@false; echo not reached
--- run
make; echo "status $?"

##### quoting
Quotes and special characters pass through.
--- Makefile
all:
	@echo 'single $$HOME' "double" back\ slash
	@echo "tab	inside"
	@printf '%s\n' "percent % sign"
--- run
make

##### prefix-combos
@, - and + together.
--- Makefile
all:
	@-false
	-@false
	@echo done
--- run
make; echo "status $?"

##### prefix-after-expansion
Prefixes that come from a variable.
--- Makefile
Q = @
all:
	$(Q)echo quiet
	$(Q)-false
	@echo end
--- run
make

##### empty-recipe-line
A recipe line that expands to nothing.
--- Makefile
E =
all:
	$(E)
	@echo after
--- run
make

##### backslash-recipe
Backslash-newline in a recipe is passed to the shell.
--- Makefile
all:
	@echo one \
	two \
	  three
	@echo 'a\
	b'
--- run
make

##### signal-exit
A recipe killed by a signal.
--- Makefile
all: ; @kill -TERM $$$$
--- run
make; echo "status $?"

##### recipe-env
The recipe environment has MAKEFLAGS and MAKELEVEL.
--- Makefile
all: ; @echo "[$$MAKELEVEL] [$$MAKEFLAGS]"
--- run
make
make -k

##### fast-path-errors
A missing program, with and without a shell.
--- Makefile
all:
	-no-such-prog-1
	-no-such-prog-2 | cat
	-./not-executable
--- file not-executable
--- run
make 2>&1 | sed 's/^.*sh[^:]*: [0-9]*: //; s/^.*sh: line [0-9]*: //'

##### recipe-cwd
Recipes run in make's directory after -C.
--- file sub/Makefile
all: ; @pwd | sed 's,.*/,,'
--- run
make -s -C sub

##### long-command
A very long command line.
--- Makefile
W := $(foreach i,$(shell seq 1 2000),word$(i))
all: ; @echo $(W) | wc -w
--- run
make

##### dollar-at-in-define
A canned recipe with automatic variables.
--- Makefile
define compile
@echo compiling $< into $@
endef
all: x.o
x.o: x.c ; $(compile)
--- file x.c
--- run
make
