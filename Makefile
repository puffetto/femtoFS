.PHONY: all clean run

all:
	$(MAKE) -C programs all

clean:
	$(MAKE) -C programs clean

run: all
	./programs/femtofsSim misc/list
