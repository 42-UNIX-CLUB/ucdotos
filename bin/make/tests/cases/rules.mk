##### pattern-rule
A pattern rule.
--- Makefile
all: a.out b.out
%.out: %.in
	cp $< $@
--- file a.in
A
--- file b.in
B
--- run
make
make

##### pattern-dir-stem
Directories are kept out of the match and put back.
--- Makefile
all: src/x.o
%.o: %.c ; @echo '$@ from $< stem $*'
--- file src/x.c
--- run
make

##### pattern-with-slash
A pattern with a slash matches the whole name.
--- Makefile
all: out/x.o
out/%.o: src/%.c ; @echo '$@ from $< stem $*'
--- file src/x.c
--- run
make

##### pattern-shortest-stem
The rule with the shortest stem wins.
--- Makefile
all: lib/foo.o
%.o: %.c ; @echo generic $@
lib/%.o: lib/%.c ; @echo specific $@
--- file lib/foo.c
--- run
make

##### pattern-order
With equal stems the first rule wins.
--- Makefile
all: x.o
%.o: %.c ; @echo from c
%.o: %.s ; @echo from s
--- file x.c
--- file x.s
--- run
make

##### pattern-choose-existing
The rule whose prerequisite exists is chosen.
--- Makefile
all: x.o
%.o: %.c ; @echo from c
%.o: %.s ; @echo from s
--- file x.s
--- run
make

##### pattern-multi-target
A pattern rule with two targets runs once.
--- Makefile
all: x.tab.c x.tab.h
%.tab.c %.tab.h: %.y
	@echo run once for $@ $*
	@touch $*.tab.c $*.tab.h
--- file x.y
--- run
make
make

##### pattern-no-recipe-cancels
A pattern rule without a recipe cancels one.
--- Makefile
%.o: %.c ; @echo compile
%.o: %.c
all: x.o
--- file x.c
--- run
make; echo "status $?"

##### pattern-order-only
Order-only prerequisites in a pattern rule.
--- Makefile
all: out/a.txt
out/%.txt: %.src | out
	@cp $< $@ && echo made $@
out:
	@mkdir out && echo made dir
--- file a.src
s
--- run
make
touch a.src
make

##### static-pattern
Static pattern rules.
--- Makefile
OBJS = foo.o bar.o
all: $(OBJS)
$(OBJS): %.o: %.c
	@echo '$@ <- $< ($*)'
--- file foo.c
--- file bar.c
--- run
make

##### static-pattern-mismatch
A target that does not match the pattern.
--- Makefile
all: foo.o bar.x
foo.o bar.x: %.o: %.c ; @echo '$@'
--- file foo.c
--- run
make; echo "status $?"

##### static-pattern-filter
Static pattern with filter.
--- Makefile
files = foo.elc bar.o lose.o
all: $(filter %.o,$(files)) $(filter %.elc,$(files))
$(filter %.o,$(files)): %.o: %.c ; @echo C $@
$(filter %.elc,$(files)): %.elc: %.el ; @echo EL $@
--- file foo.el
--- file bar.c
--- file lose.c
--- run
make

##### double-colon
Double-colon rules run independently.
--- Makefile
out:: a ; @echo from a
out:: b ; @echo from b
--- run
touch -d '2001-01-01' out a b
make
touch a
make

##### double-colon-no-prereqs
A double-colon rule without prerequisites always runs.
--- Makefile
all:: ; @echo always
--- run
touch all
make
make

##### single-double-mix
: and :: on the same target is an error.
--- Makefile
x: ; @echo a
x:: ; @echo b
--- run
make; echo "status $?"

##### order-only
Order-only prerequisites do not cause rebuilds.
--- Makefile
out: in | dir
	@echo build out
	@touch out
dir:
	@mkdir -p dir
	@echo made dir
--- file in
--- run
make
make
touch dir
make
touch in
make

##### order-only-and-normal
A prerequisite listed both ways is normal.
--- Makefile
all: a | a b ; @echo '[$^] [$|]'
a b: ;
--- run
make

