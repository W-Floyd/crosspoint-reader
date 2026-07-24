#pragma once
// Root-aware POSIX HalStorage/HalFile for the dictionary soak, modelled on the
// device/simulator's SD abstraction: every firmware path ("/dictionaries/...",
// "/.crosspoint/...") is resolved under a single root dir (env SOAK_SD, default
// ./soak_sd). Writes create parent dirs. Implements exactly the surface
// Dictionary + DictZip use.
#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

typedef int oflag_t;
#ifndef O_RDONLY
#define O_RDONLY 0x00
#define O_WRITE 0x01
#define O_CREAT 0x02
#define O_TRUNC 0x04
#endif

namespace soakfs {
inline std::string root() {
  const char* r = std::getenv("SOAK_SD");
  return (r && *r) ? std::string(r) : std::string("./soak_sd");
}
inline std::string translate(const std::string& path) {
  if (path.empty()) return root();
  return root() + (path[0] == '/' ? "" : "/") + path;
}
inline void ensureParentDirs(const std::string& full) {
  // mkdir -p over each "/" boundary (skip the leaf filename).
  for (size_t i = 1; i < full.size(); ++i) {
    if (full[i] == '/') ::mkdir(full.substr(0, i).c_str(), 0777);
  }
}
}  // namespace soakfs

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
  int read(void* buf, size_t count) { return fp_ ? static_cast<int>(std::fread(buf, 1, count, fp_)) : -1; }
  int read() {
    if (!fp_) return -1;
    const int c = std::fgetc(fp_);
    return c == EOF ? -1 : c;
  }
  int write(const void* buf, size_t count) { return fp_ ? static_cast<int>(std::fwrite(buf, 1, count, fp_)) : -1; }
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
    std::FILE* f = std::fopen(soakfs::translate(path).c_str(), "rb");
    if (f) {
      std::fclose(f);
      return true;
    }
    return false;
  }

  bool openFileForRead(const char* /*mod*/, const std::string& path, HalFile& out) {
    std::FILE* f = std::fopen(soakfs::translate(path).c_str(), "rb");
    if (!f) return false;
    out = HalFile(f);
    return true;
  }
  bool openFileForRead(const char* m, const char* path, HalFile& out) { return openFileForRead(m, std::string(path), out); }

  bool openFileForWrite(const char* /*mod*/, const std::string& path, HalFile& out) {
    const std::string full = soakfs::translate(path);
    soakfs::ensureParentDirs(full);
    std::FILE* f = std::fopen(full.c_str(), "wb+");
    if (!f) return false;
    out = HalFile(f);
    return true;
  }
  bool openFileForWrite(const char* m, const char* path, HalFile& out) { return openFileForWrite(m, std::string(path), out); }

  bool remove(const char* path) { return std::remove(soakfs::translate(path).c_str()) == 0; }

  HalFile open(const char* path, oflag_t flag = O_RDONLY) {
    const std::string full = soakfs::translate(path);
    const char* mode = (flag & O_WRITE) ? "wb+" : "rb";
    if (flag & O_WRITE) soakfs::ensureParentDirs(full);
    return HalFile(std::fopen(full.c_str(), mode));
  }
};

#define Storage HalStorage::getInstance()
