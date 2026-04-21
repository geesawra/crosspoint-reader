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
  // Build the EPUB file on SD. Returns the absolute path of the created file
  // on success, or an empty string on failure.
  static std::string build(uint64_t articleId, const char* title, const char* author, const char* rawHtml);

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
