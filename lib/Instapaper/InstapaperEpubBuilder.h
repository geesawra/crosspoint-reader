#pragma once
#include <cstdint>
#include <string>

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
  // body fragment that expat can parse. Public for testing from companion
  // host-side unit tests if any are ever added.
  static std::string sanitizeHtmlBody(const char* rawHtml);

  // Escape text for XML attribute / content use.
  static std::string escapeXml(const char* s);
};
