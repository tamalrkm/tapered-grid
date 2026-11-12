#!/usr/bin/env bash
# Fetch and build the production hash-map baselines into ./deps, with the same
# compiler and flags as the benchmarks (see Makefile):
#   abseil-cpp (absl::flat_hash_map)  -- built from source with -O3 -march=native
#   Boost 1.86 headers (unordered_flat_map, concurrent_flat_map) -- header-only
# Both are optional: the benchmarks compile without them and simply lack those
# modes. Versions actually used are written to deps/VERSIONS.
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p deps
CXX=${CXX:-g++}
FLAGS="-O3 -march=native -ffp-contract=off"
: > deps/VERSIONS

if [ ! -f deps/absl/lib/pkgconfig/absl_flat_hash_map.pc ] && [ ! -f deps/absl/lib64/pkgconfig/absl_flat_hash_map.pc ]; then
  ok=0
  for tag in 20250512.1 20250127.1 20240722.0; do
    rm -rf deps/abseil-src
    if git clone --depth 1 --branch "$tag" https://github.com/abseil/abseil-cpp.git deps/abseil-src 2>/dev/null; then
      ok=1; echo "abseil $tag" >> deps/VERSIONS; break
    fi
  done
  if [ $ok = 1 ]; then
    cmake -S deps/abseil-src -B deps/abseil-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER="$(command -v $CXX)" -DCMAKE_CXX_FLAGS="$FLAGS" \
      -DCMAKE_CXX_STANDARD=17 -DABSL_PROPAGATE_CXX_STD=ON -DABSL_BUILD_TESTING=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$PWD/deps/absl" >/dev/null \
    && cmake --build deps/abseil-build -j"$(nproc)" >/dev/null \
    && cmake --install deps/abseil-build >/dev/null \
    && echo "abseil: installed to deps/absl" || echo "abseil: BUILD FAILED (absl modes will be missing)"
  else
    echo "abseil: clone failed (no network?) -- absl modes will be missing"
  fi
else
  echo "abseil: already in deps/absl"
fi

if [ ! -d deps/boost_1_86_0/boost ]; then
  url=https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
  if curl -fsSL "$url" -o deps/boost.tar.gz || wget -q "$url" -O deps/boost.tar.gz; then
    tar -xzf deps/boost.tar.gz -C deps boost_1_86_0/boost && rm -f deps/boost.tar.gz \
      && echo "boost 1.86.0 (headers)" >> deps/VERSIONS && echo "boost: headers in deps/boost_1_86_0"
  else
    echo "boost: download failed -- boost modes will be missing"
  fi
else
  echo "boost: already in deps/boost_1_86_0"
fi
cat deps/VERSIONS
