#pragma once
#include <cstdint>
#include <string>

/**
 * Bridge between EpubReaderActivity and the Instapaper API for syncing
 * read-progress back to the user's Instapaper account. Safe to call for any
 * EPUB file — no-ops unless the filepath matches the synthesized Instapaper
 * path pattern (/.crosspoint/instapaper/article_<id>.epub).
 *
 * The sync is best-effort: it requires a live WiFi connection (caller is
 * expected not to force WiFi up), valid tokens, and a synced clock. Failures
 * are logged and swallowed.
 */
namespace InstapaperProgressSync {

// Returns the Instapaper bookmark id if `filepath` names a synthesized
// Instapaper article EPUB, 0 otherwise.
uint64_t extractBookmarkId(const std::string& filepath);

// Push `progress` (0.0..1.0) to Instapaper for `bookmarkId`. Requires
// WiFi.status() == WL_CONNECTED and tokens loaded. Returns true on OK.
// Non-blocking-safe from a task context: blocking network call.
bool pushProgress(uint64_t bookmarkId, float progress);

}  // namespace InstapaperProgressSync
