#pragma once
#include <string>
#include <utility>
#include <vector>

/**
 * OAuth 1.0a HMAC-SHA1 request signer — just enough for Instapaper's API.
 *
 * Computes the `Authorization: OAuth …` header value for a POST request.
 * Follows RFC 5849 §3.4 (signature base string) and §3.5.1 (authorization
 * header). HMAC-SHA1 and Base64 are performed with mbedtls, which is already
 * linked via the network/TLS stack.
 */
class OAuth1Signer {
 public:
  using Param = std::pair<std::string, std::string>;

  // Percent-encode per RFC 3986 §2.3 (unreserved set only). Exposed for
  // use by the client code that URL-encodes request body parameters.
  static std::string percentEncode(const char* s);
  static std::string percentEncode(const std::string& s) { return percentEncode(s.c_str()); }

  // Build an Authorization header value. `bodyParams` are form-encoded body
  // parameters; they participate in the signature base string per RFC 5849
  // §3.4.1.3.1 when the Content-Type is application/x-www-form-urlencoded.
  //
  // Returns a complete header value string (starts with `OAuth `) — callers
  // pass it to HTTPClient::addHeader("Authorization", ...).
  static std::string buildAuthHeader(const char* httpMethod, const char* url, const std::vector<Param>& bodyParams,
                                     const char* consumerKey, const char* consumerSecret, const char* oauthToken,
                                     const char* oauthTokenSecret);
};
