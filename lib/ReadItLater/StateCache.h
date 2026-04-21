#pragma once
#include <ctime>
#include <vector>

#include "ReadItLaterArticle.h"

/**
 * SD-backed cache of per-folder article lists.
 *
 * Parameterized by a provider key so multiple providers can coexist
 * without colliding on cache file names.
 *
 * Storage: `/.crosspoint/<providerKey>/list_<folderId>.json`
 */
namespace StateCache {

// Write `articles` for `folderId` under `providerKey`. Returns false on SD/encode failure.
bool save(const char* providerKey, const char* folderId, const std::vector<ReadItLaterArticle>& articles);

// Load `articles` for `folderId` from SD. Returns false if cache missing or
// malformed. `outSyncedAt` (optional) receives the last sync timestamp.
bool load(const char* providerKey, const char* folderId, std::vector<ReadItLaterArticle>& articles,
          time_t* outSyncedAt = nullptr);

// Drop the cache for `folderId` (e.g. after a bulk remote change).
void invalidate(const char* providerKey, const char* folderId);

}  // namespace StateCache
