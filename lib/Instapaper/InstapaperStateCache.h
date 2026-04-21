#pragma once
#include <ctime>
#include <vector>

#include "InstapaperArticle.h"

/**
 * SD-backed cache of per-folder article lists.
 *
 * Purpose: the article list screen can show its previous contents instantly
 * on re-entry (and when offline) instead of staring at a "Fetching..." spinner
 * while the network call completes. The cache is refreshed after every
 * successful live fetch.
 *
 * Storage: `/.crosspoint/instapaper/list_<folder>.json`, ArduinoJson array
 * of article objects mirroring the fields in InstapaperArticle. Plus a
 * top-level `synced_at` unix timestamp.
 */
namespace InstapaperStateCache {

// Write `articles` for `folder` to SD. Returns false on SD/encode failure.
bool save(InstapaperFolder folder, const std::vector<InstapaperArticle>& articles);

// Load `articles` for `folder` from SD. Returns false if cache missing or
// malformed. `outSyncedAt` (optional) receives the last sync timestamp.
bool load(InstapaperFolder folder, std::vector<InstapaperArticle>& articles, time_t* outSyncedAt = nullptr);

// Drop the cache for `folder` (e.g. after a bulk remote change).
void invalidate(InstapaperFolder folder);

}  // namespace InstapaperStateCache