##### phony
.PHONY targets always run, even if a file exists.
--- Makefile
.PHONY: clean all
all: ; @echo all
clean: ; @echo cleaning
--- run
touch clean all
make clean
make

##### phony-prereq-forces
A phony prerequisite makes its target out of date.
--- Makefile
out: force ; @echo rebuilt; touch out
force: ;
.PHONY: force
--- run
make
make

##### force-idiom
The FORCE idiom.
--- Makefile
out: FORCE ; @echo rebuilt; touch out
FORCE:
--- run
make
make

##### multiple-rules-merge
Prerequisites from several rules merge; one recipe.
--- Makefile
all: x
x: a
x: b ; @echo '[$^] [$<]'
x: c
a b c: ;
--- run
make

##### override-recipe
A second recipe replaces the first, with a warning.
--- Makefile
all: ; @echo first
all: ; @echo second
--- run
make

##### circular
A circular dependency is dropped.
--- Makefile
a: b ; @echo a
b: a ; @echo b
--- run
make

##### self-dependency
A target that depends on itself.
--- Makefile
a: a ; @echo a
--- run
make

##### grouped
Grouped targets: one recipe run makes all.
--- Makefile
all: x y
x y &: src
	@echo run $@
	@touch x y
--- file src
--- run
make
make

##### not-grouped
Without &: the recipe runs for each target.
--- Makefile
all: x y
x y: src
	@echo run $@
	@touch $@
--- file src
--- run
make

##### default-target
.DEFAULT gives a recipe to files with no rule.
--- Makefile
all: missing1 missing2
.DEFAULT: ; @echo default for $@
--- run
make

##### no-rule
No rule to make target.
--- Makefile
all: missing
--- run
make; echo "status $?"
make -k; echo "status $?"
make nothing-here; echo "status $?"

##### no-rule-keep-going
-k goes on with other prerequisites.
--- Makefile
all: a missing b ; @echo all
a: ; @echo a
b: ; @echo b
--- run
make; echo "status $?"
make -k; echo "status $?"

##### keep-going-recipe
-k after a failing recipe.
--- Makefile
all: bad good ; @echo all
bad: ; @false
good: ; @echo good
--- run
make; echo "status $?"
make -k; echo "status $?"

##### ignore-all
-i ignores every error.
--- Makefile
all: bad ; @echo all
bad: ; false
--- run
make -i; echo "status $?"

##### dot-ignore
.IGNORE for one target.
--- Makefile
.IGNORE: bad
all: bad ; @echo all
bad: ; false
--- run
make; echo "status $?"

##### dot-silent
.SILENT
--- Makefile
.SILENT: quiet
all: quiet loud
quiet: ; echo quiet
loud: ; echo loud
--- run
make

##### dot-silent-all
.SILENT with no prerequisites.
--- Makefile
.SILENT:
all: ; echo hush
--- run
make

##### delete-on-error
.DELETE_ON_ERROR removes a half-made target.
--- Makefile
.DELETE_ON_ERROR:
out: ; echo partial > $@; false
--- run
make; echo "status $?"; ls

##### delete-on-error-precious
.PRECIOUS keeps it.
--- Makefile
.DELETE_ON_ERROR:
.PRECIOUS: out
out: ; echo partial > $@; false
--- run
make; echo "status $?"; ls

##### no-delete-by-default
Without .DELETE_ON_ERROR the file stays.
--- Makefile
out: ; echo partial > $@; false
--- run
make; echo "status $?"; ls

##### second-expansion
.SECONDEXPANSION
--- Makefile
.SECONDEXPANSION:
main_OBJS := main.o util.o
lib_OBJS := lib.o
all: main lib
main lib: $$($$@_OBJS) ; @echo '$@: $^'
%.o: ; @echo obj $@
--- run
make

##### second-expansion-auto
$$< and $$^ in a second expansion.
--- Makefile
.SECONDEXPANSION:
all: x
x: a b $$(addsuffix .2,$$^) ; @echo '$^'
a b a.2 b.2: ;
--- run
make

