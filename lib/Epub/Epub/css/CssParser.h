#pragma once

#include <HalStorage.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * Index entry for lazy CSS rule loading.
 * Maps a selector hash to its file offset in the cache.
 */
struct CssIndexEntry {
  uint32_t selectorHash;
  uint32_t fileOffset;
};

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint8_t CSS_CACHE_VERSION = 5;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() { clear(); }

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found)
   */
  bool loadFromStream(FsFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(const std::string& tagName, const std::string& classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(const std::string& styleValue);

  /**
   * Check if any rules have been loaded (includes indexed rules)
   */
  [[nodiscard]] bool empty() const { return rulesBySelector_.empty() && index_.empty(); }

  /**
   * Get count of loaded rule sets (includes indexed rules)
   */
  [[nodiscard]] size_t ruleCount() const { return index_.empty() ? rulesBySelector_.size() : index_.size(); }

  /**
   * Clear all loaded rules and close cache file
   */
  void clear();

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache() const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache();

 private:
  // Storage: maps normalized selector -> style properties
  // In lazy mode, this is only used during initial parsing before cache is written
  std::unordered_map<std::string, CssStyle> rulesBySelector_;

  std::string cachePath;

  // Index-based lazy loading (version 5+ cache format)
  std::vector<CssIndexEntry> index_;      // Sorted by selectorHash for binary search
  mutable FsFile cacheFile_;              // Kept open for random-access reads
  mutable uint32_t rulesDataOffset_ = 0;  // Offset to rules data section in cache file

  // LRU cache for frequently accessed rules
  static constexpr size_t LRU_CAPACITY = 256;
  struct LruEntry {
    std::string selector;
    CssStyle style;
    uint32_t lastAccess = 0;
  };
  mutable std::vector<LruEntry> lruCache_;
  mutable uint32_t accessCounter_ = 0;

  // Lazy loading helpers
  [[nodiscard]] static uint32_t hashSelector(const std::string& selector);
  [[nodiscard]] CssStyle* lruLookup(const std::string& selector) const;
  void lruInsert(const std::string& selector, const CssStyle& style) const;
  [[nodiscard]] std::optional<CssStyle> loadRuleBySelector(const std::string& selector) const;
  void prewarmCommonSelectors();

  // Internal parsing helpers
  void processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(const std::string& declBlock);
  static void parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                        std::string& propValueBuf);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(const std::string& val);
  static CssFontStyle interpretFontStyle(const std::string& val);
  static CssFontWeight interpretFontWeight(const std::string& val);
  static CssTextDecoration interpretDecoration(const std::string& val);
  static CssLength interpretLength(const std::string& val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(const std::string& val, CssLength& out);

  // String utilities
  static std::string normalized(const std::string& s);
  static void normalizedInto(const std::string& s, std::string& out);
  static std::vector<std::string> splitOnChar(const std::string& s, char delimiter);
  static std::vector<std::string> splitWhitespace(const std::string& s);
};
