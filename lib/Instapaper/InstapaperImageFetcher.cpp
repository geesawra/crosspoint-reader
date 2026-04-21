#include "InstapaperImageFetcher.h"

#include <HTTPClient.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <WiFiClient.h>

#include <cstring>

namespace {
constexpr size_t CHUNK_SIZE = 1024;

bool startsWith(const std::string& s, const char* prefix) {
  const size_t n = std::strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

const char* extensionFromContentType(const String& contentType) {
  String ct = contentType;
  ct.toLowerCase();
  if (ct.startsWith("image/jpeg") || ct.startsWith("image/jpg")) return "jpg";
  if (ct.startsWith("image/png")) return "png";
  return nullptr;
}
}  // namespace

InstapaperImageFetcher::Result InstapaperImageFetcher::download(const std::string& url,
                                                                const std::string& localPathBase, size_t maxBytes) {
  Result out;
  if (url.empty()) return out;
  // Only accept absolute https/http URLs — relative references would require
  // resolving against the article source, which we don't track.
  if (!startsWith(url, "http://") && !startsWith(url, "https://")) return out;

  HTTPClient http;
  NetworkClientSecure tls;
  WiFiClient plain;
  const bool isHttps = startsWith(url, "https://");
  if (isHttps) {
    tls.setInsecure();
    if (!http.begin(tls, url.c_str())) return out;
  } else {
    if (!http.begin(plain, url.c_str())) return out;
  }

  // CDNs frequently 301/302 images; without redirect following we'd give up
  // at the first hop and embed nothing. Some origins also serve a 403 to
  // bare requests — a plausible User-Agent keeps them happy.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setUserAgent("Mozilla/5.0 (compatible; CrossPoint/1.2)");

  const char* kCollectHeaders[] = {"Content-Type"};
  http.collectHeaders(kCollectHeaders, 1);

  const int code = http.GET();
  if (code != 200) {
    LOG_ERR("INSTA", "Image GET %s → %d", url.c_str(), code);
    http.end();
    return out;
  }

  const String ct = http.header("Content-Type");
  const char* ext = extensionFromContentType(ct);
  if (!ext) {
    LOG_DBG("INSTA", "Skipping image (Content-Type '%s') for %s", ct.c_str(), url.c_str());
    http.end();
    return out;
  }

  const int contentLen = http.getSize();
  if (contentLen > 0 && static_cast<size_t>(contentLen) > maxBytes) {
    LOG_DBG("INSTA", "Skipping image (too large: %d > %zu)", contentLen, maxBytes);
    http.end();
    return out;
  }

  const std::string path = localPathBase + "." + ext;

  FsFile file;
  if (!Storage.openFileForWrite("INSTA", path.c_str(), file)) {
    http.end();
    return out;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buf[CHUNK_SIZE];
  while (http.connected() && (contentLen < 0 || written < static_cast<size_t>(contentLen))) {
    if (written > maxBytes) {
      LOG_DBG("INSTA", "Image exceeded cap, truncating");
      break;
    }
    const int avail = stream ? stream->available() : 0;
    if (avail <= 0) {
      delay(1);
      continue;
    }
    const size_t toRead = sizeof(buf) < static_cast<size_t>(avail) ? sizeof(buf) : static_cast<size_t>(avail);
    const int n = stream->read(buf, toRead);
    if (n <= 0) break;
    const size_t wrote = file.write(buf, n);
    if (wrote != static_cast<size_t>(n)) {
      LOG_ERR("INSTA", "Short write during image download");
      file.close();
      Storage.remove(path.c_str());
      http.end();
      return out;
    }
    written += wrote;
  }
  file.close();
  http.end();

  if (written < 100) {  // suspiciously tiny → discard
    LOG_ERR("INSTA", "Image too small (%zu bytes), discarding %s", written, path.c_str());
    Storage.remove(path.c_str());
    return out;
  }

  LOG_DBG("INSTA", "Image saved: %s (%zu bytes, Content-Type '%s')", path.c_str(), written, ct.c_str());
  out.localPath = path;
  out.extension = ext;
  return out;
}
