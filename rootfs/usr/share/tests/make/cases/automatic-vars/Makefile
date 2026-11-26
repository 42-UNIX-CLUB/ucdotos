out.txt: a.txt b.txt a.txt | dir.txt
	@echo '@=$@ <=$< ^=$^ +=$+ |=$| *=$* @D=$(@D) @F=$(@F)'
a.txt b.txt dir.txt: ; @touch $@
