#include "InstapaperClient.h"

#include <HTTPClient.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <StreamingJsonParser.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "InstapaperCredentialStore.h"
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

// OAuth 1.0a signatures embed a unix timestamp; Instapaper rejects anything
// more than ~5 minutes off. The ESP32-C3 has no RTC so time() is 0 until NTP
// syncs. Every signed call flows through signedPost(), so gating sync here
// catches every entry point (listBookmarks, getText, star/archive/…).
void ensureTimeSynced() {
  if (::time(nullptr) > 1600000000) return;

  // NTP sync requires a working network connection; skip if WiFi is down
  // to avoid blocking for 15 seconds on an operation that will fail anyway.
  if (WiFi.status() != WL_CONNECTED) {
    LOG_DBG("INSTA", "Skipping NTP sync (no WiFi)");
    return;
  }

  if (!esp_sntp_enabled()) {
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
  }

  for (int round = 0; round < 5; round++) {
    for (int i = 0; i < 30; i++) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    if (::time(nullptr) > 1600000000) break;
  }
  if (::time(nullptr) < 1600000000) {
    LOG_ERR("INSTA", "NTP sync timed out after 15s; OAuth signatures may be rejected");
  } else {
    LOG_DBG("INSTA", "NTP synced, epoch=%lld", static_cast<long long>(::time(nullptr)));
  }
}

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

// ---------------------------------------------------------------------------
// Streaming bookmark list parser
// ---------------------------------------------------------------------------
// Parses a JSON array of bookmark objects using StreamingJsonParser's
// callback API. No heap allocation proportional to response size — peak
// memory is bounded by TOKEN_BUF_SIZE (512 bytes) plus the output vector.

struct BookmarkParseCtx {
  std::vector<InstapaperArticle>* out;

  // Fields accumulated for the current bookmark object.
  uint64_t bookmark_id = 0;
  char title[128] = {0};
  char url[512] = {0};
  uint32_t word_count = 0;
  float progress = 0.0f;
  bool starred = false;
  time_t saved_at = 0;
  bool has_type = false;
  bool type_is_bookmark = false;
  bool in_bookmark = false;

  // Field name buffer (shared with StreamingJsonParser token buffer).
  char field_name[64] = {0};
  size_t field_name_len = 0;
};

// Copy a parsed value into a fixed char[] field, NUL-terminated.
static void copyField(char* dst, size_t dstLen, const char* val, size_t valLen) {
  if (!dst || dstLen == 0 || !val || valLen == 0) return;
  const size_t copyLen = valLen < dstLen - 1 ? valLen : dstLen - 1;
  std::memcpy(dst, val, copyLen);
  dst[copyLen] = '\0';
}

static void bookmarkOnKey(void* ctx, const char* key, size_t len) {
  auto* b = static_cast<BookmarkParseCtx*>(ctx);
  b->field_name_len = 0;
  std::memcpy(b->field_name, key, len < sizeof(b->field_name) - 1 ? len : sizeof(b->field_name) - 1);
  b->field_name[len < sizeof(b->field_name) - 1 ? len : sizeof(b->field_name) - 1] = '\0';
  (void)b;
}

static void bookmarkOnString(void* ctx, const char* val, size_t valLen) {
  auto* b = static_cast<BookmarkParseCtx*>(ctx);
  if (!b->in_bookmark) return;

  if (std::strcmp(b->field_name, "type") == 0) {
    b->has_type = true;
    b->type_is_bookmark = (valLen == 8 && std::strncmp(val, "bookmark", 8) == 0);
  } else if (std::strcmp(b->field_name, "title") == 0) {
    copyField(b->title, sizeof(b->title), val, valLen);
  } else if (std::strcmp(b->field_name, "url") == 0) {
    copyField(b->url, sizeof(b->url), val, valLen);
  }
}

static void bookmarkOnNumber(void* ctx, const char* val, size_t valLen) {
  auto* b = static_cast<BookmarkParseCtx*>(ctx);
  if (!b->in_bookmark) return;

  if (std::strcmp(b->field_name, "bookmark_id") == 0) {
    b->bookmark_id = strtoull(val, nullptr, 10);
  } else if (std::strcmp(b->field_name, "word_count") == 0) {
    b->word_count = static_cast<uint32_t>(strtoul(val, nullptr, 10));
  } else if (std::strcmp(b->field_name, "progress") == 0) {
    b->progress = strtof(val, nullptr);
  } else if (std::strcmp(b->field_name, "time") == 0) {
    b->saved_at = static_cast<time_t>(strtoll(val, nullptr, 10));
  }
}

