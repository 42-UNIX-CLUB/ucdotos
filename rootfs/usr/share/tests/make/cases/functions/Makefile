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
