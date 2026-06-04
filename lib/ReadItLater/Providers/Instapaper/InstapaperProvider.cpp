#include "InstapaperProvider.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

namespace {
std::string sanitizeFilename(const char* raw) {
  if (!raw || !*raw) return "untitled";
  std::string out;
  out.reserve(std::strlen(raw));
  for (const char* p = raw; *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|') {
      out.push_back('_');
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  // Trim trailing spaces and dots (invalid on FAT32)
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }
  if (out.empty()) return "untitled";
  // Leave room for "_{id}_v2.epub" plus a collision counter suffix
  constexpr size_t MAX_BASENAME = 200;
  if (out.size() > MAX_BASENAME) out.resize(MAX_BASENAME);
  return out;
}
}  // namespace

InstapaperFolder InstapaperProvider::toInstapaperFolder(const char* folderId) {
  if (std::strcmp(folderId, "starred") == 0) return InstapaperFolder::STARRED;
  if (std::strcmp(folderId, "archive") == 0) return InstapaperFolder::ARCHIVE;
  return InstapaperFolder::UNREAD;
}

Provider::Result InstapaperProvider::fromInstapaperResult(const InstapaperClient::ActionResult& ar) {
  Provider::Result::Code code;
  switch (ar.result) {
    case InstapaperClient::OK:
      code = Provider::Result::Code::OK;
      break;
    case InstapaperClient::NO_TOKENS:
      code = Provider::Result::Code::NO_TOKENS;
      break;
    case InstapaperClient::AUTH_FAILED:
      code = Provider::Result::Code::AUTH_FAILED;
      break;
    case InstapaperClient::FORBIDDEN:
      code = Provider::Result::Code::FORBIDDEN;
      break;
    case InstapaperClient::NOT_FOUND:
      code = Provider::Result::Code::NOT_FOUND;
      break;
    case InstapaperClient::NETWORK_FAILED:
      code = Provider::Result::Code::NETWORK_FAILED;
      break;
    case InstapaperClient::PARSE_FAILED:
      code = Provider::Result::Code::PARSE_FAILED;
      break;
    case InstapaperClient::RATE_LIMITED:
      code = Provider::Result::Code::RATE_LIMITED;
      break;
    case InstapaperClient::INSUFFICIENT_MEMORY:
      code = Provider::Result::Code::INSUFFICIENT_MEMORY;
      break;
    case InstapaperClient::INTERNAL_ERROR:
      code = Provider::Result::Code::INTERNAL_ERROR;
      break;
    default:
      code = Provider::Result::Code::INTERNAL_ERROR;
      break;
  }
  return {code, ar.message};
}

Provider::Result InstapaperProvider::listFolders(std::vector<FolderInfo>& out) {
  out = {
      {"unread", "Unread"},
      {"starred", "Starred"},
      {"archive", "Archive"},
  };
  return {Provider::Result::Code::OK, {}};
}

Provider::Result InstapaperProvider::listArticles(const FolderInfo& folder, int limit,
                                                  std::vector<ReadItLaterArticle>& out) {
  if (WiFi.status() != WL_CONNECTED) return {Provider::Result::Code::NETWORK_FAILED, "No WiFi connection"};

  std::vector<InstapaperArticle> instaArticles;
  const auto r = InstapaperClient::listBookmarks(toInstapaperFolder(folder.id), limit, instaArticles);
  if (!r.isOk()) {
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
  return {Provider::Result::Code::OK, {}};
}

Provider::Result InstapaperProvider::fetchText(uint64_t articleId, const std::string& outPath) {
  if (WiFi.status() != WL_CONNECTED) return {Provider::Result::Code::NETWORK_FAILED, "No WiFi connection"};
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
  if (WiFi.status() != WL_CONNECTED) return {Provider::Result::Code::NETWORK_FAILED, "No WiFi connection"};

  InstapaperClient::ActionResult r;
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
  if (WiFi.status() != WL_CONNECTED) return {Provider::Result::Code::NETWORK_FAILED, "No WiFi connection"};
  return fromInstapaperResult(InstapaperClient::updateReadProgress(articleId, progress));
}

std::string InstapaperProvider::epubPathFor(uint64_t articleId, const char* title) const {
  if (!title || !*title) {
    // Fallback to legacy hidden path when no title is available.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "/.crosspoint/read-it-later/article_v2_%llu.epub",
                  static_cast<unsigned long long>(articleId));
    return std::string(buf);
  }

  const std::string baseDir = "/read-it-later/Instapaper/";
  Storage.mkdir(baseDir.c_str());

  const std::string sanitized = sanitizeFilename(title);
  char idBuf[32];
  std::snprintf(idBuf, sizeof(idBuf), "_%llu", static_cast<unsigned long long>(articleId));

  std::string path = baseDir + sanitized + idBuf + "_v2.epub";
  if (!Storage.exists(path.c_str())) return path;

  // Handle collision by appending a counter before _v2.epub
  for (int counter = 1; counter < 1000; ++counter) {
    char counterBuf[16];
    std::snprintf(counterBuf, sizeof(counterBuf), "_%d", counter);
    std::string candidate = baseDir + sanitized + idBuf + counterBuf + "_v2.epub";
    if (!Storage.exists(candidate.c_str())) return candidate;
  }
  return path;  // fallback
}

uint64_t InstapaperProvider::extractArticleId(const std::string& epubPath) const {
  constexpr char SUFFIX[] = "_v2.epub";
  constexpr size_t suffixLen = sizeof(SUFFIX) - 1;
  if (epubPath.size() <= suffixLen) return 0;
  if (epubPath.compare(epubPath.size() - suffixLen, suffixLen, SUFFIX) != 0) return 0;

  // The numeric token immediately before _v2.epub is the article ID.
  size_t idEnd = epubPath.size() - suffixLen;
  size_t idStart = idEnd;
  while (idStart > 0 && epubPath[idStart - 1] != '_') --idStart;
  if (idStart == 0 || idStart == idEnd) return 0;

  uint64_t id = 0;
  for (size_t i = idStart; i < idEnd; ++i) {
    char c = epubPath[i];
    if (c < '0' || c > '9') return 0;
    id = id * 10 + static_cast<uint64_t>(c - '0');
  }
  return id;
}
