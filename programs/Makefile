.PHONY: all clean run

CXX ?= c++
CXXFLAGS ?= -O2 -pipe
CPPFLAGS ?= -I../include
LDFLAGS ?=
LDLIBS ?=

TARGET = femtofsSim
SRCS = femtofsSim.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 $(SRCS) $(LDFLAGS) $(LDLIBS) -o $(TARGET)

run: $(TARGET)
	./$(TARGET) ../misc/list

clean:
	rm -f $(TARGET)