##### second-expansion-pattern
Second expansion in a pattern rule.
--- Makefile
.SECONDEXPANSION:
all: foo.out
%.out: $$(patsubst %,%.in,$$*) ; @echo '$@ from $^'
foo.in: ;
--- run
make

##### second-expansion-off
Without .SECONDEXPANSION $$ stays a literal dollar.
--- Makefile
X = a
all: $$X ; @echo '$^'
$$X: ; @echo making '$@'
--- run
make

##### wait
.WAIT in a prerequisite list.
--- Makefile
all: a .WAIT b ; @echo all
a b: ; @echo $@
--- run
make
make -j4

##### suffix-rule
Old-style suffix rules.
--- Makefile
.SUFFIXES: .in .out
.in.out:
	@cp $< $@ && echo '$@ from $< ($*)'
all: x.out
--- file x.in
--- run
make

##### suffix-rule-single
A single-suffix rule.
--- Makefile
.SUFFIXES: .sh2
.sh2:
	@echo make $@ from $<
all: prog
--- file prog.sh2
--- run
make

##### suffix-cleared
.SUFFIXES: with no prerequisites disables suffix rules.
--- Makefile
.SUFFIXES:
all: x.o
--- file x.c
--- run
make; echo "status $?"

##### suffix-rule-with-prereq
A suffix rule with prerequisites is an ordinary target.
--- Makefile
.c.o: dep ; @echo odd
dep: ;
all: x.o ; @echo all
--- file x.c
--- run
make -n

##### vpath-var
VPATH finds prerequisites.
--- Makefile
VPATH = src:other
all: prog
prog: a.c b.c ; @echo 'build from $^ first $<'
--- file src/a.c
--- file other/b.c
--- run
make

##### vpath-directive
The vpath directive.
--- Makefile
vpath %.c src
vpath %.h include
all: prog
prog: a.c x.h ; @echo '$^'
--- file src/a.c
--- file include/x.h
--- run
make

##### vpath-pattern-rule
vpath with a pattern rule.
--- Makefile
vpath %.c src
all: a.o
%.o: %.c ; @echo '$@ from $<'
--- file src/a.c
--- run
make

##### vpath-clear
vpath with no directories forgets the pattern.
--- Makefile
vpath %.c src
vpath %.c
all: a.c ; @echo found
--- file src/a.c
--- run
make; echo "status $?"

##### vpath-target-uptodate
A target found through VPATH that is up to date.
--- Makefile
VPATH = build
all: out.txt ; @echo '$<'
out.txt: in.txt ; @echo rebuilding
--- run
mkdir build && touch in.txt && sleep 0.01 && touch build/out.txt && make

##### intermediate
Intermediate files are deleted afterwards.
--- Makefile
all: x.final
%.final: %.mid ; @cp $< $@ && echo final
%.mid: %.src ; @cp $< $@ && echo mid
--- file x.src
s
--- run
make
make
ls

##### intermediate-not-remade
A missing intermediate file does not force a rebuild.
--- Makefile
all: x.final
%.final: %.mid ; @cp $< $@ && echo final
%.mid: %.src ; @cp $< $@ && echo mid
--- file x.src
s
--- run
make
touch x.final
make
touch x.src
make

##### intermediate-explicit
A file mentioned in the makefile is not intermediate.
--- Makefile
all: x.final
x.final: x.mid
%.final: %.mid ; @cp $< $@ && echo final
%.mid: %.src ; @cp $< $@ && echo mid
--- file x.src
s
--- run
make
ls

##### secondary
.SECONDARY keeps intermediate files.
--- Makefile
.SECONDARY: x.mid
all: x.final
%.final: %.mid ; @cp $< $@ && echo final
%.mid: %.src ; @cp $< $@ && echo mid
--- file x.src
--- run
make
ls

##### dot-intermediate
.INTERMEDIATE makes an explicit file intermediate.
--- Makefile
.INTERMEDIATE: x.mid
all: x.final
x.final: x.mid ; @cp $< $@ && echo final
x.mid: x.src ; @cp $< $@ && echo mid
--- file x.src
--- run
make
ls

