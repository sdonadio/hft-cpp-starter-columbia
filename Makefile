# HFT starter — local autograder + per-challenge quick builds.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -pthread -Iinclude -Itests

.PHONY: test clean pool book fix u64toa rolling spsc client register run

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

# ── Arena: build the C++ client, register your team, run your bot ─────────────
ARENA ?= https://algoarenafin.duckdns.org
client:                    ## build hft/cpp_client with TLS -> hft/cpp_client/build/hft_bot
	cmake -S hft/cpp_client -B hft/cpp_client/build -DHFT_USE_TLS=ON && cmake --build hft/cpp_client/build
register:                  ## make register CODE=<class code> NAME="Your Team"  (writes .env)
	python3 scripts/register.py --arena $(ARENA) --code $(CODE) --name "$(NAME)"
run:                       ## start your bot with the credentials in .env
	./run_bot.sh