static void bookmarkOnBool(void* ctx, bool value) {
  auto* b = static_cast<BookmarkParseCtx*>(ctx);
  if (!b->in_bookmark) return;

  if (std::strcmp(b->field_name, "starred") == 0) {
    b->starred = value;
  }
}

static void bookmarkOnArrayStart(void* ctx) { (void)ctx; }

static void bookmarkOnObjectStart(void* ctx) {
  auto* b = static_cast<BookmarkParseCtx*>(ctx);
  b->in_bookmark = true;
  b->has_type = false;
  b->type_is_bookmark = false;
  b->bookmark_id = 0;
  b->title[0] = '\0';
  b->url[0] = '\0';
  b->word_count = 0;
  b->progress = 0.0f;
  b->starred = false;
  b->saved_at = 0;
}

static void bookmarkOnObjectEnd(void* ctx) {
  auto* b = static_cast<BookmarkParseCtx*>(ctx);
  if (!b->in_bookmark || !b->has_type || !b->type_is_bookmark) {
    b->in_bookmark = false;
    return;
  }

  InstapaperArticle a;
  a.id = b->bookmark_id;
  std::memcpy(a.title, b->title, sizeof(a.title));
  const char* scheme = std::strstr(b->url, "://");
  const char* host = scheme ? scheme + 3 : b->url;
  const char* pathSep = std::strchr(host, '/');
  const size_t hostLen = pathSep ? static_cast<size_t>(pathSep - host) : std::strlen(host);
  const size_t copyLen = hostLen < sizeof(a.domain) - 1 ? hostLen : sizeof(a.domain) - 1;
  std::memcpy(a.domain, host, copyLen);
  a.domain[copyLen] = '\0';
  a.word_count = b->word_count;
  a.progress_pct = static_cast<uint32_t>(b->progress * 100.0f);
  a.starred = b->starred;
  a.saved_at = b->saved_at;

  if (b->out) b->out->push_back(a);

  b->in_bookmark = false;
}

// Parse a JSON array of bookmarks by streaming from the WiFiClient stream.
// Returns 0 on success, -1 on failure.
static int parseBookmarksStreaming(WiFiClient* stream, std::vector<InstapaperArticle>& out) {
  if (!stream) return -1;

  BookmarkParseCtx ctx;
  ctx.out = &out;

  JsonCallbacks cbs = {};
  cbs.ctx = &ctx;
  cbs.onKey = bookmarkOnKey;
  cbs.onString = bookmarkOnString;
  cbs.onNumber = bookmarkOnNumber;
  cbs.onBool = bookmarkOnBool;
  cbs.onArrayStart = bookmarkOnArrayStart;
  cbs.onArrayEnd = nullptr;
  cbs.onObjectStart = bookmarkOnObjectStart;
  cbs.onObjectEnd = bookmarkOnObjectEnd;

  StreamingJsonParser parser(cbs);
  uint8_t buf[512];

  while (stream->available()) {
    const int n = stream->read(buf, sizeof(buf));
    if (n <= 0) break;
    parser.feed(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    if (parser.hasError()) {
      LOG_ERR("INSTA", "Streaming JSON parse error");
      return -1;
    }
  }

  LOG_DBG("INSTA", "Parsed %zu bookmarks from stream", out.size());
  return 0;
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

// Send a signed POST request and stream the response. `parseFn` is called
// with the stream and user `ctx` after HTTP headers are consumed.
// Returns 0 on success. The caller is responsible for calling http.end().
static int signedPostStreaming(const char* url, const std::vector<OAuth1Signer::Param>& bodyParams,
                               int (*parseFn)(WiFiClient*, void*), void* parseCtx) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("INSTA", "No WiFi — cannot perform network operation");
    return -1;
  }
  if (!INSTAPAPER_CREDENTIALS.hasTokens()) {
    return -1;
  }

  ensureTimeSynced();

  const std::string authHeader = OAuth1Signer::buildAuthHeader(
      "POST", url, bodyParams, INSTAPAPER_CREDENTIALS.getConsumerKey().c_str(),
      INSTAPAPER_CREDENTIALS.getConsumerSecret().c_str(), INSTAPAPER_CREDENTIALS.getOauthToken().c_str(),
      INSTAPAPER_CREDENTIALS.getOauthTokenSecret().c_str());
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

  WiFiClient* stream = http.getStreamPtr();
  if (code >= 200 && code < 300 && parseFn) {
    if (parseFn(stream, parseCtx) != 0) {
      http.end();
      return -1;
    }
  }

  http.end();
  return code;
}

