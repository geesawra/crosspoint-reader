#pragma once
#include <string>
#include <vector>

#include "Provider.h"

/**
 * Bridge between EpubReaderActivity and Read-it-Later providers for syncing
 * read-progress back to the user's account. Safe to call for any EPUB file —
 * no-ops unless a registered provider recognizes the filepath as one of its
 * own synthesized EPUBs.
 *
 * The sync is best-effort: it requires a live WiFi connection (caller is
 * expected not to force WiFi up), valid credentials, and a synced clock.
 * Failures are logged and swallowed.
 */
namespace ProgressSync {

// Register a provider so pushForPath() can delegate to it.
void registerProvider(Provider* provider);

// Push `progress` (0.0..1.0) to whichever provider owns `epubPath`.
// Returns true if a provider claimed the path and the sync succeeded.
bool pushForPath(const std::string& epubPath, float progress);

}  // namespace ProgressSync
