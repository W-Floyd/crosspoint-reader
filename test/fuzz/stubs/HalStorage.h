#pragma once
// Minimal POSIX-backed HalStorage/HalFile for the dictionary fuzz target.
// Implements exactly the surface Dictionary.cpp (and the stubbed DictZip /
// DictionaryRegistry) use — file paths are taken verbatim (the fuzz harness
// hands out absolute temp paths), so there is no SD-root translation here.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

typedef int oflag_t;
#ifndef O_RDONLY
#define O_RDONLY 0x00
#define O_WRITE 0x01
#define O_CREAT 0x02
#define O_TRUNC 0x04
#endif

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::FILE* fp) : fp_(fp) {}
  HalFile(HalFile&& o) noexcept : fp_(o.fp_) { o.fp_ = nullptr; }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      closeInternal();
      fp_ = o.fp_;
      o.fp_ = nullptr;
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { closeInternal(); }

  explicit operator bool() const { return fp_ != nullptr; }

  bool seekSet(uint64_t pos) { return fp_ && std::fseek(fp_, static_cast<long>(pos), SEEK_SET) == 0; }
  int read(void* buf, size_t count) {
    if (!fp_) return -1;
    return static_cast<int>(std::fread(buf, 1, count, fp_));
  }
  int read() {
    if (!fp_) return -1;
    const int c = std::fgetc(fp_);
    return c == EOF ? -1 : c;
  }
  int write(const void* buf, size_t count) {
    if (!fp_) return -1;
    return static_cast<int>(std::fwrite(buf, 1, count, fp_));
  }
  uint32_t position() { return fp_ ? static_cast<uint32_t>(std::ftell(fp_)) : 0; }
  uint32_t fileSize() {
    if (!fp_) return 0;
    const long cur = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_END);
    const long sz = std::ftell(fp_);
    std::fseek(fp_, cur, SEEK_SET);
    return static_cast<uint32_t>(sz);
  }
  void close() { closeInternal(); }

 private:
  void closeInternal() {
    if (fp_) {
      std::fclose(fp_);
      fp_ = nullptr;
    }
  }
  std::FILE* fp_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage s;
    return s;
  }

  bool exists(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f) {
      std::fclose(f);
      return true;
    }
    return false;
  }

  bool openFileForRead(const char* /*mod*/, const std::string& path, HalFile& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out = HalFile(f);
    return true;
  }
  bool openFileForRead(const char* mod, const char* path, HalFile& out) {
    return openFileForRead(mod, std::string(path), out);
  }

  bool openFileForWrite(const char* /*mod*/, const std::string& path, HalFile& out) {
    std::FILE* f = std::fopen(path.c_str(), "wb+");
    if (!f) return false;
    out = HalFile(f);
    return true;
  }
  bool openFileForWrite(const char* mod, const char* path, HalFile& out) {
    return openFileForWrite(mod, std::string(path), out);
  }

  bool remove(const char* path) { return std::remove(path) == 0; }

  HalFile open(const char* path, oflag_t flag = O_RDONLY) {
    const char* mode = (flag & O_WRITE) ? "wb+" : "rb";
    return HalFile(std::fopen(path, mode));
  }
};

#define Storage HalStorage::getInstance()
