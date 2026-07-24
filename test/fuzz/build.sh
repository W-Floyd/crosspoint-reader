#!/usr/bin/env bash
# Build the dictionary libFuzzer target. Requires a clang with the fuzzer +
# address sanitizers (Apple clang or Homebrew LLVM both work).
#
#   ./test/fuzz/build.sh              # build only
#   ./test/fuzz/build.sh && \
#     DICTFUZZ_DIR=/tmp/dictfuzz ./test/fuzz/dictionary_fuzz -max_len=4096 corpus
#
# Notes:
# - -fno-exceptions / -fno-rtti match the firmware build so a repro reflects
#   on-device semantics (bare new/std::string OOM => abort, as on the C3).
# - The target writes a StarDict fileset under $DICTFUZZ_DIR each iteration;
#   point it at a tmpfs for speed if desired.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

# Pick a clang that ships the libFuzzer runtime. Apple's Command Line Tools
# clang does NOT (missing libclang_rt.fuzzer_osx.a), so prefer Homebrew LLVM.
if [ -z "${CXX:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang++ ]; then
    CXX=/opt/homebrew/opt/llvm/bin/clang++
  elif [ -x /usr/local/opt/llvm/bin/clang++ ]; then
    CXX=/usr/local/opt/llvm/bin/clang++
  else
    CXX=clang++  # Linux clang usually bundles the fuzzer runtime
  fi
fi

"$CXX" -std=gnu++2a -g -O1 -fno-exceptions -fno-rtti \
  -fsanitize=fuzzer,address -fno-omit-frame-pointer \
  -DXML_GE=0 -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1 \
  -I"$HERE/stubs" \
  -I"$ROOT/src/util" \
  -I"$ROOT/lib/Memory" \
  "$ROOT/src/util/Dictionary.cpp" \
  "$HERE/stubs.cpp" \
  "$HERE/dictionary_fuzz.cpp" \
  -o "$HERE/dictionary_fuzz"

echo "Built $HERE/dictionary_fuzz"
