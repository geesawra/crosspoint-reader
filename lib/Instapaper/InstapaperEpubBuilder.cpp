#include "InstapaperEpubBuilder.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include "InstapaperCoverAsset.h"
#include "InstapaperImageFetcher.h"
#include "StoredZipWriter.h"

namespace {
constexpr char EPUB_DIR[] = "/.crosspoint/instapaper";

constexpr char MIMETYPE[] = "application/epub+zip";

constexpr char CONTAINER_XML[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles>\n"
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
    "  </rootfiles>\n"
    "</container>\n";

// Tags allowed in the sanitized output. Everything else is stripped
// (content preserved if the tag is not a scripting/styling container).
const char* const ALLOWED_TAGS[] = {"p",    "h1",  "h2",     "h3",         "h4",     "h5",         "h6",  "ul", "ol",
                                    "li",   "em",  "i",      "b",          "strong", "blockquote", "br",  "hr", "a",
                                    "span", "div", "figure", "figcaption", "code",   "pre",        "sup", "sub"};
constexpr size_t ALLOWED_TAG_COUNT = sizeof(ALLOWED_TAGS) / sizeof(ALLOWED_TAGS[0]);

// Tags whose entire subtree must be stripped (including text content).
const char* const DROP_TAGS[] = {"script", "style", "iframe", "noscript", "form", "input", "button"};
constexpr size_t DROP_TAG_COUNT = sizeof(DROP_TAGS) / sizeof(DROP_TAGS[0]);

// XML-void tags we render self-closed.
const char* const VOID_TAGS[] = {"br", "hr"};
constexpr size_t VOID_TAG_COUNT = sizeof(VOID_TAGS) / sizeof(VOID_TAGS[0]);

bool caseInsensitiveEquals(const char* a, const char* b, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return b[len] == '\0';
}

bool matchesAny(const char* name, size_t len, const char* const* list, size_t count) {
  for (size_t i = 0; i < count; i++) {
    const char* candidate = list[i];
    if (std::strlen(candidate) == len && caseInsensitiveEquals(name, candidate, len)) {
      return true;
    }
  }
  return false;
}

// Copy a substring with minimal XML escaping for text content.
void appendEscapedText(std::string& out, const char* s, size_t len) {
  out.reserve(out.size() + len);
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '<') {
      out.append("&lt;");
    } else if (c == '>') {
      out.append("&gt;");
    } else if (c == '&') {
      // Preserve already-valid numeric/named entities, otherwise escape the ampersand.
      bool looksLikeEntity = false;
      for (size_t j = i + 1; j < len && j < i + 10; j++) {
        const char cj = s[j];
        if (cj == ';') {
          looksLikeEntity = (j > i + 1);
          break;
        }
        if (!std::isalnum(static_cast<unsigned char>(cj)) && cj != '#') {
          break;
        }
      }
      if (looksLikeEntity) {
        out.push_back('&');
      } else {
        out.append("&amp;");
      }
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
}

// Extract <img src="..."> URLs from a raw HTML fragment. Caps output at
// `maxUrls`. Simple attribute scan — no HTML parser — so weird quoting is
// silently skipped. Used by build() to pre-fetch images before the sanitizer
// rewrites their references.
std::vector<std::string> extractImageUrls(const char* html, size_t maxUrls) {
  std::vector<std::string> urls;
  if (!html) return urls;
  const size_t n = std::strlen(html);
  size_t i = 0;
  while (i + 4 < n && urls.size() < maxUrls) {
    if (!((html[i] == '<') && ((html[i + 1] | 0x20) == 'i') && ((html[i + 2] | 0x20) == 'm') &&
          ((html[i + 3] | 0x20) == 'g'))) {
      i++;
      continue;
    }
    size_t end = i + 4;
    while (end < n && html[end] != '>') end++;
    if (end >= n) break;
    for (size_t p = i + 4; p + 4 < end; p++) {
      if ((html[p] | 0x20) == 's' && (html[p + 1] | 0x20) == 'r' && (html[p + 2] | 0x20) == 'c' && html[p + 3] == '=') {
        size_t vs = p + 4;
        char quote = 0;
        if (vs < end && (html[vs] == '"' || html[vs] == '\'')) {
          quote = html[vs];
          vs++;
        }
        size_t ve = vs;
        while (ve < end && html[ve] != (quote ? quote : ' ') && html[ve] != '>') ve++;
        if (ve > vs) {
          urls.emplace_back(html + vs, ve - vs);
        }
        break;
      }
    }
    i = end + 1;
  }
  return urls;
}

// Replace non-XML named entities with numeric equivalents so expat accepts them.
// Only &nbsp; is common enough to matter.
void replaceNonXmlEntities(std::string& s) {
  size_t pos = 0;
  while ((pos = s.find("&nbsp;", pos)) != std::string::npos) {
    s.replace(pos, 6, "&#160;");
    pos += 6;
  }
}
}  // namespace

std::string InstapaperEpubBuilder::sanitizeHtmlBody(const char* rawHtml,
                                                    const std::unordered_map<std::string, std::string>* imageMap) {
  if (!rawHtml) return {};

  std::string out;
  const size_t inLen = std::strlen(rawHtml);
  out.reserve(inLen + inLen / 4);

  // Tag stack to emit matching closers if the source leaves tags open.
  std::vector<std::string> openStack;

  size_t i = 0;
  while (i < inLen) {
    const char c = rawHtml[i];

    if (c != '<') {
      // Copy text, escaping stray ampersands only.
      size_t j = i;
      while (j < inLen && rawHtml[j] != '<') j++;
      appendEscapedText(out, rawHtml + i, j - i);
      i = j;
      continue;
    }

    // Find matching '>' for this tag. If none, treat as literal text.
    size_t end = i + 1;
    while (end < inLen && rawHtml[end] != '>') end++;
    if (end >= inLen) {
      appendEscapedText(out, rawHtml + i, inLen - i);
      break;
    }

    // Comment / DOCTYPE / processing instruction — drop the whole thing.
    if (rawHtml[i + 1] == '!' || rawHtml[i + 1] == '?') {
      if (i + 3 < inLen && rawHtml[i + 1] == '!' && rawHtml[i + 2] == '-' && rawHtml[i + 3] == '-') {
        const char* close = std::strstr(rawHtml + i + 4, "-->");
        i = close ? static_cast<size_t>(close - rawHtml) + 3 : inLen;
      } else {
        i = end + 1;
      }
      continue;
    }

    const bool isClose = (rawHtml[i + 1] == '/');
    const size_t nameStart = i + 1 + (isClose ? 1 : 0);
    size_t nameEnd = nameStart;
    while (nameEnd < end && !std::isspace(static_cast<unsigned char>(rawHtml[nameEnd])) && rawHtml[nameEnd] != '/' &&
           rawHtml[nameEnd] != '>') {
      nameEnd++;
    }
    const size_t nameLen = nameEnd - nameStart;
    if (nameLen == 0) {
      i = end + 1;
      continue;
    }

    const char* name = rawHtml + nameStart;

    // Strip envelope tags completely — we emit our own.
    static const char* const envelopeTags[] = {"html", "head", "body", "title", "meta", "link"};
    for (const char* env : envelopeTags) {
      if (std::strlen(env) == nameLen && caseInsensitiveEquals(name, env, nameLen)) {
        goto nextTag;
      }
    }

    // Scripting/styling: skip the subtree until matching close.
    if (!isClose && matchesAny(name, nameLen, DROP_TAGS, DROP_TAG_COUNT)) {
      std::string needle = "</";
      needle.append(name, nameLen);
      needle.push_back('>');
      const char* close = nullptr;
      for (size_t scan = end + 1; scan + needle.size() <= inLen; scan++) {
        if (caseInsensitiveEquals(rawHtml + scan, needle.c_str(), needle.size() - 1) &&
            rawHtml[scan + needle.size() - 1] == '>') {
          close = rawHtml + scan;
          break;
        }
      }
      i = close ? static_cast<size_t>(close - rawHtml) + needle.size() : inLen;
      continue;
    }

    // <img>: emit only if we have a local replacement path for its remote src.
    // This block runs before the allow/void check so an img without a match
    // can be dropped cleanly instead of falling through to the void-tag
    // emission path.
    if (!isClose && nameLen == 3 && caseInsensitiveEquals(name, "img", 3)) {
      if (imageMap && !imageMap->empty()) {
        const char* tagStart = rawHtml + i;
        const char* srcAt = nullptr;
        for (const char* p = tagStart; p + 4 < rawHtml + end; p++) {
          if ((p[0] | 0x20) == 's' && (p[1] | 0x20) == 'r' && (p[2] | 0x20) == 'c' && p[3] == '=') {
            srcAt = p + 4;
            break;
          }
        }
        if (srcAt) {
          char quote = 0;
          if (*srcAt == '"' || *srcAt == '\'') {
            quote = *srcAt;
            srcAt++;
          }
          const char* ve = srcAt;
          while (ve < rawHtml + end && *ve != (quote ? quote : ' ') && *ve != '>') ve++;
          std::string src(srcAt, ve - srcAt);
          auto it = imageMap->find(src);
          if (it != imageMap->end()) {
            out.append("<img src=\"");
            out.append(it->second);
            out.append("\"/>");
          }
        }
      }
      goto nextTag;
    }

    if (!matchesAny(name, nameLen, ALLOWED_TAGS, ALLOWED_TAG_COUNT) &&
        !matchesAny(name, nameLen, VOID_TAGS, VOID_TAG_COUNT)) {
      // Unknown tag: drop the tag itself but keep any text content that follows.
      goto nextTag;
    }

    if (isClose) {
      // Emit matching close, popping the stack if present.
      std::string lowerName(name, nameLen);
      for (char& ch : lowerName) ch = std::tolower(static_cast<unsigned char>(ch));
      for (auto it = openStack.rbegin(); it != openStack.rend(); ++it) {
        if (*it == lowerName) {
          // Close any unclosed children before popping this one.
          while (!openStack.empty() && openStack.back() != lowerName) {
            out.append("</").append(openStack.back()).append(">");
            openStack.pop_back();
          }
          out.append("</").append(openStack.back()).append(">");
          openStack.pop_back();
          break;
        }
      }
      goto nextTag;
    }

    {
      std::string lowerName(name, nameLen);
      for (char& ch : lowerName) ch = std::tolower(static_cast<unsigned char>(ch));

      const bool isVoid = matchesAny(name, nameLen, VOID_TAGS, VOID_TAG_COUNT);

      out.push_back('<');
      out.append(lowerName);

      // For <a>, preserve href if present and http(s). For <img>, preserve src only
      // if it's a data URI (remote images are stripped in MVP).
      if (lowerName == "a") {
        const char* tagStart = rawHtml + i;
        const char* hrefAt = std::strstr(tagStart, "href=");
        if (hrefAt && hrefAt < rawHtml + end) {
          const char* vs = hrefAt + 5;
          char quote = 0;
          if (*vs == '"' || *vs == '\'') {
            quote = *vs;
            vs++;
          }
          const char* ve = vs;
          while (ve < rawHtml + end && *ve != (quote ? quote : ' ') && *ve != '>') ve++;
          const size_t hlen = ve - vs;
          if (hlen > 0 && hlen < 512) {
            out.append(" href=\"");
            appendEscapedText(out, vs, hlen);
            out.push_back('"');
          }
        }
      }

      if (isVoid) {
        out.append("/>");
      } else {
        out.push_back('>');
        openStack.push_back(lowerName);
      }
    }

  nextTag:
    i = end + 1;
  }

  // Close anything left open.
  while (!openStack.empty()) {
    out.append("</").append(openStack.back()).append(">");
    openStack.pop_back();
  }

  replaceNonXmlEntities(out);
  return out;
}

std::string InstapaperEpubBuilder::escapeXml(const char* s) {
  std::string out;
  if (!s) return out;
  const size_t len = std::strlen(s);
  out.reserve(len + 8);
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    switch (c) {
      case '&':
        out.append("&amp;");
        break;
      case '<':
        out.append("&lt;");
        break;
      case '>':
        out.append("&gt;");
        break;
      case '"':
        out.append("&quot;");
        break;
      case '\'':
        out.append("&apos;");
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string InstapaperEpubBuilder::pathFor(uint64_t articleId) {
  // Versioned filename — bumping this forces every cached article to be
  // re-synthesized with the current builder. Bumped from v1 to v2 when
  // inline image embedding landed, since v1-cached EPUBs were built with
  // `<img>` stripped and would never show pictures even after the image
  // code shipped.
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s/article_v2_%llu.epub", EPUB_DIR, static_cast<unsigned long long>(articleId));
  return std::string(buf);
}

std::string InstapaperEpubBuilder::build(uint64_t articleId, const char* title, const char* author, const char* rawHtml,
                                         ProgressCallback cb, void* ctx) {
  auto report = [&](int pct, const char* label) {
    if (cb) cb(ctx, pct, label);
  };

  report(2, "Preparing");

  Storage.mkdir(EPUB_DIR);
  const std::string outPath = pathFor(articleId);

  const std::string titleEsc = escapeXml(title && *title ? title : "Untitled");
  const std::string authorEsc = escapeXml(author && *author ? author : "");

  // Download inline article images so the reader can render them offline.
  // Capped at MAX_IMAGES to avoid long downloads over slow networks and
  // bounded per-article SD usage. Failures are silently skipped — the img
  // tag is then stripped by sanitizeHtmlBody.
  constexpr size_t MAX_IMAGES = 3;
  const std::vector<std::string> imageUrls = extractImageUrls(rawHtml, MAX_IMAGES);
  LOG_DBG("INSTA", "Article has %zu image URL(s) to fetch", imageUrls.size());
  for (size_t idx = 0; idx < imageUrls.size(); idx++) {
    LOG_DBG("INSTA", "  [%zu] %s", idx, imageUrls[idx].c_str());
  }
  std::unordered_map<std::string, std::string> imageMap;  // URL → zip-relative filename
  struct DownloadedImage {
    std::string tempPath;
    std::string zipName;    // e.g. "img_0.jpg"
    std::string mediaType;  // "image/jpeg"
  };
  std::vector<DownloadedImage> downloaded;
  downloaded.reserve(imageUrls.size());

  // Image downloads dominate wall-clock for articles with photos. Spread
  // percentage across a generous 5..45 range so the bar moves visibly.
  for (size_t idx = 0; idx < imageUrls.size(); idx++) {
    char label[40];
    std::snprintf(label, sizeof(label), "Image %zu of %zu", idx + 1, imageUrls.size());
    const int pct = 5 + static_cast<int>((idx * 40) / imageUrls.size());
    report(pct, label);

    char base[96];
    std::snprintf(base, sizeof(base), "%s/tmp_img_%llu_%zu", EPUB_DIR, static_cast<unsigned long long>(articleId), idx);
    const auto result = InstapaperImageFetcher::download(imageUrls[idx], base);
    if (result.localPath.empty()) continue;
    char zipName[32];
    std::snprintf(zipName, sizeof(zipName), "img_%zu.%s", idx, result.extension.c_str());
    DownloadedImage d;
    d.tempPath = result.localPath;
    d.zipName = zipName;
    d.mediaType = (result.extension == "png") ? "image/png" : "image/jpeg";
    imageMap[imageUrls[idx]] = zipName;
    downloaded.push_back(std::move(d));
  }

  report(50, "Sanitizing");
  const std::string body = sanitizeHtmlBody(rawHtml, &imageMap);

  // Build OPF. Start with the fixed header + core manifest, then append image
  // items as they were downloaded.
  std::string opf;
  opf.reserve(1024 + downloaded.size() * 128);
  char opfHead[1024];
  const int opfHeadLen = std::snprintf(
      opfHead, sizeof(opfHead),
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\">\n"
      "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
      "    <dc:identifier id=\"bookid\">instapaper-%llu</dc:identifier>\n"
      "    <dc:title>%s</dc:title>\n"
      "    <dc:creator>%s</dc:creator>\n"
      "    <dc:language>en</dc:language>\n"
      "    <meta property=\"dcterms:modified\">1970-01-01T00:00:00Z</meta>\n"
      "  </metadata>\n"
      "  <manifest>\n"
      "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
      "    <item id=\"cover\" href=\"cover.png\" media-type=\"image/png\" properties=\"cover-image\"/>\n"
      "    <item id=\"article\" href=\"article.xhtml\" media-type=\"application/xhtml+xml\"/>\n",
      static_cast<unsigned long long>(articleId), titleEsc.c_str(),
      authorEsc.empty() ? "Instapaper" : authorEsc.c_str());
  if (opfHeadLen <= 0 || opfHeadLen >= static_cast<int>(sizeof(opfHead))) {
    LOG_ERR("INSTA", "OPF buffer overflow");
    return {};
  }
  opf.append(opfHead, opfHeadLen);
  for (size_t idx = 0; idx < downloaded.size(); idx++) {
    char line[192];
    std::snprintf(line, sizeof(line), "    <item id=\"img%zu\" href=\"%s\" media-type=\"%s\"/>\n", idx,
                  downloaded[idx].zipName.c_str(), downloaded[idx].mediaType.c_str());
    opf.append(line);
  }
  opf.append(
      "  </manifest>\n"
      "  <spine>\n"
      "    <itemref idref=\"article\"/>\n"
      "  </spine>\n"
      "</package>\n");

  // Build NAV document (EPUB 3 TOC). The reader uses this to label the
  // status-bar chapter name — without it the title reads "Unnamed".
  std::string navDoc;
  navDoc.reserve(titleEsc.size() + 512);
  navDoc.append(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE html>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
      "<head><title>Contents</title></head>\n"
      "<body>\n"
      "  <nav epub:type=\"toc\" id=\"toc\">\n"
      "    <h1>Contents</h1>\n"
      "    <ol>\n"
      "      <li><a href=\"article.xhtml\">");
  navDoc.append(titleEsc);
  navDoc.append(
      "</a></li>\n"
      "    </ol>\n"
      "  </nav>\n"
      "</body>\n"
      "</html>\n");

  // Build XHTML.
  std::string xhtml;
  xhtml.reserve(body.size() + 512);
  xhtml.append(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE html>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
      "<head>\n"
      "  <title>");
  xhtml.append(titleEsc);
  xhtml.append(
      "</title>\n"
      "</head>\n"
      "<body>\n"
      "  <h1>");
  xhtml.append(titleEsc);
  xhtml.append("</h1>\n");
  if (!authorEsc.empty()) {
    xhtml.append("  <p><em>");
    xhtml.append(authorEsc);
    xhtml.append("</em></p>\n");
  }
  xhtml.append(body);
  xhtml.append("\n</body>\n</html>\n");

  report(65, "Writing EPUB");
  StoredZipWriter zip;
  if (!zip.open(outPath.c_str())) {
    LOG_ERR("INSTA", "Cannot open %s for write", outPath.c_str());
    return {};
  }

  // mimetype must be first and STORED per EPUB spec.
  if (!zip.addFile("mimetype", MIMETYPE, sizeof(MIMETYPE) - 1)) return {};
  if (!zip.addFile("META-INF/container.xml", CONTAINER_XML, sizeof(CONTAINER_XML) - 1)) return {};
  if (!zip.addFile("OEBPS/content.opf", opf.data(), opf.size())) return {};
  if (!zip.addFile("OEBPS/nav.xhtml", navDoc.data(), navDoc.size())) return {};
  if (!zip.addFile("OEBPS/cover.png", INSTAPAPER_COVER_PNG, INSTAPAPER_COVER_PNG_LEN)) return {};
  if (!zip.addFile("OEBPS/article.xhtml", xhtml.data(), xhtml.size())) return {};

  // Add downloaded images. Stream from SD into the ZIP in small chunks —
  // loading a 300 KB image fully into heap alongside the sanitized body and
  // the OPF would push peak DRAM into bad_alloc territory on ESP32-C3.
  for (size_t idx = 0; idx < downloaded.size(); idx++) {
    const DownloadedImage& img = downloaded[idx];
    char label[40];
    std::snprintf(label, sizeof(label), "Embedding image %zu/%zu", idx + 1, downloaded.size());
    const int pct = 75 + static_cast<int>((idx * 20) / (downloaded.empty() ? 1 : downloaded.size()));
    report(pct, label);

    const std::string zipPath = std::string("OEBPS/") + img.zipName;
    if (!zip.addFileFromPath(zipPath.c_str(), img.tempPath.c_str())) {
      LOG_ERR("INSTA", "Failed to embed image %s", img.tempPath.c_str());
    }
    Storage.remove(img.tempPath.c_str());
  }

  report(97, "Finalizing");
  if (!zip.finish()) return {};

  report(100, "Done");
  LOG_DBG("INSTA", "Built EPUB: %s (+%zu images)", outPath.c_str(), downloaded.size());
  return outPath;
}
