#include "InstapaperTokenStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

InstapaperTokenStore InstapaperTokenStore::instance;

namespace {
constexpr char TOKENS_PATH[] = "/.crosspoint/instapaper_tokens.txt";
constexpr size_t MAX_FILE_BYTES = 1024;

// Strip leading/trailing whitespace (space, tab, CR, LF) in place.
void trim(std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
    start++;
  }
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
    end--;
  }
  s = s.substr(start, end - start);
}

// Match a line with `key=value`. On match, assign trimmed value to `out`.
bool matchField(const std::string& line, const char* key, std::string& out) {
  const size_t keyLen = strlen(key);
  if (line.size() <= keyLen || line.compare(0, keyLen, key) != 0 || line[keyLen] != '=') {
    return false;
  }
  out = line.substr(keyLen + 1);
  trim(out);
  return true;
}
}  // namespace

bool InstapaperTokenStore::loadFromFile() {
  invalidate();

  if (!Storage.exists(TOKENS_PATH)) {
    LOG_DBG("INSTA", "Tokens file missing: %s", TOKENS_PATH);
    return false;
  }

  char buf[MAX_FILE_BYTES + 1];
  const size_t n = Storage.readFileToBuffer(TOKENS_PATH, buf, sizeof(buf));
  if (n == 0) {
    LOG_ERR("INSTA", "Empty tokens file");
    return false;
  }

  // Split into lines and match each key.
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && buf[j] != '\n') {
      j++;
    }
    std::string line(buf + i, j - i);
    trim(line);
    if (!line.empty() && line[0] != '#') {
      matchField(line, "consumer_key", consumerKey) || matchField(line, "consumer_secret", consumerSecret) ||
          matchField(line, "oauth_token", oauthToken) || matchField(line, "oauth_token_secret", oauthTokenSecret);
    }
    i = j + 1;
  }

  if (!hasTokens()) {
    LOG_ERR("INSTA", "Tokens file incomplete");
    invalidate();
    return false;
  }

  LOG_DBG("INSTA", "Loaded Instapaper tokens");
  return true;
}

bool InstapaperTokenStore::hasTokens() const {
  return !consumerKey.empty() && !consumerSecret.empty() && !oauthToken.empty() && !oauthTokenSecret.empty();
}

void InstapaperTokenStore::invalidate() {
  consumerKey.clear();
  consumerSecret.clear();
  oauthToken.clear();
  oauthTokenSecret.clear();
}
