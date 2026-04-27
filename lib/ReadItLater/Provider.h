#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "ReadItLaterArticle.h"

/**
 * Pure virtual interface for a Read-it-Later service provider.
 *
 * Each provider (Instapaper, Pocket, Wallabag, etc.) implements this
 * interface. The UI activities are fully generic and operate only through
 * this interface — they never touch provider-specific types.
 */
class Provider {
 public:
  virtual ~Provider() = default;

  // ---------- Identity ----------

  /** Human-readable service name, e.g. "Instapaper". */
  virtual const char* name() const = 0;

  /** Machine key for cache directories, e.g. "instapaper". */
  virtual const char* cacheDirName() const = 0;

  // ---------- Credentials ----------

  /**
   * Load provider-specific credentials from the SD card.
   * Each provider knows its own file format and location under
   * /.crosspoint/ (e.g. instapaper.txt, pocket.json, etc.).
   */
  virtual bool loadCredentials() = 0;

  /** Returns true if credentials were loaded and appear valid. */
  virtual bool isConfigured() const = 0;

  // ---------- Result / errors ----------

  enum class Result : uint8_t {
    OK = 0,
    NO_TOKENS,
    AUTH_FAILED,
    NETWORK_FAILED,
    PARSE_FAILED,
    RATE_LIMITED,
    SERVER_ERROR,
  };

  static const char* errorString(Result r);

  // ---------- Folders ----------

  struct FolderInfo {
    const char* id;     // machine key, e.g. "unread"
    const char* label;  // human label (already translated, or raw string)
  };

  /** Populate `out` with the folders available for this provider. */
  virtual Result listFolders(std::vector<FolderInfo>& out) = 0;

  // ---------- Articles ----------

  /** List articles in `folder`. `limit` is advisory (1..500). */
  virtual Result listArticles(const FolderInfo& folder, int limit,
                              std::vector<ReadItLaterArticle>& out) = 0;

  /** Fetch the processed article body HTML, streaming to `outPath` on SD. */
  virtual Result fetchText(uint64_t articleId, const std::string& outPath) = 0;

  // ---------- Actions ----------

  enum class Action : uint8_t {
    Star,
    Unstar,
    Archive,
    Unarchive,
    Delete,
  };

  /**
   * Return the actions available for `article` when viewed inside `folder`.
   * The generic ActionsActivity renders these as a menu.
   */
  virtual std::vector<Action> availableActions(const FolderInfo& folder,
                                               const ReadItLaterArticle& article) const = 0;

  /** Perform `action` on `articleId`. */
  virtual Result performAction(uint64_t articleId, Action action) = 0;

  // ---------- Progress sync ----------

  /** Push read progress (0.0..1.0) back to the service. */
  virtual Result updateProgress(uint64_t articleId, float progress) = 0;

  // ---------- EPUB path mapping ----------

  /** Absolute SD path where the EPUB for this article should live. */
  virtual std::string epubPathFor(uint64_t articleId) const = 0;

  /** Extract article ID from an EPUB path, or 0 if not owned by this provider. */
  virtual uint64_t extractArticleId(const std::string& epubPath) const = 0;

  /** Raw PNG bytes for the default cover image. */
  virtual const char* coverPngData() const = 0;
  virtual size_t coverPngLen() const = 0;
};
