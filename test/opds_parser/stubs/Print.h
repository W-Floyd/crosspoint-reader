#pragma once

// Host-test stub for Arduino's Print base class. OpdsParser derives from Print
// so a streaming HTTP body can be fed straight into the XML parser; on the host
// the test drives write()/flush() directly, so only the minimal contract used by
// OpdsParser is provided here.

#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t) = 0;
  virtual void flush() {}
};
