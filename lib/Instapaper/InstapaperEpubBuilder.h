#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

/**
 * Synthesizes a minimal valid EPUB from an Instapaper article and writes it
 * to /.crosspoint/instapaper/article_<id>.epub.
 *
 * The resulting file can be opened by the existing `Epub` / `EpubReaderActivity`
 * pipeline without any modifications. This reuses all caching, progress
 * tracking, CSS handling, and pagination work already present in the codebase.
 *
 * The output contains four STORED entries:
 *   mimetype
 *   META-INF/container.xml
 *   OEBPS/content.opf
 *   OEBPS/article.xhtml
 */
class InstapaperEpubBuilder {
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
  static std::string build(uint64_t articleId, const char* title, const char* author, const char* rawHtml,
                           ProgressCallback cb = nullptr, void* ctx = nullptr);

  // Compute the expected on-SD path for an article. Does not touch the SD.
  static std::string pathFor(uint64_t articleId);

 private:
  // Convert an HTML body fragment from Instapaper into a well-formed XHTML
  // body fragment that expat can parse. If `imageMap` is provided, any
  // `<img src="URL">` whose URL is in the map is rewritten to
  // `<img src="<mapped value>"/>`; others are stripped. If the map is null,
  // all `<img>` tags are stripped.
  static std::string sanitizeHtmlBody(const char* rawHtml,
                                      const std::unordered_map<std::string, std::string>* imageMap = nullptr);

  // Escape text for XML attribute / content use.
  static std::string escapeXml(const char* s);
};
