# Dictionary fuzz target

A [libFuzzer](https://llvm.org/docs/LibFuzzer.html) harness for the StarDict
dictionary lookup path (`src/util/Dictionary.cpp`) — built to chase memory bugs
in index parsing that don't reproduce reliably on-device.

It compiles the **real** `Dictionary` against a tiny host platform (own
`Arduino.h` / `Logging.h` / POSIX `HalStorage` in `stubs/`), so no ESP toolchain,
SDL, or the `.dz` decompressor is pulled in. `DictZip` and `DictionaryRegistry`
are stubbed (`stubs.cpp`) — the harness uses a plain `.dict`, so the decompressor
is never exercised.

## What it drives

Each iteration writes the fuzz input as both `fuzz.idx` and `fuzz.syn`, a fixed
64 KB `fuzz.dict`, then runs the full pipeline:

`open() → needsIndex() → buildIndex()` (the sampled-offset builder `buildSidecar`)
`→ lookup()` → `locate()` (binary search + ≤`SAMPLE_INTERVAL` linear scan),
`locateSynonym()` / `locateByOrdinal()` (synonym → headword), `readWordInto()`
(the 256-byte `wordBuf` boundary), and the stemmer fallback.

Prime suspects it targets: `readWordInto` / `wordBuf` overruns and the BE32
offset/ordinal math in `locate` / `locateByOrdinal` on malformed `.idx`/`.syn`/`.qidx`.

## Build

```bash
./test/fuzz/build.sh
```

Uses Homebrew LLVM clang automatically (Apple's Command Line Tools clang lacks
the libFuzzer runtime). On Linux, system clang works. `-fno-exceptions
-fno-rtti` match the firmware so a repro reflects on-device semantics.

## Run

```bash
mkdir -p test/fuzz/corpus
DICTFUZZ_DIR=/tmp/dictfuzz ASAN_OPTIONS=detect_leaks=0 \
  ./test/fuzz/dictionary_fuzz -max_len=8192 test/fuzz/corpus
```

- `DICTFUZZ_DIR` — where the throwaway StarDict fileset is written each
  iteration. Point it at a RAM disk to cut I/O (Linux: `/dev/shm/dictfuzz`).
- A crashing input is saved as `crash-<hash>`; replay it with
  `./test/fuzz/dictionary_fuzz crash-<hash>` to get the ASan backtrace.

**Seed the corpus** for much faster convergence: drop a real StarDict `.idx`
(e.g. from the GCIDE build) into `test/fuzz/corpus/` before running — the fuzzer
mutates from valid index structure instead of discovering it from scratch.

## Notes

- Throughput is bounded by the per-iteration file writes + sidecar rebuild
  (~1.5–2k exec/s locally). A real hunt wants minutes-to-hours, not seconds.
- The harness tracks whatever is in `Dictionary.cpp` today (incl. the `.syn`
  synonym path). If the index binary format changes, no harness edit is needed.
