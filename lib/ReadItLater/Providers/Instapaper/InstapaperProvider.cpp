#include "InstapaperProvider.h"

#include <Logging.h>

#include <cstring>

InstapaperFolder InstapaperProvider::toInstapaperFolder(const char* folderId) {
  if (std::strcmp(folderId, "starred") == 0) return InstapaperFolder::STARRED;
  if (std::strcmp(folderId, "archive") == 0) return InstapaperFolder::ARCHIVE;
  return InstapaperFolder::UNREAD;
}

Provider::Result InstapaperProvider::fromInstapaperResult(InstapaperClient::Result r) {
  switch (r) {
    case InstapaperClient::OK:
      return Result::OK;
    case InstapaperClient::NO_TOKENS:
      return Result::NO_TOKENS;
    case InstapaperClient::AUTH_FAILED:
      return Result::AUTH_FAILED;
    case InstapaperClient::NETWORK_FAILED:
      return Result::NETWORK_FAILED;
    case InstapaperClient::PARSE_FAILED:
      return Result::PARSE_FAILED;
    case InstapaperClient::RATE_LIMITED:
      return Result::RATE_LIMITED;
    case InstapaperClient::SERVER_ERROR:
      return Result::SERVER_ERROR;
  }
  return Result::SERVER_ERROR;
}

Provider::Result InstapaperProvider::listFolders(std::vector<FolderInfo>& out) {
  out = {
      {"unread", "Unread"},
      {"starred", "Starred"},
      {"archive", "Archive"},
  };
  return Result::OK;
}

Provider::Result InstapaperProvider::listArticles(const FolderInfo& folder, int limit,
                                                  std::vector<ReadItLaterArticle>& out) {
  std::vector<InstapaperArticle> instaArticles;
  const auto r = InstapaperClient::listBookmarks(toInstapaperFolder(folder.id), limit, instaArticles);
  if (r != InstapaperClient::OK) {
    return fromInstapaperResult(r);
  }
  out.reserve(instaArticles.size());
  for (const auto& a : instaArticles) {
    ReadItLaterArticle ra;
    ra.id = a.id;
    std::strncpy(ra.title, a.title, sizeof(ra.title) - 1);
    ra.title[sizeof(ra.title) - 1] = '\0';
    std::strncpy(ra.domain, a.domain, sizeof(ra.domain) - 1);
    ra.domain[sizeof(ra.domain) - 1] = '\0';
    std::strncpy(ra.author, a.author, sizeof(ra.author) - 1);
    ra.author[sizeof(ra.author) - 1] = '\0';
    ra.word_count = a.word_count;
    ra.progress_pct = a.progress_pct;
    ra.starred = a.starred;
    ra.saved_at = a.saved_at;
    out.push_back(ra);
  }
  return Result::OK;
}

Provider::Result InstapaperProvider::fetchText(uint64_t articleId, const std::string& outPath) {
  return fromInstapaperResult(InstapaperClient::getText(articleId, outPath));
}

std::vector<Provider::Action> InstapaperProvider::availableActions(const FolderInfo& folder,
                                                                   const ReadItLaterArticle& article) const {
  std::vector<Action> actions;
  InstapaperFolder f = toInstapaperFolder(folder.id);
  switch (f) {
    case InstapaperFolder::UNREAD:
      actions.push_back(article.starred ? Action::Unstar : Action::Star);
      actions.push_back(Action::Archive);
      actions.push_back(Action::Delete);
      break;
    case InstapaperFolder::STARRED:
      actions.push_back(Action::Unstar);
      actions.push_back(Action::Archive);
      actions.push_back(Action::Delete);
      break;
    case InstapaperFolder::ARCHIVE:
      actions.push_back(Action::Unarchive);
      actions.push_back(Action::Delete);
      break;
  }
  return actions;
}

Provider::Result InstapaperProvider::performAction(uint64_t articleId, Action action) {
  InstapaperClient::Result r = InstapaperClient::OK;
  switch (action) {
    case Action::Star:
      r = InstapaperClient::star(articleId);
      break;
    case Action::Unstar:
      r = InstapaperClient::unstar(articleId);
      break;
    case Action::Archive:
      r = InstapaperClient::archive(articleId);
      break;
    case Action::Unarchive:
      r = InstapaperClient::unarchive(articleId);
      break;
    case Action::Delete:
      r = InstapaperClient::deleteBookmark(articleId);
      break;
  }
  return fromInstapaperResult(r);
}

Provider::Result InstapaperProvider::updateProgress(uint64_t articleId, float progress) {
  return fromInstapaperResult(InstapaperClient::updateReadProgress(articleId, progress));
}

std::string InstapaperProvider::epubPathFor(uint64_t articleId) const {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/.crosspoint/read-it-later/article_v2_%llu.epub",
                static_cast<unsigned long long>(articleId));
  return std::string(buf);
}

uint64_t InstapaperProvider::extractArticleId(const std::string& epubPath) const {
  constexpr char PREFIX[] = "/.crosspoint/read-it-later/article_v2_";
  constexpr char SUFFIX[] = ".epub";
  const size_t prefixLen = sizeof(PREFIX) - 1;
  const size_t suffixLen = sizeof(SUFFIX) - 1;
  if (epubPath.size() <= prefixLen + suffixLen) return 0;
  if (epubPath.compare(0, prefixLen, PREFIX) != 0) return 0;
  if (epubPath.compare(epubPath.size() - suffixLen, suffixLen, SUFFIX) != 0) return 0;

  uint64_t id = 0;
  for (size_t i = prefixLen; i < epubPath.size() - suffixLen; i++) {
    char c = epubPath[i];
    if (c < '0' || c > '9') return 0;
    id = id * 10 + static_cast<uint64_t>(c - '0');
  }
  return id;
}
