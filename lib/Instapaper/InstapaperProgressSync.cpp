#include "InstapaperProgressSync.h"

#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "InstapaperClient.h"
#include "InstapaperTokenStore.h"

namespace {
constexpr char PATH_PREFIX[] = "/.crosspoint/instapaper/article_";
constexpr char PATH_SUFFIX[] = ".epub";
}  // namespace

uint64_t InstapaperProgressSync::extractBookmarkId(const std::string& filepath) {
  const size_t prefixLen = std::strlen(PATH_PREFIX);
  const size_t suffixLen = std::strlen(PATH_SUFFIX);
  if (filepath.size() <= prefixLen + suffixLen) return 0;
  if (filepath.compare(0, prefixLen, PATH_PREFIX) != 0) return 0;
  if (filepath.compare(filepath.size() - suffixLen, suffixLen, PATH_SUFFIX) != 0) return 0;

  const std::string idStr = filepath.substr(prefixLen, filepath.size() - prefixLen - suffixLen);
  uint64_t id = 0;
  for (char c : idStr) {
    if (c < '0' || c > '9') return 0;
    id = id * 10 + static_cast<uint64_t>(c - '0');
  }
  return id;
}

bool InstapaperProgressSync::pushProgress(uint64_t bookmarkId, float progress) {
  if (bookmarkId == 0) return false;
  if (WiFi.status() != WL_CONNECTED) {
    LOG_DBG("INSTA", "Skipping progress sync (no WiFi)");
    return false;
  }
  if (!INSTAPAPER_TOKENS.hasTokens()) {
    // Attempt lazy load — the reader may be launched before the folder screen.
    if (!INSTAPAPER_TOKENS.loadFromFile()) {
      LOG_DBG("INSTA", "Skipping progress sync (no tokens)");
      return false;
    }
  }

  const auto r = InstapaperClient::updateReadProgress(bookmarkId, progress);
  if (r != InstapaperClient::OK) {
    LOG_ERR("INSTA", "Progress sync failed: %s", InstapaperClient::errorString(r));
    return false;
  }
  LOG_DBG("INSTA", "Synced progress: id=%llu progress=%.3f", static_cast<unsigned long long>(bookmarkId), progress);
  return true;
}
