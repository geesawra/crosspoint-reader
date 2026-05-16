#include "StateCache.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace {
std::string cacheDirFor(const char* providerKey) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/.crosspoint/%s", providerKey);
  return std::string(buf);
}

std::string pathFor(const char* providerKey, const char* folderId) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/.crosspoint/%s/list_%s.json", providerKey, folderId);
  return std::string(buf);
}
}  // namespace

bool StateCache::save(const char* providerKey, const char* folderId, const std::vector<ReadItLaterArticle>& articles) {
  Storage.mkdir(cacheDirFor(providerKey).c_str());

  JsonDocument doc;
  doc["synced_at"] = static_cast<int64_t>(::time(nullptr));
  JsonArray arr = doc["articles"].to<JsonArray>();
  for (const ReadItLaterArticle& a : articles) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = a.id;
    o["title"] = a.title;
    o["domain"] = a.domain;
    o["author"] = a.author;
    o["word_count"] = a.word_count;
    o["progress_pct"] = a.progress_pct;
    o["starred"] = a.starred;
    o["saved_at"] = static_cast<int64_t>(a.saved_at);
  }

  std::string serialized;
  serializeJson(doc, serialized);

  const std::string path = pathFor(providerKey, folderId);
  FsFile f;
  if (!Storage.openFileForWrite("RIL", path.c_str(), f)) {
    LOG_ERR("RIL", "Cache save: cannot open %s", path.c_str());
    return false;
  }
  const size_t n = f.write(serialized.data(), serialized.size());
  f.close();
  if (n != serialized.size()) {
    LOG_ERR("RIL", "Cache save: short write %zu/%zu", n, serialized.size());
    return false;
  }
  return true;
}

bool StateCache::load(const char* providerKey, const char* folderId, std::vector<ReadItLaterArticle>& articles,
                      time_t* outSyncedAt) {
  articles.clear();
  const std::string path = pathFor(providerKey, folderId);
  if (!Storage.exists(path.c_str())) return false;

  const String body = Storage.readFile(path.c_str());
  if (body.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body.c_str())) {
    LOG_ERR("RIL", "Cache load: JSON parse failed for %s", path.c_str());
    return false;
  }

  if (outSyncedAt) {
    *outSyncedAt = static_cast<time_t>(doc["synced_at"] | 0);
  }

  JsonArrayConst arr = doc["articles"].as<JsonArrayConst>();
  if (arr.isNull()) return false;

  articles.reserve(arr.size());
  for (JsonVariantConst item : arr) {
    ReadItLaterArticle a;
    a.id = item["id"].as<uint64_t>();
    const char* title = item["title"] | "";
    const char* domain = item["domain"] | "";
    const char* author = item["author"] | "";
    std::strncpy(a.title, title, sizeof(a.title) - 1);
    a.title[sizeof(a.title) - 1] = '\0';
    std::strncpy(a.domain, domain, sizeof(a.domain) - 1);
    a.domain[sizeof(a.domain) - 1] = '\0';
    std::strncpy(a.author, author, sizeof(a.author) - 1);
    a.author[sizeof(a.author) - 1] = '\0';
    a.word_count = item["word_count"] | 0;
    a.progress_pct = item["progress_pct"] | 0;
    a.starred = item["starred"] | false;
    a.saved_at = static_cast<time_t>(item["saved_at"] | 0);
    articles.push_back(a);
  }
  return true;
}

void StateCache::invalidate(const char* providerKey, const char* folderId) {
  const std::string path = pathFor(providerKey, folderId);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
}
