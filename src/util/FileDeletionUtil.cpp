#include "FileDeletionUtil.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

namespace FileDeletionUtil {

void clearEpubCacheIfNeeded(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
    LOG_DBG("DEL", "Cleared epub cache for: %s", path.c_str());
  }
}

bool deletePath(const char* path) {
  if (!Storage.exists(path)) {
    LOG_ERR("DEL", "Path does not exist: %s", path);
    return false;
  }

  auto file = Storage.open(path);
  if (!file) {
    LOG_ERR("DEL", "Failed to open path: %s", path);
    return false;
  }

  if (file.isDirectory()) {
    file.rewindDirectory();
    char name[256];

    for (auto entry = file.openNextFile(); entry; entry = file.openNextFile()) {
      entry.getName(name, sizeof(name));

      String childPath = String(path);
      if (!childPath.endsWith("/")) {
        childPath += "/";
      }
      childPath += name;

      if (entry.isDirectory()) {
        if (!deletePath(childPath.c_str())) {
          entry.close();
          file.close();
          return false;
        }
      } else {
        clearEpubCacheIfNeeded(childPath.c_str());
        entry.close();
        if (!Storage.remove(childPath.c_str())) {
          file.close();
          return false;
        }
        continue;  // entry already closed
      }
      entry.close();
    }

    file.close();
    bool ok = Storage.rmdir(path);
    if (ok) {
      LOG_DBG("DEL", "Deleted directory: %s", path);
    } else {
      LOG_ERR("DEL", "Failed to delete directory: %s", path);
    }
    return ok;
  } else {
    file.close();
    clearEpubCacheIfNeeded(path);
    bool ok = Storage.remove(path);
    if (ok) {
      LOG_DBG("DEL", "Deleted file: %s", path);
    } else {
      LOG_ERR("DEL", "Failed to delete file: %s", path);
    }
    return ok;
  }
}

}  // namespace FileDeletionUtil