// Stream JSON from HTTP response into a small fixed buffer for error checking.
// Used by action endpoints that return tiny responses.
static int signedPostSmallBuffer(const char* url, const std::vector<OAuth1Signer::Param>& bodyParams,
                                 std::string* outBody) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("INSTA", "No WiFi — cannot perform network operation");
    return -1;
  }
  if (!INSTAPAPER_CREDENTIALS.hasTokens()) {
    return -1;
  }

  ensureTimeSynced();

  const std::string authHeader = OAuth1Signer::buildAuthHeader(
      "POST", url, bodyParams, INSTAPAPER_CREDENTIALS.getConsumerKey().c_str(),
      INSTAPAPER_CREDENTIALS.getConsumerSecret().c_str(), INSTAPAPER_CREDENTIALS.getOauthToken().c_str(),
      INSTAPAPER_CREDENTIALS.getOauthTokenSecret().c_str());
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

  // Action responses are tiny (< 1 KB). Stream into a small fixed buffer.
  char respBuf[1024];
  size_t respLen = 0;
  WiFiClient* stream = http.getStreamPtr();
  while (http.connected() && stream->available() && respLen < sizeof(respBuf) - 1) {
    const int avail = stream->available();
    const size_t toRead = sizeof(respBuf) - 1 - respLen < static_cast<size_t>(avail) ? sizeof(respBuf) - 1 - respLen
                                                                                     : static_cast<size_t>(avail);
    const int n = stream->read(reinterpret_cast<uint8_t*>(respBuf) + respLen, toRead);
    if (n <= 0) break;
    respLen += n;
  }
  respBuf[respLen] = '\0';

  if (outBody) *outBody = respBuf;

  http.end();

  if (code < 200 || code >= 300) {
    LOG_ERR("INSTA", "POST %s -> HTTP %d, body: %.160s", url, code, respBuf);
  } else if (outBody && outBody->find("\"type\":\"error\"") != std::string::npos) {
    LOG_ERR("INSTA", "POST %s -> API error: %.160s", url, respBuf);
  }
  return code;
}

// Stream HTML response directly to an SD file. Used for article body downloads.
int signedPostStreamingToFile(const char* url, const std::vector<OAuth1Signer::Param>& bodyParams, FsFile& outFile) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("INSTA", "No WiFi — cannot perform network operation");
    return -1;
  }
  if (!INSTAPAPER_CREDENTIALS.hasTokens()) {
    return -1;
  }

  ensureTimeSynced();

  const std::string authHeader = OAuth1Signer::buildAuthHeader(
      "POST", url, bodyParams, INSTAPAPER_CREDENTIALS.getConsumerKey().c_str(),
      INSTAPAPER_CREDENTIALS.getConsumerSecret().c_str(), INSTAPAPER_CREDENTIALS.getOauthToken().c_str(),
      INSTAPAPER_CREDENTIALS.getOauthTokenSecret().c_str());
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
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    size_t written = 0;
    while (http.connected() && (contentLen < 0 || written < static_cast<size_t>(contentLen))) {
      const int avail = stream ? stream->available() : 0;
      if (avail > 0) {
        const int n = stream->read(buf, sizeof(buf) < static_cast<size_t>(avail) ? sizeof(buf) : avail);
        if (n <= 0) break;
        if (outFile.write(buf, n) != static_cast<size_t>(n)) {
          LOG_ERR("INSTA", "Short write to HTML temp file after %zu bytes", written);
          http.end();
          return -1;
        }
        written += n;
      } else {
        delay(1);
      }
    }
    LOG_DBG("INSTA", "Streamed %zu bytes to HTML temp file", written);
  }
  http.end();
  return code;
}

