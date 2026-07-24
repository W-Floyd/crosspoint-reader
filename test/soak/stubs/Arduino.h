#pragma once
// Minimal host Arduino shim for the dictionary soak — just the symbols
// Dictionary.cpp touches. Not a general Arduino emulation.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

inline unsigned long millis() { return 0; }

// Dictionary::readDefinition refuses a definition unless getMaxAllocHeap() >=
// size + 8KB. Default to plenty (host), but let the soak model the ~380KB C3's
// tight, mid-reading free heap via SOAK_MAX_ALLOC_HEAP (bytes) — set it to e.g.
// 51200 to reproduce large-definition allocation refusals on device-like heap.
struct ESPClass {
  size_t getMaxAllocHeap() const {
    const char* cap = std::getenv("SOAK_MAX_ALLOC_HEAP");
    return (cap && *cap) ? static_cast<size_t>(std::strtoull(cap, nullptr, 10)) : 16u * 1024 * 1024;
  }
  size_t getFreeHeap() const { return getMaxAllocHeap(); }
};
inline ESPClass ESP;