##### precious-pattern
.PRECIOUS with a pattern keeps intermediates.
--- Makefile
.PRECIOUS: %.mid
all: x.final
%.final: %.mid ; @cp $< $@
%.mid: %.src ; @cp $< $@
--- file x.src
--- run
make
ls

##### notintermediate
.NOTINTERMEDIATE
--- Makefile
.NOTINTERMEDIATE:
all: x.final
%.final: %.mid ; @cp $< $@
%.mid: %.src ; @cp $< $@
--- file x.src
--- run
make
ls

##### chain-three
A chain of three rules.
--- Makefile
all: x.c
%.c: %.b ; @cp $< $@ && echo c
%.b: %.a ; @cp $< $@ && echo b
%.a: %.z ; @cp $< $@ && echo a
--- file x.z
--- run
make
ls

##### terminal-rule
A double-colon pattern rule is terminal.
--- Makefile
all: x.out
%.out:: %.in ; @echo terminal $@
%.in: %.src ; @echo never
--- file x.src
--- run
make; echo "status $?"

##### match-anything
A match-anything rule.
--- Makefile
all: foo
%: %.tmpl ; @echo generate $@ from $<
--- file foo.tmpl
--- run
make

##### match-anything-specific
A match-anything rule is not used for files of a known type.
--- Makefile
%: %.tmpl ; @echo generate $@
all: x.c ; @echo all
--- file x.c.tmpl
--- run
make; echo "status $?"

##### implicit-with-explicit-prereqs
An implicit recipe plus explicit prerequisites.
--- Makefile
all: x.o
x.o: x.h
%.o: %.c ; @echo '$@: $^ [$<]'
--- file x.c
--- file x.h
--- run
make

##### target-is-directory
A target that is a directory.
--- Makefile
all: dir ; @echo all
dir: ; mkdir dir
--- run
make
make

##### wildcard-in-prereqs
Wildcards in prerequisite lists.
--- Makefile
all: *.txt ; @echo '$^'
--- file b.txt
--- file a.txt
--- run
make

##### wildcard-no-match
A wildcard that matches nothing stays as it is.
--- Makefile
all: *.none ; @echo '$^'
--- run
make; echo "status $?"

##### dot-slash
./name and name are the same file.
--- Makefile
all: ./foo foo ; @echo '$^'
foo: ; @echo making $@
--- run
make

##### mixed-implicit-normal
Mixing pattern and normal targets is an error.
--- Makefile
a %.o: ; @echo x
--- run
make; echo "status $?"

##### missing-separator
Lines that are not rules.
--- Makefile
just some words
--- run
make; echo "status $?"

##### eight-spaces
Spaces instead of a tab.
--- Makefile
all:
        echo x
--- run
make; echo "status $?"

##### recipe-before-target
A recipe line before any rule.
--- Makefile
	echo early
all: ;
--- run
make; echo "status $?"

##### tab-line-after-assignment
A tab line after an assignment is a normal line.
--- Makefile
all: ; @echo $(V)
V = v
	W = w
--- run
make; echo "status $?"

##### empty-target-list
A rule whose targets expand to nothing.
--- Makefile
NONE =
$(NONE): foo
	@echo never
all: ; @echo all
--- run
make

##### variable-rule
A rule produced by expanding a variable.
--- Makefile
RULE = gen: ; @echo generated
$(RULE)
--- run
make gen

##### archive-member
Archive members as targets.
--- Makefile
lib.a: lib.a(a.o) lib.a(b.o)
	@echo 'archive $@ newer: $?'
lib.a(%.o): %.o ; @echo 'member $% of $@ from $<'
a.o b.o: ; @touch $@
--- run
make -n

##### dot-silent-messages
.SILENT and the messages make prints itself.
--- Makefile
.SILENT:
all: x.final
%.final: %.mid ; cp $< $@
%.mid: %.src ; cp $< $@
up: ; 
--- file x.src
--- run
make
make
make up
make -t x.final
make -n

##### dot-silent-submake
.SILENT in a sub-make does not hide its directory lines.
--- Makefile
all: ; @$(MAKE) -C sub
--- file sub/Makefile
.SILENT:
all: ; echo sub
--- run
make
