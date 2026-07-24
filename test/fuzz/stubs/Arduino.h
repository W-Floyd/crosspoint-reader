#pragma once
// Minimal host Arduino shim for the dictionary fuzz target — just the few
// symbols Dictionary.cpp touches. Not a general Arduino emulation.
#include <cstddef>
#include <cstdint>

inline unsigned long millis() { return 0; }

// Dictionary::readDefinition refuses a definition unless it fits comfortably in
// the largest free block. Report plenty so the real code path (bounds checks,
// read, resize) runs rather than early-returning.
struct ESPClass {
  size_t getMaxAllocHeap() const { return 16u * 1024 * 1024; }
  size_t getFreeHeap() const { return 16u * 1024 * 1024; }
};
inline ESPClass ESP;
