#include "InstapaperClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "InstapaperTokenStore.h"
#include "OAuth1Signer.h"

namespace {
constexpr char API_ROOT[] = "https://www.instapaper.com/api/1";

constexpr char URL_LIST[] = "https://www.instapaper.com/api/1/bookmarks/list";
constexpr char URL_GET_TEXT[] = "https://www.instapaper.com/api/1/bookmarks/get_text";
constexpr char URL_UPDATE_PROGRESS[] = "https://www.instapaper.com/api/1/bookmarks/update_read_progress";
constexpr char URL_STAR[] = "https://www.instapaper.com/api/1/bookmarks/star";
constexpr char URL_UNSTAR[] = "https://www.instapaper.com/api/1/bookmarks/unstar";
constexpr char URL_ARCHIVE[] = "https://www.instapaper.com/api/1/bookmarks/archive";
constexpr char URL_UNARCHIVE[] = "https://www.instapaper.com/api/1/bookmarks/unarchive";
constexpr char URL_DELETE[] = "https://www.instapaper.com/api/1/bookmarks/delete";

const char* folderId(InstapaperFolder f) {
  switch (f) {
    case InstapaperFolder::UNREAD:
      return "unread";
    case InstapaperFolder::STARRED:
      return "starred";
    case InstapaperFolder::ARCHIVE:
      return "archive";
  }
  return "unread";
}

// Build `k1=v1&k2=v2...` using the percent-encoding appropriate for bodies.
std::string buildFormBody(const std::vector<OAuth1Signer::Param>& params) {
  std::string s;
  for (size_t i = 0; i < params.size(); i++) {
    if (i > 0) s.push_back('&');
    s.append(OAuth1Signer::percentEncode(params[i].first));
    s.push_back('=');
    s.append(OAuth1Signer::percentEncode(params[i].second));
  }
  return s;
}

// Shared POST helper. Sends a signed form-encoded request and returns the
// HTTP status code. On success, either captures the response body into
// `outBody` (if non-null) or streams to `outStream` (if non-null). Caller
// may pass both as null to drop the body.
int signedPost(const char* url, const std::vector<OAuth1Signer::Param>& bodyParams, std::string* outBody) {
  if (!INSTAPAPER_TOKENS.hasTokens()) {
    return -1;
  }

  const std::string authHeader = OAuth1Signer::buildAuthHeader(
      "POST", url, bodyParams, INSTAPAPER_TOKENS.getConsumerKey().c_str(),
      INSTAPAPER_TOKENS.getConsumerSecret().c_str(), INSTAPAPER_TOKENS.getOauthToken().c_str(),
      INSTAPAPER_TOKENS.getOauthTokenSecret().c_str());
  if (authHeader.empty()) {
    LOG_ERR("INSTA", "Failed to build Authorization header");
    return -1;
  }

  const std::string body = buildFormBody(bodyParams);

  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;

  if (!http.begin(tls, url)) {
    LOG_ERR("INSTA", "HTTPClient.begin failed for %s", url);
    return -1;
  }
  http.addHeader("Authorization", authHeader.c_str());
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Accept", "application/json");

  const int code = http.POST(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(body.data())), body.size());

  // Always capture body so we can surface API-level error messages even when
  // the HTTP status is 200 (Instapaper returns {"type":"error", …} with a 200
  // for invalid auth in some cases).
  const String respBody = http.getString();
  if (outBody) {
    *outBody = respBody.c_str();
  }
  http.end();

  if (code < 200 || code >= 300) {
    LOG_ERR("INSTA", "POST %s → HTTP %d, body: %.160s", url, code, respBody.c_str());
  } else if (respBody.indexOf("\"type\":\"error\"") >= 0) {
    LOG_ERR("INSTA", "POST %s → API error: %.160s", url, respBody.c_str());
  }
  return code;
}

