# HFT starter — local autograder + per-challenge quick builds.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -pthread -Iinclude -Itests

.PHONY: test clean pool book fix u64toa rolling spsc

test:                      ## run the full autograder (grades what you've implemented)
	python3 tests/run_ci.py

# quick single-challenge builds while you iterate:
pool:    ; $(CXX) $(CXXFLAGS) tests/pool_test.cpp    -o /tmp/pool    && /tmp/pool
book:    ; $(CXX) $(CXXFLAGS) tests/book_test.cpp    -o /tmp/book    && /tmp/book
fix:     ; $(CXX) $(CXXFLAGS) tests/fix_test.cpp     -o /tmp/fix     && /tmp/fix
u64toa:  ; $(CXX) $(CXXFLAGS) tests/u64toa_test.cpp  -o /tmp/u64toa  && /tmp/u64toa
rolling: ; $(CXX) $(CXXFLAGS) tests/rolling_counter_test.cpp -o /tmp/rolling && /tmp/rolling
spsc:    ; $(CXX) $(CXXFLAGS) tests/spsc_correctness.cpp -o /tmp/spsc && /tmp/spsc

clean:   ; rm -f /tmp/pool /tmp/book /tmp/fix /tmp/u64toa /tmp/rolling /tmp/spsc report.json
