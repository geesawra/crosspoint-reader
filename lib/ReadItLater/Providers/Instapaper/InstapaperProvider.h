#pragma once

#include <Provider.h>

#include "InstapaperClient.h"
#include "InstapaperCredentialStore.h"
#include "InstapaperCoverAsset.h"

/**
 * Instapaper implementation of the Provider interface.
 *
 * Wraps the existing InstapaperClient and exposes it through the generic
 * Provider interface so the UI activities work without knowing about
 * Instapaper-specific types.
 */
class InstapaperProvider : public Provider {
 public:
  InstapaperProvider() = default;

  const char* name() const override { return "Instapaper"; }
  const char* cacheDirName() const override { return "instapaper"; }

  bool loadCredentials() override { return INSTAPAPER_CREDENTIALS.loadFromFile(); }
  bool isConfigured() const override { return INSTAPAPER_CREDENTIALS.hasTokens(); }

  Result listFolders(std::vector<FolderInfo>& out) override;
  Result listArticles(const FolderInfo& folder, int limit, std::vector<ReadItLaterArticle>& out) override;
  Result fetchText(uint64_t articleId, std::string& outHtml) override;
  std::vector<Action> availableActions(const FolderInfo& folder, const ReadItLaterArticle& article) const override;
  Result performAction(uint64_t articleId, Action action) override;
  Result updateProgress(uint64_t articleId, float progress) override;

  std::string epubPathFor(uint64_t articleId) const override;
  uint64_t extractArticleId(const std::string& epubPath) const override;
  const char* coverPngData() const override { return reinterpret_cast<const char*>(INSTAPAPER_COVER_PNG); }
  size_t coverPngLen() const override { return INSTAPAPER_COVER_PNG_LEN; }

 private:
  static InstapaperFolder toInstapaperFolder(const char* folderId);
  static Result fromInstapaperResult(InstapaperClient::Result r);
};
