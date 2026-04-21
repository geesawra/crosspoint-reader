#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

/**
 * Synthesizes a minimal valid EPUB from an article's HTML and writes it
 * to the provider's cache directory.
 *
 * The resulting file can be opened by the existing `Epub` / `EpubReaderActivity`
 * pipeline without any modifications.
 *
 * Parameterized by cache directory and cover asset so it works for any
 * Read-it-Later provider.
 */
class EpubBuilder {
 public:
  // Progress reporter. Called at known checkpoints during build() so the
  // calling activity can drive a progress bar or status label. `percent` is
  // 0..100, `label` is a short human-readable phase name (never null). Use
  // raw function pointer + context — std::function is avoided in this lib
  // path per the project's memory rules.
  using ProgressCallback = void (*)(void* ctx, int percent, const char* label);

  // Build the EPUB file on SD. Returns the absolute path of the created file
  // on success, or an empty string on failure. If `cb` is non-null it is
  // invoked from the same thread as the caller, synchronously, at each
  // phase transition — safe to update UI state inside.
  static std::string build(const char* cacheDir,
                           const char* coverPngData, size_t coverPngLen,
                           uint64_t articleId,
                           const char* title,
                           const char* author,
                           const char* rawHtml,
                           ProgressCallback cb = nullptr,
                           void* ctx = nullptr);

  // Compute the expected on-SD path for an article. Does not touch the SD.
  static std::string pathFor(const char* cacheDir, uint64_t articleId);

 private:
  // Convert an HTML body fragment into a well-formed XHTML body fragment
  // that expat can parse. If `imageMap` is provided, any `<img src="URL">`
  // whose URL is in the map is rewritten; others are stripped.
  static std::string sanitizeHtmlBody(const char* rawHtml,
                                      const std::unordered_map<std::string, std::string>* imageMap = nullptr);

  // Escape text for XML attribute / content use.
  static std::string escapeXml(const char* s);
};
