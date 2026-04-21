#pragma once
#include <cstddef>
#include <string>

/**
 * Downloads a remote image URL to a temporary SD file, inferring the
 * extension from the HTTP Content-Type header. Only JPEG/PNG responses are
 * accepted — everything else yields an empty result so the caller strips the
 * `<img>` tag rather than producing something the renderer can't handle.
 *
 * Safe to call with an insecure TLS context (setInsecure()); image CDNs
 * rarely present certificates worth verifying.
 */
namespace ImageFetcher {

struct Result {
  std::string localPath;  // absolute SD path of the downloaded file, empty on failure
  std::string extension;  // "jpg" or "png" without the dot, empty on failure
};

// Download `url` to `localPathHint` (extension will be overridden with the
// detected one). Caps body size at `maxBytes`. Returns Result with empty
// strings on any failure.
Result download(const std::string& url, const std::string& localPathBase, size_t maxBytes = 150 * 1024);

}  // namespace ImageFetcher