// ---------------------------------------------------------------------------
// Response mapping
// ---------------------------------------------------------------------------

static InstapaperClient::ActionResult mapHttpCode(int code, const std::string& jsonBody) {
  const auto makeResult = [&](InstapaperClient::Result r, const char* defaultMsg) -> InstapaperClient::ActionResult {
    if (!jsonBody.empty()) {
      size_t msgPos = jsonBody.find("\"message\":\"");
      if (msgPos != std::string::npos) {
        msgPos += 11;
        size_t msgEnd = jsonBody.find('"', msgPos);
        if (msgEnd != std::string::npos) {
          return {r, jsonBody.substr(msgPos, msgEnd - msgPos)};
        }
      }
    }
    return {r, defaultMsg};
  };

  if (code < 0) return {InstapaperClient::NETWORK_FAILED, "SSL or DNS failure"};
  if (code == 401) return makeResult(InstapaperClient::AUTH_FAILED, "Access denied — token may be invalid or expired");
  if (code == 403) return makeResult(InstapaperClient::FORBIDDEN, "Access forbidden — account may be restricted");
  if (code == 404) return makeResult(InstapaperClient::NOT_FOUND, "The requested resource was not found");
  if (code == 429) return makeResult(InstapaperClient::RATE_LIMITED, "Too many requests — please wait a few minutes");
  if (code >= 200 && code < 300) {
    if (!jsonBody.empty() && jsonBody.find("\"type\":\"error\"") != std::string::npos) {
      if (jsonBody.find("\"error_code\":1040") != std::string::npos)
        return makeResult(InstapaperClient::RATE_LIMITED, "Too many requests — please wait a few minutes");
      if (jsonBody.find("\"error_code\":403") != std::string::npos)
        return makeResult(InstapaperClient::FORBIDDEN, "Access forbidden — account may be restricted");
      if (jsonBody.find("\"error_code\":1420") != std::string::npos)
        return makeResult(InstapaperClient::FORBIDDEN, "Account suspended or access revoked");
      if (jsonBody.find("\"error_code\":1430") != std::string::npos)
        return makeResult(InstapaperClient::FORBIDDEN, "Account subscription expired");
      return makeResult(InstapaperClient::INTERNAL_ERROR, "Unknown API error");
    }
    return {InstapaperClient::OK, {}};
  }
  return makeResult(InstapaperClient::INTERNAL_ERROR, "Server error");
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
    case FORBIDDEN:
      return "Access denied";
    case NOT_FOUND:
      return "Not found";
    case NETWORK_FAILED:
      return "Network error";
    case PARSE_FAILED:
      return "Response parse error";
    case RATE_LIMITED:
      return "Rate limited";
    case INSUFFICIENT_MEMORY:
      return "Device memory low";
    case INTERNAL_ERROR:
      return "Internal error";
  }
  return "Unknown";
}

InstapaperClient::ActionResult InstapaperClient::listBookmarks(InstapaperFolder folder, int limit,
                                                               std::vector<InstapaperArticle>& out) {
  out.clear();
  if (!INSTAPAPER_CREDENTIALS.hasTokens())
    return {NO_TOKENS, "Instapaper credentials not loaded — run instapaper_auth.py"};
  if (limit < 1) limit = 100;

  // Calculate required heap: each InstapaperArticle is ~280 bytes.
  // Add a 90 KB buffer for the WiFi / TLS / parser overhead.
  constexpr size_t NETWORK_OVERHEAD = 90 * 1024;
  const size_t required = NETWORK_OVERHEAD + static_cast<size_t>(limit) * sizeof(InstapaperArticle);
  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < required) {
    char msg[80];
    std::snprintf(msg, sizeof(msg), "Not enough memory (%zu KB free, %zu KB needed)",
                  freeHeap / 1024, required / 1024);
    LOG_ERR("INSTA", "Heap too low (%zu bytes) to fetch %d bookmarks", freeHeap, limit);
    return {INSUFFICIENT_MEMORY, msg};
  }

  char limitStr[8];
  std::snprintf(limitStr, sizeof(limitStr), "%d", limit);

  std::vector<OAuth1Signer::Param> params;
  params.push_back({"limit", limitStr});
  params.push_back({"folder_id", folderId(folder)});

  // Use streaming JSON parser — no heap allocation proportional to response.
  const int code = signedPostStreaming(
      URL_LIST, params,
      [](WiFiClient* stream, void* ctx) -> int {
        return parseBookmarksStreaming(stream, *static_cast<std::vector<InstapaperArticle>*>(ctx));
      },
      &out);

  // Sort most-recent-first by saved_at. Instapaper's default order is usually
  // chronological but not contractually guaranteed; enforce it here.
  std::sort(out.begin(), out.end(),
            [](const InstapaperArticle& a, const InstapaperArticle& b) { return a.saved_at > b.saved_at; });

  return mapHttpCode(code, {});
}

