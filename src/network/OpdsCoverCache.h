#pragma once

#include <string>

/**
 * On-SD cache for OPDS catalog cover thumbnails.
 *
 * Covers are fetched (JPEG) → decoded/downscaled (JPEGDEC) → stored as a
 * pre-rendered 1-bit Atkinson-dithered BMP at the row-thumbnail size, keyed by a
 * hash of the absolute cover URL, the target size, and a format tag. This mirrors
 * the home-screen cover thumb approach so the browser can blit a cached BMP
 * straight to the fast B/W framebuffer.
 *
 * Memory discipline (OOM is the dominant risk on this hardware):
 *  - Decodes happen one at a time; the caller must never run two ensure() calls
 *    concurrently (the JPEG decoder needs ~20 KB plus scaling buffers).
 *  - ensure() gates on free heap and returns Skipped (leaving no cache artifact)
 *    rather than risking an out-of-memory abort under pressure.
 *  - A decode that genuinely fails (undecodable cover) writes a small sentinel so
 *    the same cover is not re-fetched on every visit.
 */
class OpdsCoverCache {
 public:
  enum class Status {
    Ready,    // A decoded BMP is present at cachePath()
    NoCover,  // Known-undecodable (sentinel present); render a placeholder, do not retry
    Skipped,  // Transient miss (low heap, cancelled, or network failure); retry later
  };

  // Absolute path of the cached BMP for this cover URL + thumb size. Deterministic;
  // valid to call before ensure() (the file may not exist yet).
  static std::string cachePath(const std::string& absoluteUrl, int thumbW, int thumbH);

  // Cheap check: is a decoded BMP already cached? (stat only, no decode/network)
  static bool isCached(const std::string& absoluteUrl, int thumbW, int thumbH);

  // Ensure a cover BMP exists in the cache. Cache hits are cheap. On a miss this
  // downloads and decodes (blocking) — call it off the render path. cancelFlag, if
  // provided, aborts an in-flight download.
  static Status ensure(const std::string& absoluteUrl, int thumbW, int thumbH, const std::string& username,
                       const std::string& password, bool* cancelFlag = nullptr);

  // Evict cached covers (oldest first, best effort) until the cache directory is
  // under maxBytes. Bounds SD usage across sessions.
  static void enforceBudget(size_t maxBytes = kDefaultBudgetBytes);

  static constexpr size_t kDefaultBudgetBytes = 1024 * 1024;  // 1 MB

 private:
  static std::string keyHex(const std::string& absoluteUrl, int thumbW, int thumbH);
};
