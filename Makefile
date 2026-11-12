# One set of flags for every binary and for the abseil build (scripts/get_deps.sh).
# -ffp-contract=off and no -ffast-math: payloads are bit-reproducible across
# call sites (see src/pl.hpp, floating-point policy).
CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -ffp-contract=off
CXXFLAGS += -std=c++17 -pthread -Wall
BUILD     = build

ABSL_PC   = $(firstword $(wildcard deps/absl/lib/pkgconfig) $(wildcard deps/absl/lib64/pkgconfig))
ifneq ($(ABSL_PC),)
  ABSL_CFLAGS = $(shell PKG_CONFIG_PATH=$(ABSL_PC) pkg-config --cflags absl_flat_hash_map absl_hash)
  ABSL_LIBS   = $(shell PKG_CONFIG_PATH=$(ABSL_PC) pkg-config --libs absl_flat_hash_map absl_hash) \
                -Wl,-rpath,$(abspath deps/absl/lib) -Wl,-rpath,$(abspath deps/absl/lib64)
endif
ifneq ($(wildcard deps/boost_1_86_0/boost),)
  BOOST_CFLAGS = -isystem deps/boost_1_86_0
endif
# -DNDEBUG for the benchmarks, as in the abseil build: without it absl's find runs
# debug checks on every lookup. The tests keep their assertions.
DEPFLAGS = $(ABSL_CFLAGS) $(BOOST_CFLAGS) -DNDEBUG

BINS = bench build_parallel ooc_bench pl_bench pl_fit entmax_bench test_tg
all: $(addprefix $(BUILD)/,$(BINS))

$(BUILD)/%: src/%.cpp src/tg.hpp src/common.hpp src/pl.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -o $@ $< $(ABSL_LIBS)
$(BUILD)/test_tg: tests/test_tg.cpp src/tg.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<
$(BUILD)/listing1: tests/listing1.cpp src/tg.hpp src/pl.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $<
$(BUILD):
	mkdir -p $(BUILD)

# Correctness harness: C++ unit tests + the Python reference checks. About 2 min.
PY ?= python3
verify: $(BUILD)/test_tg $(BUILD)/listing1
	$(BUILD)/test_tg
	cd $(BUILD) && ./listing1
	cd src && $(PY) refimpl.py
	cd src && $(PY) warped.py > ../$(BUILD)/warped.log; cat ../$(BUILD)/warped.log; grep -q "ALL PASS" ../$(BUILD)/warped.log
	cd src && $(PY) verify_penalty.py > ../$(BUILD)/penalty.log && echo "[ok] penalty formula check ran"

# Which optional baselines were compiled in.
modes: all
	@for m in absl boost; do $(BUILD)/bench $$m 4 3 10 1 >/dev/null 2>&1 && echo "bench: $$m available" || echo "bench: $$m MISSING"; done

# Regenerate every number, table body and figure from the raw logs and compare
# with the copies committed under paper/. Needs matplotlib for the figures.
PAPER ?= paper
numbers:
	$(PY) analysis/make_numbers.py --results . --paper $(PAPER)
figures:
	$(PY) analysis/make_figs.py --results . --out $(PAPER)/figs
check-numbers:
	@t=$$(mktemp -d) && mkdir $$t/tables && cp $(PAPER)/main.tex $$t/ && $(PY) analysis/make_numbers.py --results . --paper $$t > /dev/null && \
	diff -r $$t/tables $(PAPER)/tables && diff $$t/numbers.tex $(PAPER)/numbers.tex && echo "numbers and tables match the raw logs"; rm -rf $$t

clean:
	rm -rf $(BUILD)

.PHONY: all verify modes numbers figures check-numbers clean