InstapaperClient::ActionResult InstapaperClient::getText(uint64_t bookmarkId, const std::string& outPath) {
  if (!INSTAPAPER_CREDENTIALS.hasTokens())
    return {NO_TOKENS, "Instapaper credentials not loaded — run instapaper_auth.py"};

  // Guardrail: refuse if heap is critically low.
  constexpr size_t MIN_HEAP = 64 * 1024;
  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "Not enough memory (%zu KB free, %zu KB needed)",
                  freeHeap / 1024, MIN_HEAP / 1024);
    LOG_ERR("INSTA", "Heap too low (%zu bytes) to fetch article", freeHeap);
    return {INSUFFICIENT_MEMORY, msg};
  }

  char idStr[24];
  std::snprintf(idStr, sizeof(idStr), "%llu", static_cast<unsigned long long>(bookmarkId));

  std::vector<OAuth1Signer::Param> params;
  params.push_back({"bookmark_id", idStr});

  FsFile file;
  if (!Storage.openFileForWrite("INSTA", outPath.c_str(), file)) {
    LOG_ERR("INSTA", "Cannot open HTML temp file: %s", outPath.c_str());
    return {NETWORK_FAILED, "Cannot write to SD card"};
  }

  const int code = signedPostStreamingToFile(URL_GET_TEXT, params, file);
  file.close();

  if (code < 0) {
    Storage.remove(outPath.c_str());
    return {NETWORK_FAILED, "SSL or DNS failure"};
  }

  ActionResult r = mapHttpCode(code, {});
  if (!r.isOk()) {
    Storage.remove(outPath.c_str());
  }
  return r;
}

namespace {
InstapaperClient::ActionResult actionWithBookmarkId(const char* url, uint64_t bookmarkId) {
  if (!INSTAPAPER_CREDENTIALS.hasTokens())
    return {InstapaperClient::NO_TOKENS, "Instapaper credentials not loaded"};
  char idStr[24];
  std::snprintf(idStr, sizeof(idStr), "%llu", static_cast<unsigned long long>(bookmarkId));
  std::vector<OAuth1Signer::Param> params;
  params.push_back({"bookmark_id", idStr});
  std::string body;
  const int code = signedPostSmallBuffer(url, params, &body);
  return mapHttpCode(code, body);
}
}  // namespace

InstapaperClient::ActionResult InstapaperClient::updateReadProgress(uint64_t bookmarkId, float progress) {
  if (!INSTAPAPER_CREDENTIALS.hasTokens())
    return {NO_TOKENS, "Instapaper credentials not loaded"};
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
  const int code = signedPostSmallBuffer(URL_UPDATE_PROGRESS, params, &body);
  return mapHttpCode(code, body);
}

InstapaperClient::ActionResult InstapaperClient::star(uint64_t id) { return actionWithBookmarkId(URL_STAR, id); }
InstapaperClient::ActionResult InstapaperClient::unstar(uint64_t id) { return actionWithBookmarkId(URL_UNSTAR, id); }
InstapaperClient::ActionResult InstapaperClient::archive(uint64_t id) { return actionWithBookmarkId(URL_ARCHIVE, id); }
InstapaperClient::ActionResult InstapaperClient::unarchive(uint64_t id) { return actionWithBookmarkId(URL_UNARCHIVE, id); }
InstapaperClient::ActionResult InstapaperClient::deleteBookmark(uint64_t id) { return actionWithBookmarkId(URL_DELETE, id); }
