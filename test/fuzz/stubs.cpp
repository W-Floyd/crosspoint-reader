// Stub definitions so the fuzz target links without dragging in the .dz
// decompressor (InflateReader + miniz/uzlib) or the SD directory scanner.
//
// The harness uses a plain .dict (hasPlainDict == true), so DictZip::extractEntry
// is never called — it only needs to link. resolveBasePath returns a fixed temp
// base path the harness writes its StarDict fileset to.
#include <cstdlib>
#include <string>

#include <DictZip.h>
#include <DictionaryRegistry.h>

namespace DictZip {
bool extractEntry(const char* /*path*/, uint32_t /*offset*/, uint32_t /*size*/, HalFile& /*outFile*/) {
  return false;
}
}  // namespace DictZip

namespace DictionaryRegistry {
bool resolveBasePath(const char* /*folderName*/, std::string& basePathOut) {
  const char* dir = std::getenv("DICTFUZZ_DIR");
  basePathOut = std::string(dir ? dir : "/tmp/dictfuzz") + "/fuzz";
  return true;
}
}  // namespace DictionaryRegistry
