// libFuzzer harness for the StarDict dictionary lookup path.
//
// Builds a StarDict fileset from the fuzz input and runs the full open ->
// needsIndex -> buildIndex -> lookup pipeline, so the sampled-offset index
// builder (buildSidecar), the binary search + linear scan (locate), the synonym
// path (locateSynonym / locateByOrdinal), and readWordInto's 256-byte wordBuf
// boundary are all driven by attacker-controlled bytes. Build with
// -fsanitize=fuzzer,address (see build.sh); ASan pinpoints the overflow/UAF.
//
// The .idx and .syn get the raw fuzz bytes; the .dict is fixed filler so
// readDefinition has data to read after a "hit".
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <Dictionary.h>

namespace {
std::string g_dir;

void writeFile(const std::string& path, const void* data, size_t n) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  if (n) std::fwrite(data, 1, n, f);
  std::fclose(f);
}
}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
  const char* dir = std::getenv("DICTFUZZ_DIR");
  g_dir = dir ? dir : "/tmp/dictfuzz";
  std::string mk = "mkdir -p '" + g_dir + "'";
  if (std::system(mk.c_str()) != 0) { /* best effort */ }

  // Fixed 64 KB .dict so a resolved definition offset/size has real bytes to
  // read (exercises readDefinition rather than early-returning).
  static std::string dict(64u * 1024, 'x');
  writeFile(g_dir + "/fuzz.dict", dict.data(), dict.size());
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Feed the fuzz bytes as both the .idx (word index) and .syn (synonym index).
  writeFile(g_dir + "/fuzz.idx", data, size);
  writeFile(g_dir + "/fuzz.syn", data, size);
  // Drop stale sidecars so buildIndex rescans the fresh inputs each run.
  std::remove((g_dir + "/fuzz.qidx").c_str());
  std::remove((g_dir + "/fuzz.sidx").c_str());

  Dictionary dict;
  if (!dict.open("fuzz")) return 0;
  dict.needsIndex();
  dict.buildIndex();

  std::string def, head;
  dict.lookup("a", def, head);
  dict.lookup("the", def, head);
  dict.lookup("zzzzzzzz", def, head);

  // Stress readWordInto's fixed 256-byte wordBuf with an over-long target.
  const std::string longWord(400, 'q');
  dict.lookup(longWord.c_str(), def, head);

  // Look up a word taken from the input itself, to hit exact-match branches
  // against whatever headwords the fuzzer discovers in the .idx/.syn.
  if (size) {
    std::string w(reinterpret_cast<const char*>(data), size < 64 ? size : 64);
    for (char& c : w) {
      if (c == '\0') c = '.';  // keep it a valid C string
    }
    dict.lookup(w.c_str(), def, head);
  }
  return 0;
}
