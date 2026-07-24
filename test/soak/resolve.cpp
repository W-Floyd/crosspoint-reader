// Host implementation of DictionaryRegistry::resolveBasePath for the soak:
// scan $SOAK_SD/dictionaries/<folder> for a *.idx and return the device-style
// base path "/dictionaries/<folder>/<stem>" (HalStorage re-roots it). Avoids
// pulling the firmware's SD directory-enumeration into the host build.
#include <filesystem>
#include <string>

#include <DictionaryRegistry.h>

#include "stubs/HalStorage.h"  // soakfs::root()

namespace fs = std::filesystem;

namespace DictionaryRegistry {
bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || !*folderName) return false;
  const std::string dir = soakfs::root() + "/dictionaries/" + folderName;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (entry.path().extension() == ".idx") {
      const std::string stem = entry.path().stem().string();  // e.g. "stardict"
      basePathOut = std::string("/dictionaries/") + folderName + "/" + stem;
      return true;
    }
  }
  return false;
}
}  // namespace DictionaryRegistry
