CXX      ?= g++
CXXFLAGS ?= -O3 -march=native
BINS      = bench ooc_bench pl_bench entmax_bench

all: $(BINS)

bench: src/bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<
ooc_bench: src/ooc_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<
pl_bench: src/pl_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<
entmax_bench: src/entmax_bench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

check: all
	python3 src/refimpl.py
	python3 src/warped.py
	python3 src/verify_penalty.py

clean:
	rm -f $(BINS) *.bin

.PHONY: all check clean
