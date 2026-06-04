#include "ProgressSync.h"

#include <Logging.h>
#include <WiFi.h>

#include <cstring>

namespace {
std::vector<Provider*> g_providers;
}  // namespace

void ProgressSync::registerProvider(Provider* provider) {
  if (!provider) return;
  for (Provider* p : g_providers) {
    if (p == provider) return;
  }
  g_providers.push_back(provider);
}

bool ProgressSync::pushForPath(const std::string& epubPath, float progress) {
  if (epubPath.empty()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    LOG_DBG("RIL", "Skipping progress sync (no WiFi)");
    return false;
  }

  for (Provider* provider : g_providers) {
    if (!provider->isConfigured()) continue;
    const uint64_t articleId = provider->extractArticleId(epubPath);
    if (articleId != 0) {
      const auto r = provider->updateProgress(articleId, progress);
      if (!r.isOk()) {
        LOG_ERR("RIL", "Progress sync failed for %s: %s", provider->name(), Provider::errorString(r.code));
        return false;
      }
      LOG_DBG("RIL", "Synced progress via %s: id=%llu progress=%.3f", provider->name(),
              static_cast<unsigned long long>(articleId), progress);
      return true;
    }
  }
  return false;
}
