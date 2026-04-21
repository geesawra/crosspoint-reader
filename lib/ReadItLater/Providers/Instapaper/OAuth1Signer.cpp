#include "OAuth1Signer.h"

#include <Logging.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

bool isUnreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '~';
}

// 16 random bytes hex-encoded = 32 chars.
std::string makeNonce() {
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  std::string out;
  out.reserve(32);
  for (uint8_t b : raw) {
    out.push_back(HEX_DIGITS[(b >> 4) & 0xF]);
    out.push_back(HEX_DIGITS[b & 0xF]);
  }
  return out;
}

std::string makeTimestamp() {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(::time(nullptr)));
  return std::string(buf);
}

bool hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[20]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (!info) return false;
  return mbedtls_md_hmac(info, key, keyLen, msg, msgLen, out) == 0;
}

std::string base64(const uint8_t* data, size_t len) {
  // Required dst size = 4 * ceil(len/3) + 1 for NUL.
  size_t dstCap = 4 * ((len + 2) / 3) + 1;
  std::string out(dstCap, '\0');
  size_t olen = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), dstCap, &olen, data, len) != 0) {
    return {};
  }
  out.resize(olen);
  return out;
}
}  // namespace

std::string OAuth1Signer::percentEncode(const char* s) {
  std::string out;
  if (!s) return out;
  const size_t n = std::strlen(s);
  out.reserve(n + n / 4);
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (isUnreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX_DIGITS[(c >> 4) & 0xF]);
      out.push_back(HEX_DIGITS[c & 0xF]);
    }
  }
  return out;
}

std::string OAuth1Signer::buildAuthHeader(const char* httpMethod, const char* url, const std::vector<Param>& bodyParams,
                                          const char* consumerKey, const char* consumerSecret, const char* oauthToken,
                                          const char* oauthTokenSecret) {
  const std::string nonce = makeNonce();
  const std::string timestamp = makeTimestamp();

  // Percent-encoded parameter list for the signature base string.
  std::vector<Param> all;
  all.reserve(bodyParams.size() + 6);
  all.push_back({percentEncode("oauth_consumer_key"), percentEncode(consumerKey)});
  all.push_back({percentEncode("oauth_nonce"), percentEncode(nonce)});
  all.push_back({percentEncode("oauth_signature_method"), percentEncode("HMAC-SHA1")});
  all.push_back({percentEncode("oauth_timestamp"), percentEncode(timestamp)});
  all.push_back({percentEncode("oauth_token"), percentEncode(oauthToken)});
  all.push_back({percentEncode("oauth_version"), percentEncode("1.0")});
  for (const Param& p : bodyParams) {
    all.push_back({percentEncode(p.first), percentEncode(p.second)});
  }

  std::sort(all.begin(), all.end(), [](const Param& a, const Param& b) {
    return a.first != b.first ? a.first < b.first : a.second < b.second;
  });

  std::string paramStr;
  for (size_t i = 0; i < all.size(); i++) {
    if (i > 0) paramStr.push_back('&');
    paramStr.append(all[i].first);
    paramStr.push_back('=');
    paramStr.append(all[i].second);
  }

  std::string base;
  base.reserve(paramStr.size() * 2);
  base.append(httpMethod);
  base.push_back('&');
  base.append(percentEncode(url));
  base.push_back('&');
  base.append(percentEncode(paramStr));

  std::string signingKey;
  signingKey.append(percentEncode(consumerSecret));
  signingKey.push_back('&');
  signingKey.append(percentEncode(oauthTokenSecret));

  uint8_t mac[20];
  if (!hmacSha1(reinterpret_cast<const uint8_t*>(signingKey.data()), signingKey.size(),
                reinterpret_cast<const uint8_t*>(base.data()), base.size(), mac)) {
    LOG_ERR("OAUTH", "HMAC-SHA1 failed");
    return {};
  }
  const std::string signature = base64(mac, sizeof(mac));
  if (signature.empty()) {
    LOG_ERR("OAUTH", "Base64 failed");
    return {};
  }

  // Only oauth_* parameters go in the Authorization header.
  std::string header = "OAuth ";
  auto appendField = [&header](const char* k, const std::string& v, bool last) {
    header.append(k);
    header.append("=\"");
    header.append(OAuth1Signer::percentEncode(v));
    header.push_back('"');
    if (!last) header.append(", ");
  };
  appendField("oauth_consumer_key", consumerKey, false);
  appendField("oauth_nonce", nonce, false);
  appendField("oauth_signature", signature, false);
  appendField("oauth_signature_method", "HMAC-SHA1", false);
  appendField("oauth_timestamp", timestamp, false);
  appendField("oauth_token", oauthToken, false);
  appendField("oauth_version", "1.0", true);
  return header;
}
