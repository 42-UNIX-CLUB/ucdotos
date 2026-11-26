OBJS = x.o y.o
all: $(OBJS)
$(OBJS): %.o: %.c
	@echo $@ from $<
	@touch $@