// Same as signedPost but streams the response body to a std::string (no JSON
// parsing here — used by get_text which returns raw HTML, potentially large).
int signedPostStreaming(const char* url, const std::vector<OAuth1Signer::Param>& bodyParams, std::string& outBody) {
  if (!INSTAPAPER_TOKENS.hasTokens()) {
    return -1;
  }

  const std::string authHeader = OAuth1Signer::buildAuthHeader(
      "POST", url, bodyParams, INSTAPAPER_TOKENS.getConsumerKey().c_str(),
      INSTAPAPER_TOKENS.getConsumerSecret().c_str(), INSTAPAPER_TOKENS.getOauthToken().c_str(),
      INSTAPAPER_TOKENS.getOauthTokenSecret().c_str());
  if (authHeader.empty()) return -1;

  const std::string body = buildFormBody(bodyParams);

  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;

  if (!http.begin(tls, url)) return -1;
  http.addHeader("Authorization", authHeader.c_str());
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Accept", "text/html,*/*");

  const int code = http.POST(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(body.data())), body.size());
  if (code >= 200 && code < 300) {
    const int contentLen = http.getSize();
    if (contentLen > 0) outBody.reserve(static_cast<size_t>(contentLen));
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    while (http.connected() && (contentLen < 0 || outBody.size() < static_cast<size_t>(contentLen))) {
      const int avail = stream ? stream->available() : 0;
      if (avail > 0) {
        const int n = stream->read(buf, sizeof(buf) < static_cast<size_t>(avail) ? sizeof(buf) : avail);
        if (n <= 0) break;
        outBody.append(reinterpret_cast<const char*>(buf), n);
      } else {
        delay(1);
      }
    }
  }
  http.end();
  return code;
}

InstapaperClient::Result mapHttpCode(int code, const std::string& jsonBody = std::string()) {
  if (code < 0) return InstapaperClient::NETWORK_FAILED;
  if (code == 401 || code == 403) return InstapaperClient::AUTH_FAILED;
  if (code == 429) return InstapaperClient::RATE_LIMITED;
  if (code >= 200 && code < 300) {
    // Instapaper sometimes returns 200 OK with an embedded error object.
    if (!jsonBody.empty() && jsonBody.find("\"type\":\"error\"") != std::string::npos) {
      if (jsonBody.find("\"error_code\":1040") != std::string::npos) return InstapaperClient::RATE_LIMITED;
      if (jsonBody.find("\"error_code\":403") != std::string::npos ||
          jsonBody.find("\"error_code\":1420") != std::string::npos ||
          jsonBody.find("\"error_code\":1430") != std::string::npos) {
        return InstapaperClient::AUTH_FAILED;
      }
      return InstapaperClient::SERVER_ERROR;
    }
    return InstapaperClient::OK;
  }
  return InstapaperClient::SERVER_ERROR;
}

// Safely copy a std::string into a fixed char[] field, NUL-terminated.
void copyInto(char* dst, size_t dstLen, const char* src) {
  if (!dst || dstLen == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}
}  // namespace

const char* InstapaperClient::errorString(Result r) {
  switch (r) {
    case OK:
      return "OK";
    case NO_TOKENS:
      return "No tokens loaded";
    case AUTH_FAILED:
      return "Auth failed";
    case NETWORK_FAILED:
      return "Network error";
    case PARSE_FAILED:
      return "Response parse error";
    case RATE_LIMITED:
      return "Rate limited";
    case SERVER_ERROR:
      return "Server error";
  }
  return "Unknown";
}

InstapaperClient::Result InstapaperClient::listBookmarks(InstapaperFolder folder, int limit,
                                                         std::vector<InstapaperArticle>& out) {
  out.clear();
  if (!INSTAPAPER_TOKENS.hasTokens()) return NO_TOKENS;
  if (limit < 1) limit = 25;
  if (limit > 500) limit = 500;

  char limitStr[8];
  std::snprintf(limitStr, sizeof(limitStr), "%d", limit);

  std::vector<OAuth1Signer::Param> params;
  params.push_back({"limit", limitStr});
  params.push_back({"folder_id", folderId(folder)});

  std::string body;
  const int code = signedPost(URL_LIST, params, &body);
  const Result r = mapHttpCode(code, body);
  if (r != OK) return r;

  JsonDocument doc;
  const auto err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR("INSTA", "JSON parse failed: %s", err.c_str());
    return PARSE_FAILED;
  }

  JsonArrayConst arr = doc.as<JsonArrayConst>();
  if (arr.isNull()) {
    LOG_ERR("INSTA", "List response is not an array");
    return PARSE_FAILED;
  }

  out.reserve(arr.size());
  for (JsonVariantConst item : arr) {
    const char* type = item["type"] | "";
    if (std::strcmp(type, "bookmark") != 0) continue;
    InstapaperArticle a;
    a.id = item["bookmark_id"].as<uint64_t>();
    copyInto(a.title, sizeof(a.title), item["title"] | "");
    // `domain` is extracted client-side from the URL; Instapaper returns a
    // full URL in `url` rather than a bare host.
    const char* url = item["url"] | "";
    if (url && *url) {
      const char* scheme = std::strstr(url, "://");
      const char* host = scheme ? scheme + 3 : url;
      const char* pathSep = std::strchr(host, '/');
      const size_t hostLen = pathSep ? static_cast<size_t>(pathSep - host) : std::strlen(host);
      const size_t copyLen = hostLen < sizeof(a.domain) - 1 ? hostLen : sizeof(a.domain) - 1;
      std::memcpy(a.domain, host, copyLen);
      a.domain[copyLen] = '\0';
    }
    const char* author = item["title"].as<const char*>();
    (void)author;  // Instapaper `bookmark` object has no author field; leave blank.
    a.word_count = item["word_count"] | 0;
    a.progress_pct = static_cast<uint32_t>((item["progress"] | 0.0f) * 100.0f);
    a.starred = (std::strcmp(item["starred"] | "0", "1") == 0);
    a.saved_at = item["time"] | 0;
    out.push_back(a);
  }
  return OK;
}

InstapaperClient::Result InstapaperClient::getText(uint64_t bookmarkId, std::string& outHtml) {
  if (!INSTAPAPER_TOKENS.hasTokens()) return NO_TOKENS;
  outHtml.clear();

  char idStr[24];
  std::snprintf(idStr, sizeof(idStr), "%llu", static_cast<unsigned long long>(bookmarkId));

  std::vector<OAuth1Signer::Param> params;
  params.push_back({"bookmark_id", idStr});

  const int code = signedPostStreaming(URL_GET_TEXT, params, outHtml);
  return mapHttpCode(code, outHtml);
}

namespace {
InstapaperClient::Result actionWithBookmarkId(const char* url, uint64_t bookmarkId) {
  if (!INSTAPAPER_TOKENS.hasTokens()) return InstapaperClient::NO_TOKENS;
  char idStr[24];
  std::snprintf(idStr, sizeof(idStr), "%llu", static_cast<unsigned long long>(bookmarkId));
  std::vector<OAuth1Signer::Param> params;
  params.push_back({"bookmark_id", idStr});
  std::string body;
  const int code = signedPost(url, params, &body);
  return mapHttpCode(code, body);
}
}  // namespace

InstapaperClient::Result InstapaperClient::updateReadProgress(uint64_t bookmarkId, float progress) {
  if (!INSTAPAPER_TOKENS.hasTokens()) return NO_TOKENS;
  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;

  char idStr[24];
  std::snprintf(idStr, sizeof(idStr), "%llu", static_cast<unsigned long long>(bookmarkId));
  char progressStr[16];
  std::snprintf(progressStr, sizeof(progressStr), "%.4f", progress);
  char timestampStr[24];
  std::snprintf(timestampStr, sizeof(timestampStr), "%lld", static_cast<long long>(::time(nullptr)));

  std::vector<OAuth1Signer::Param> params;
  params.push_back({"bookmark_id", idStr});
  params.push_back({"progress", progressStr});
  params.push_back({"progress_timestamp", timestampStr});

  std::string body;
  const int code = signedPost(URL_UPDATE_PROGRESS, params, &body);
  return mapHttpCode(code, body);
}

InstapaperClient::Result InstapaperClient::star(uint64_t id) { return actionWithBookmarkId(URL_STAR, id); }
InstapaperClient::Result InstapaperClient::unstar(uint64_t id) { return actionWithBookmarkId(URL_UNSTAR, id); }
InstapaperClient::Result InstapaperClient::archive(uint64_t id) { return actionWithBookmarkId(URL_ARCHIVE, id); }
InstapaperClient::Result InstapaperClient::unarchive(uint64_t id) { return actionWithBookmarkId(URL_UNARCHIVE, id); }
InstapaperClient::Result InstapaperClient::deleteBookmark(uint64_t id) { return actionWithBookmarkId(URL_DELETE, id); }
