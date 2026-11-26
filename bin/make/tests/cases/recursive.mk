##### recursive-basic
$(MAKE) -C sub
--- Makefile
all:
	@echo top
	$(MAKE) -C sub
	@echo back
--- file sub/Makefile
all: ; @echo sub level $(MAKELEVEL)
--- run
make

##### recursive-no-print-directory
--no-print-directory in a recursive make.
--- Makefile
all: ; @$(MAKE) --no-print-directory -C sub
--- file sub/Makefile
all: ; @echo sub
--- run
make

##### recursive-s
-s propagates and turns off the directory lines.
--- Makefile
all: ; $(MAKE) -C sub
--- file sub/Makefile
all: ; echo sub
--- run
make -s

##### recursive-flags
Flags and variables reach the sub-make.
--- Makefile
all: ; @$(MAKE) -C sub
--- file sub/Makefile
all: ; @echo "k=$(findstring k,$(firstword $(MAKEFLAGS))) V=$(V) origin=$(origin V)"
--- run
make -k V=val
make V=x

##### recursive-n
-n reaches sub-makes, whose lines are printed.
--- Makefile
all:
	$(MAKE) -C sub
--- file sub/Makefile
all: ; echo in sub
--- run
make -n

##### recursive-error
A failing sub-make.
--- Makefile
all:
	$(MAKE) -C sub
	@echo not reached
--- file sub/Makefile
all: ; @exit 4
--- run
make; echo "status $?"

##### recursive-keep-going
-k across sub-makes.
--- Makefile
all: s1 s2
s1 s2: ; @$(MAKE) -C $@
.PHONY: s1 s2
--- file s1/Makefile
all: ; @false
--- file s2/Makefile
all: ; @echo s2 ok
--- run
make -k; echo "status $?"

##### recursive-export
Exported variables reach the sub-make's environment.
--- Makefile
export A = exported
B = notexported
all: ; @$(MAKE) -s -C sub
--- file sub/Makefile
all: ; @echo "[$(A)] [$(B)] [$(origin A)]"
--- run
make
make B=cmdline

##### recursive-plus
+ runs a line under -n.
--- Makefile
all:
	+@echo plus runs
	@echo plain
--- run
make -n

##### recursive-make-var-name
$(MAKE) inside a variable is still recursive under -n.
--- Makefile
SUB = $(MAKE) -s -f sub.mk
all: ; $(SUB)
--- file sub.mk
x: ; @echo sub
--- run
make -n

##### recursive-makelevel
MAKELEVEL counts the depth.
--- Makefile
all: ; @$(MAKE) -s -f lvl.mk
--- file lvl.mk
all: ; @echo $(MAKELEVEL); $(if $(filter 3,$(MAKELEVEL)),,$(MAKE) -f lvl.mk)
--- run
make

##### recursive-cmdgoals
Goals of the sub-make.
--- Makefile
all: ; @$(MAKE) -s -C sub one two
--- file sub/Makefile
one two: ; @echo $@ [$(MAKECMDGOALS)]
--- run
make

##### recursive-cd
cd in a recipe.
--- Makefile
all: ; cd sub && $(MAKE)
--- file sub/Makefile
all: ; @echo in $(notdir $(CURDIR))
--- run
make

##### recursive-override-cmdline
Command-line variables are passed down.
--- Makefile
X = top
all: ; @$(MAKE) -s -C sub X=$(X)2
--- file sub/Makefile
X = sub
all: ; @echo $(X)
--- run
make
make X=cmd
