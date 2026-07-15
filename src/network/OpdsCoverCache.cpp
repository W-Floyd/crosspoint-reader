#include "OpdsCoverCache.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <functional>
#include <vector>

#include "HttpDownloader.h"

namespace {
constexpr char COVER_DIR[] = "/.crosspoint/opds_covers";
constexpr char MODULE[] = "OPDSCVR";

// Free-heap floor for attempting a decode. JpegToBmpConverter has its own hard
// floor of ~52 KB (20 KB decoder + 32 KB working); we gate just above it so we
// only skip (Skipped, no cache artifact) when the converter would refuse anyway.
// Gating much higher needlessly blocks covers on the OPDS browser, where WiFi
// keeps free heap only a few KB above that floor.
constexpr uint32_t COVER_DECODE_MIN_HEAP = 54 * 1024;

constexpr size_t NAME_BUFFER_SIZE = 64;

std::string coverDir() { return COVER_DIR; }

std::string bmpPath(const std::string& hex) { return std::string(COVER_DIR) + "/" + hex + ".bmp"; }

std::string sentinelPath(const std::string& hex) { return std::string(COVER_DIR) + "/" + hex + ".none"; }

std::string tmpPath() { return std::string(COVER_DIR) + "/dl.tmp"; }

// Create a zero-length sentinel marking a cover as known-undecodable.
void writeSentinel(const std::string& path) {
  HalFile f;
  if (Storage.openFileForWrite(MODULE, path, f)) {
    // Opened with O_TRUNC; closing an empty file is enough. Destructor closes it.
  }
}
}  // namespace

std::string OpdsCoverCache::keyHex(const std::string& absoluteUrl, const int thumbW, const int thumbH) {
  // Key on the exact fetched URL (query string intact) plus the render size and a
  // format tag, so a resize, a changed cover URL (e.g. new timestamp/preset), or a
  // change to the decode format all miss cleanly. Bump the tag if the cached BMP
  // format changes (currently v2 = 1-bit Atkinson dither).
  char sizeSuffix[32];
  snprintf(sizeSuffix, sizeof(sizeSuffix), "|%dx%d|v2", thumbW, thumbH);
  const size_t h = std::hash<std::string>{}(absoluteUrl + sizeSuffix);
  char hex[2 * sizeof(size_t) + 1];
  snprintf(hex, sizeof(hex), "%zx", h);
  return hex;
}

std::string OpdsCoverCache::cachePath(const std::string& absoluteUrl, const int thumbW, const int thumbH) {
  return bmpPath(keyHex(absoluteUrl, thumbW, thumbH));
}

bool OpdsCoverCache::isCached(const std::string& absoluteUrl, const int thumbW, const int thumbH) {
  return Storage.exists(cachePath(absoluteUrl, thumbW, thumbH).c_str());
}

OpdsCoverCache::Status OpdsCoverCache::ensure(const std::string& absoluteUrl, const int thumbW, const int thumbH,
                                              const std::string& username, const std::string& password,
                                              bool* cancelFlag) {
  if (absoluteUrl.empty() || thumbW <= 0 || thumbH <= 0) return Status::Skipped;

  const std::string hex = keyHex(absoluteUrl, thumbW, thumbH);
  const std::string bmp = bmpPath(hex);
  const std::string none = sentinelPath(hex);

  if (Storage.exists(bmp.c_str())) return Status::Ready;
  if (Storage.exists(none.c_str())) return Status::NoCover;
  if (cancelFlag && *cancelFlag) return Status::Skipped;

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < COVER_DECODE_MIN_HEAP) {
    LOG_DBG(MODULE, "Skipping cover decode, low heap: %u < %u", freeHeap, COVER_DECODE_MIN_HEAP);
    return Status::Skipped;
  }

  Storage.mkdir(COVER_DIR);
  const std::string tmp = tmpPath();

  const auto dl = HttpDownloader::downloadToFile(absoluteUrl, tmp, nullptr, cancelFlag, username, password);
  if (dl != HttpDownloader::OK) {
    LOG_DBG(MODULE, "Cover download failed (%d): %s", static_cast<int>(dl), absoluteUrl.c_str());
    Storage.remove(tmp.c_str());
    return Status::Skipped;  // transient (network/cancel) — allow a later retry
  }

  bool decoded = false;
  {
    HalFile jpeg;
    HalFile out;
    if (Storage.openFileForRead(MODULE, tmp, jpeg) && Storage.openFileForWrite(MODULE, bmp, out)) {
      // 1-bit Atkinson-dithered BMP (as the home-screen thumbnails use): the
      // browser blits it to the fast B/W framebuffer, so a proper 1-bit dither
      // looks cleaner than a 2-bit image crushed to B/W at draw time.
      decoded = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpeg, out, thumbW, thumbH);
      out.flush();
    }
    // jpeg/out close here (scope exit) before we remove/rewrite the files below.
  }

  Storage.remove(tmp.c_str());

  if (decoded) return Status::Ready;

  // Decode failed. Drop the partial BMP. Only record a permanent sentinel if heap
  // is still healthy — otherwise the failure was likely memory pressure, so leave
  // the cover un-marked and retry it another time.
  Storage.remove(bmp.c_str());
  if (ESP.getFreeHeap() >= COVER_DECODE_MIN_HEAP) {
    LOG_DBG(MODULE, "Cover undecodable, writing sentinel: %s", absoluteUrl.c_str());
    writeSentinel(none);
    return Status::NoCover;
  }
  LOG_DBG(MODULE, "Cover decode failed under heap pressure, will retry: %s", absoluteUrl.c_str());
  return Status::Skipped;
}

void OpdsCoverCache::enforceBudget(const size_t maxBytes) {
  HalFile dir = Storage.open(coverDir().c_str(), O_RDONLY);
  if (!dir || !dir.isDirectory()) return;

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR(MODULE, "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    return;
  }

  // Snapshot names + sizes in one pass (directory-iteration order ~ oldest-first on
  // FAT). Deleting is done afterwards so we never mutate the dir mid-iteration.
  struct Entry {
    std::string name;
    size_t size;
  };
  std::vector<Entry> files;
  size_t total = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    const size_t sz = file.size();
    total += sz;
    file.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    files.push_back({nameBuffer.get(), sz});
  }
  dir.close();

  if (total <= maxBytes) return;

  // Best-effort LRU (HalFile exposes no mtime): evict from the front until under budget.
  for (const auto& f : files) {
    if (total <= maxBytes) break;
    const std::string path = coverDir() + "/" + f.name;
    if (Storage.remove(path.c_str())) {
      total -= f.size;
      LOG_DBG(MODULE, "Evicted cover cache entry: %s", path.c_str());
    }
  }
}
