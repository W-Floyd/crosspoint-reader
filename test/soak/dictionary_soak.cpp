// Dictionary soak test: hammer the real StarDict lookup path a very large
// number of times and watch for degradation — the "use it a lot and it
// eventually stops working" failure mode (file-handle leak, temp-file wear,
// heap accumulation, or a lookup that used to hit starting to miss).
//
// Uses the REAL Dictionary + DictZip + InflateReader + uzlib against a real
// dictionary under $SOAK_SD/dictionaries/<folder> (default folder: gcide).
// Mirrors device usage: a fresh Dictionary per "session" (see --reopen-every),
// open() -> needsIndex() -> buildIndex()-once -> many lookup()s.
//
// Monitors, every report interval:
//   - open file descriptors (an fd leak is the classic embedded "stops working")
//   - peak RSS (unbounded growth => heap leak/accumulation)
//   - a control word that MUST keep resolving (functional regression)
// Exits non-zero the moment the control word stops resolving or fds run away.
//
// Build: test/soak/build.sh   Run: SOAK_SD=... ./test/soak/dictionary_soak gcide
#include <dirent.h>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <Dictionary.h>

#include "stubs/HalStorage.h"  // soakfs::root()

namespace {

int countOpenFds() {
  DIR* d = opendir("/dev/fd");
  if (!d) d = opendir("/proc/self/fd");
  if (!d) return -1;
  int n = 0;
  while (readdir(d)) n++;
  closedir(d);
  return n;  // includes ., .. and the dir handle — only the delta matters
}

// CURRENT resident set (not the monotonic ru_maxrss high-water mark), so a
// plateau means no runaway leak and a steady climb means one.
long currentRssKb() {
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
    return static_cast<long>(info.resident_size / 1024);
  return -1;
#else
  long rssPages = 0;
  if (FILE* f = std::fopen("/proc/self/statm", "r")) {
    long total = 0;
    if (std::fscanf(f, "%ld %ld", &total, &rssPages) != 2) rssPages = 0;
    std::fclose(f);
  }
  return rssPages * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

std::string findIdx(const std::string& folder) {
  const std::string dir = soakfs::root() + "/dictionaries/" + folder;
  DIR* d = opendir(dir.c_str());
  if (!d) return "";
  std::string found;
  for (dirent* e; (e = readdir(d));) {
    const std::string name = e->d_name;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".idx") == 0) {
      found = dir + "/" + name;
      break;
    }
  }
  closedir(d);
  return found;
}

// Parse the .idx headwords (word NUL + 4-byte offset + 4-byte size). Keep every
// `stride`-th word so a huge dictionary still gives a broad, bounded sample.
std::vector<std::string> loadHeadwords(const std::string& idxPath, int stride) {
  std::vector<std::string> words;
  FILE* f = std::fopen(idxPath.c_str(), "rb");
  if (!f) return words;
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { std::fclose(f); return words; }
  std::fclose(f);

  size_t i = 0, n = 0;
  while (i < buf.size()) {
    const size_t start = i;
    while (i < buf.size() && buf[i] != 0) i++;
    if (i >= buf.size()) break;
    if ((n++ % stride) == 0) words.emplace_back(reinterpret_cast<const char*>(&buf[start]), i - start);
    i += 1 + 8;  // NUL + BE32 offset + BE32 size
  }
  return words;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string folder = argc > 1 ? argv[1] : "gcide";
  const long iters = std::getenv("SOAK_ITERS") ? std::atol(std::getenv("SOAK_ITERS")) : 200000;
  const int reopenEvery = std::getenv("SOAK_REOPEN") ? std::atoi(std::getenv("SOAK_REOPEN")) : 8;
  const long report = std::getenv("SOAK_REPORT") ? std::atol(std::getenv("SOAK_REPORT")) : 5000;

  const std::string idx = findIdx(folder);
  if (idx.empty()) {
    std::fprintf(stderr, "No .idx under %s/dictionaries/%s — set SOAK_SD and place the dict there.\n",
                 soakfs::root().c_str(), folder.c_str());
    return 2;
  }
  std::vector<std::string> words = loadHeadwords(idx, /*stride=*/17);
  if (words.empty()) {
    std::fprintf(stderr, "Parsed 0 headwords from %s\n", idx.c_str());
    return 2;
  }
  const std::string control = words.front();  // a guaranteed direct hit
  std::printf("Soak: folder=%s idx=%s sample=%zu words, iters=%ld reopen-every=%d\n",
              folder.c_str(), idx.c_str(), words.size(), iters, reopenEvery);

  // Some deliberate variety: real hits, near-misses, and stem/case variants.
  auto target = [&](long i) -> std::string {
    switch (i % 8) {
      case 3: return words[i % words.size()] + "zzq";     // near-miss
      case 5: return words[i % words.size()] + "s";        // stem variant
      case 6: { std::string w = words[i % words.size()]; for (char& c : w) c = std::toupper((unsigned char)c); return w; }  // case
      default: return words[i % words.size()];             // direct hit
    }
  };

  Dictionary dict;
  bool open = false;
  bool builtOnce = false;
  int baselineFds = -1;
  long hits = 0, misses = 0;

  for (long i = 0; i < iters; ++i) {
    if (!open || (reopenEvery > 0 && i % reopenEvery == 0)) {
      dict = Dictionary();  // fresh instance, mirrors a new activity
      if (!dict.open(folder.c_str())) {
        std::fprintf(stderr, "FAIL: dict.open() failed at iter %ld\n", i);
        return 1;
      }
      if (dict.needsIndex()) {
        if (!builtOnce) { std::printf("Indexing (first open)...\n"); dict.buildIndex(); builtOnce = true; }
        else { std::fprintf(stderr, "WARN: needsIndex() true again at iter %ld (sidecar churn?)\n", i); dict.buildIndex(); }
      }
      open = true;
    }

    std::string def, head;
    if (dict.lookup(target(i).c_str(), def, head)) hits++; else misses++;

    if (i > 0 && i % report == 0) {
      // Control invariant: a fresh Dictionary must still resolve the control word.
      Dictionary probe;
      std::string pdef, phead;
      const bool ctlOk = probe.open(folder.c_str()) && probe.lookup(control.c_str(), pdef, phead);
      const int fds = countOpenFds();
      if (baselineFds < 0) baselineFds = fds;
      std::printf("iter=%-9ld hits=%-8ld miss=%-8ld fds=%-4d (base %d) rssKB=%-8ld control['%s']=%s\n",
                  i, hits, misses, fds, baselineFds, currentRssKb(), control.c_str(), ctlOk ? "OK" : "*** LOST ***");
      std::fflush(stdout);
      if (!ctlOk) {
        std::fprintf(stderr, "FAIL: control word '%s' stopped resolving at iter %ld — dictionary broke.\n",
                     control.c_str(), i);
        return 1;
      }
      if (baselineFds >= 0 && fds > baselineFds + 24) {
        std::fprintf(stderr, "FAIL: file-descriptor leak — fds %d exceed baseline %d at iter %ld.\n",
                     fds, baselineFds, i);
        return 1;
      }
    }
  }

  std::printf("Done: %ld iters, hits=%ld miss=%ld, final fds=%d, rssKB=%ld — no failure detected.\n",
              iters, hits, misses, countOpenFds(), currentRssKb());
  return 0;
}
