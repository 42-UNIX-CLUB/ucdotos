##### simple
A single rule and its recipe.
--- Makefile
all:
	echo hello
--- run
make
make all

##### default-goal-first
The first target is the default goal.
--- Makefile
first: ; @echo first
second: ; @echo second
--- run
make
make second first

##### default-goal-skips-dot
Targets starting with a dot are not default goals.
--- Makefile
.hidden: ; @echo hidden
%.x: ; @echo pattern
dir/.ok: ; @echo dirdot
real: ; @echo real
--- run
make

##### default-goal-var
.DEFAULT_GOAL chooses the goal.
--- Makefile
.DEFAULT_GOAL := two
one: ; @echo one
two: ; @echo two
--- run
make

##### default-goal-reset
Clearing .DEFAULT_GOAL lets the next rule set it.
--- Makefile
one: ; @echo one
$(info goal=$(.DEFAULT_GOAL))
.DEFAULT_GOAL :=
two: ; @echo two
--- run
make

##### up-to-date
Messages for targets with nothing to do.
--- Makefile
all: out
out: in
	cp in out
--- file in
data
--- run
make
make
make out
make all

##### nothing-to-be-done
A target with no recipe.
--- Makefile
all: in
--- file in
x
--- run
make
make in

##### multiple-targets
A rule with several targets is one rule per target.
--- Makefile
a b c:
	@echo making $@
--- run
make a b c
make b

##### line-continuation
Backslash-newline in makefile lines and in recipes.
--- Makefile
X = one \
    two \
	three
all:
	@echo $(X)
	@echo a \
	b
	@echo 'x \
	y'
--- run
make

##### comments
Comments in all their places.
--- Makefile
# a comment
X = value # trailing comment
Y = with\#hash
Z = $(subst a,#,abc)
all: # comment after the colon
	@echo "[$(X)]" "[$(Y)]" "[$(Z)]"
	# this goes to the shell
	@# and this too
--- run
make

##### semicolon-recipe
Recipes after a semicolon.
--- Makefile
all: dep ; @echo all $<
dep: ; @echo dep
--- run
make

##### recipe-prefix
.RECIPEPREFIX changes the recipe character.
--- Makefile
.RECIPEPREFIX = >
all:
> @echo with prefix
> @echo second line
--- run
make

##### blank-lines-in-recipe
Blank and comment lines do not end a recipe.
--- Makefile
all:
	@echo one

	@echo two
# comment
	@echo three
--- run
make

##### at-sign
@ hides the command line.
--- Makefile
all:
	@echo silent
	echo loud
--- run
make
make -s
make -n

##### dash-ignore
- ignores errors.
--- Makefile
all:
	-false
	@echo after
--- run
make; echo "status $?"

##### error-stops
An error stops make.
--- Makefile
all:
	@echo before
	false
	@echo never
--- run
make; echo "status $?"

##### exit-code-in-message
The exit code appears in the message.
--- Makefile
all:
	exit 3
--- run
make; echo "status $?"

##### prereq-order
Prerequisites are made left to right.
--- Makefile
all: a b c
	@echo all
a: ; @echo a
b: ; @echo b
c: ; @echo c
--- run
make

##### shared-prereq
A prerequisite is made once.
--- Makefile
all: x y
x: z ; @echo x
y: z ; @echo y
z: ; @echo z
--- run
make

##### timestamps
Out of date only when a prerequisite is newer.
--- Makefile
out: in1 in2
	@echo rebuild
	@touch out
--- run
touch -d '2001-01-01' in1 in2
make
make
touch -d '2001-01-02' out
make
touch in2
make

##### missing-target-file
A target that is never created is remade each time.
--- Makefile
all: phonyish
phonyish:
	@echo running
--- run
make
make

##### dollar-dollar
$$ in recipes.
--- Makefile
all:
	@x=5; echo $$x
	@echo '$$HOME is not expanded by make'
--- run
make

##### var-in-recipe-expanded-late
Recipes expand when they run.
--- Makefile
all:
	@echo $(X)
X = late
--- run
make

##### crlf
CRLF line endings are tolerated.
--- Makefile
all:
	@echo crlf
--- run
printf 'all:\r\n\t@echo crlf\r\n' > Makefile; make

##### empty-makefile
No targets.
--- Makefile
# nothing
--- run
make; echo "status $?"

##### no-makefile
No makefile at all.
--- run
make; echo "status $?"
make foo; echo "status $?"

##### lowercase-makefile
makefile is read before Makefile.
--- file makefile
all: ; @echo lower
--- file Makefile
all: ; @echo upper
--- run
make

##### gnumakefile
GNUmakefile wins.
--- file GNUmakefile
all: ; @echo gnu
--- file makefile
all: ; @echo lower
--- run
make

##### dash-f
-f picks the makefile; several -f are read in order.
--- file one.mk
a: ; @echo a $(V)
--- file two.mk
V = from-two
b: ; @echo b
--- run
make -f one.mk
make -f one.mk -f two.mk a b
make --file=two.mk
make --makefile two.mk

##### dash-f-stdin
-f - reads standard input.
--- run
printf 'all:\n\t@echo from stdin\n' | make -f -

##### dash-f-missing
-f with a missing file.
--- run
make -f nope.mk; echo "status $?"

##### dash-C
-C changes directory.
--- file sub/Makefile
all: ; @echo in sub $(notdir $(CURDIR))
--- run
make -C sub
make -C sub --no-print-directory
make --directory=sub -s
make -C nonexistent; echo "status $?"

##### dash-C-twice
-C is cumulative.
--- file a/b/Makefile
all: ; @echo deep
--- run
make -C a -C b

##### dash-w
-w prints the directory.
--- Makefile
all: ; @echo hi
--- run
make -w
make -w -s

##### unknown-option
A bad option.
--- Makefile
all: ; @echo hi
--- run
make -Z 2>/dev/null; echo "status $?"
make --no-such-option 2>/dev/null; echo "status $?"

##### command-not-found
A command that does not exist.
--- Makefile
all:
	nonexistent-command-xyz arg
--- run
make; echo "status $?"

##### long-line
A long recipe line.
--- Makefile
X := $(foreach i,1 2 3 4 5 6 7 8 9 10,word$(i)word$(i)word$(i)word$(i)word$(i)word$(i)word$(i)word$(i))
all: ; @echo $(X) | wc -c
--- run
make
