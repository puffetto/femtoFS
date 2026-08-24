# File: tmp/Makefile
# Created by Andrea "Nemesi" Cocito on 23/08/2026
# Build a single C source file with BSD make or GNU make.

program ?=

CC ?= cc
CPPFLAGS ?=
CFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

CSTANDARD ?= -std=c23
CWARNINGS ?= -Wall

.PHONY: all clean

all:
	@test -n "$(program)" || { echo "usage: make program=<name>" >&2; exit 2; }
	@$(MAKE) "$(program)"

.SUFFIXES: .c

.c:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CSTANDARD) $(CWARNINGS) "$<" $(LDFLAGS) $(LDLIBS) -o "$@"

clean:
	@test -n "$(program)" || { echo "usage: make clean program=<name>" >&2; exit 2; }
	rm -f "$(program)"

# END File: tmp/Makefile
