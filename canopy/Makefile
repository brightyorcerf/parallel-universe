CXX ?= g++
CXXFLAGS ?= -std=c++23 -O3 -march=native -pthread -Wall -Wextra
# Windows/MinGW: link statically so bench.exe runs without the toolchain on PATH.
ifeq ($(OS),Windows_NT)
LDFLAGS += -static
endif

bench: src/bench.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/bench.cpp $(LDFLAGS)

test: bench
	./bench test

clean:
	rm -f bench bench.exe
.PHONY: test clean
