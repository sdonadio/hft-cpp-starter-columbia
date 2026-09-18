# HW 2 — pointers, references & the cost of a copy.
#
#   make          build -O2 and run           (this is the build you report)
#   make debug    build -O0 and run           (to SEE why a debug build lies)
#   make clean
#
# One-liner, no make needed:
#   g++ -std=c++17 -O2 -Wall hw2.cpp -o hw2 && ./hw2

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: all debug clean

all: hw2                   ## release build — REPORT THESE NUMBERS
	./hw2

hw2: hw2.cpp ../../tests/bench.hpp
	$(CXX) $(CXXFLAGS) hw2.cpp -o hw2

debug: hw2.cpp             ## -O0: shows you what an un-optimised "benchmark" claims
	$(CXX) -std=c++17 -O0 -Wall -Wextra hw2.cpp -o hw2_O0
	./hw2_O0

clean:
	rm -f hw2 hw2_O0
