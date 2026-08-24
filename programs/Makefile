.PHONY: all clean run

CXX ?= c++
CXXFLAGS ?= -O2 -pipe
WARNFLAGS ?= -Wall -Wextra -Wpedantic
CPPFLAGS ?= -I../include
LDFLAGS ?=
LDLIBS ?=

TARGETS = femtofsSim makefemtofs

all: $(TARGETS)

femtofsSim: femtofsSim.cpp ../include/isPrime.h ../include/femtofs/format.h \
		../include/femtofs/hash_plan.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNFLAGS) -std=c++20 femtofsSim.cpp $(LDFLAGS) $(LDLIBS) -o $@

makefemtofs: makefemtofs.cpp ../include/isPrime.h ../include/femtofs/format.h \
		../include/femtofs/hash_plan.h
	@set -e; \
	archive_cflags=""; archive_libs="-larchive"; \
	if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libarchive; then \
		archive_cflags="$$(pkg-config --cflags libarchive)"; \
		archive_libs="$$(pkg-config --libs libarchive)"; \
	elif command -v brew >/dev/null 2>&1; then \
		archive_prefix="$$(brew --prefix libarchive)"; \
		archive_cflags="-I$$archive_prefix/include"; \
		archive_libs="-L$$archive_prefix/lib -larchive"; \
	fi; \
	$(CXX) $(CPPFLAGS) $$archive_cflags $(CXXFLAGS) $(WARNFLAGS) -std=c++20 \
		makefemtofs.cpp $(LDFLAGS) $$archive_libs $(LDLIBS) -o $@

run: femtofsSim
	./femtofsSim ../misc/list

clean:
	rm -f $(TARGETS)
