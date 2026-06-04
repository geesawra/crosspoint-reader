#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "InstapaperArticle.h"

/**
 * Instapaper Full API client. All endpoints are POST over HTTPS with OAuth
 * 1.0a HMAC-SHA1 signatures. Tokens come from InstapaperTokenStore.
 *
 * Every call is blocking — drive them from an activity callback after
 * requestUpdateAndWait() has shown a progress screen, matching the pattern
 * used by KOReaderSyncClient.
 *
 * Pre-requisites: WiFi.status() == WL_CONNECTED and INSTAPAPER_CREDENTIALS
 * already loaded from SD.
 */
class InstapaperClient {
 public:
  enum Result : uint8_t {
    OK = 0,
    NO_TOKENS,
    AUTH_FAILED,
    FORBIDDEN,
    NOT_FOUND,
    NETWORK_FAILED,
    PARSE_FAILED,
    RATE_LIMITED,
    INSUFFICIENT_MEMORY,
    INTERNAL_ERROR,
  };

  struct ActionResult {
    Result result;
    std::string message;

    bool isOk() const { return result == OK; }
  };

  static const char* errorString(Result r);

  // List up to `limit` (1..500) bookmarks in `folder`. Output cleared on entry.
  static ActionResult listBookmarks(InstapaperFolder folder, int limit, std::vector<InstapaperArticle>& out);

  // Fetch the processed article body HTML, streaming directly to `outPath` on
  // SD. The file is created (or truncated) on entry and removed on any error.
  static ActionResult getText(uint64_t bookmarkId, const std::string& outPath);

  // Push read progress (0.0..1.0) back to Instapaper so other clients resume
  // at the right spot.
  static ActionResult updateReadProgress(uint64_t bookmarkId, float progress);

  static ActionResult star(uint64_t bookmarkId);
  static ActionResult unstar(uint64_t bookmarkId);
  static ActionResult archive(uint64_t bookmarkId);
  static ActionResult unarchive(uint64_t bookmarkId);
  static ActionResult deleteBookmark(uint64_t bookmarkId);
};
