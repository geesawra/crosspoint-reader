#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    }
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }

  // Convert to string for passing to functions (single allocation)
  std::string str() const { return std::string(data, len); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (value.substr(suffixPos) != IMPORTANT) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

}  // anonymous namespace

// Clear all state and close cache file
void CssParser::clear() {
  rulesBySelector_.clear();
  index_.clear();
  lruCache_.clear();
  accessCounter_ = 0;
  rulesDataOffset_ = 0;
  if (cacheFile_) {
    cacheFile_.close();
  }
}

// FNV-1a hash for selector strings (fast, good distribution)
uint32_t CssParser::hashSelector(const std::string& selector) {
  uint32_t hash = 2166136261u;
  for (const char c : selector) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

// LRU cache lookup - returns pointer to cached style or nullptr
CssStyle* CssParser::lruLookup(const std::string& selector) const {
  for (auto& entry : lruCache_) {
    if (entry.selector == selector) {
      entry.lastAccess = ++accessCounter_;
      return &entry.style;
    }
  }
  return nullptr;
}

// LRU cache insertion - evicts least recently used if at capacity
void CssParser::lruInsert(const std::string& selector, const CssStyle& style) const {
  if (lruCache_.size() >= LRU_CAPACITY) {
    // Evict least recently used
    auto lru = std::min_element(lruCache_.begin(), lruCache_.end(),
                                [](const LruEntry& a, const LruEntry& b) { return a.lastAccess < b.lastAccess; });
    *lru = {selector, style, ++accessCounter_};
  } else {
    lruCache_.push_back({selector, style, ++accessCounter_});
  }
}

// String utilities implementation

std::string CssParser::normalized(const std::string& s) {
  std::string result;
  result.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  // Remove trailing space
  while (!result.empty() && (result.back() == ' ' || result.back() == '\n')) {
    result.pop_back();
  }
  return result;
}

void CssParser::normalizedInto(const std::string& s, std::string& out) {
  out.clear();
  out.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

std::vector<std::string> CssParser::splitOnChar(const std::string& s, const char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;

  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == delimiter) {
      std::string part = s.substr(start, i - start);
      std::string trimmed = normalized(part);
      if (!trimmed.empty()) {
        parts.push_back(trimmed);
      }
      start = i + 1;
    }
  }
  return parts;
}

std::vector<std::string> CssParser::splitWhitespace(const std::string& s) {
  std::vector<std::string> parts;
  size_t start = 0;
  bool inWord = false;

  for (size_t i = 0; i <= s.size(); ++i) {
    const bool isSpace = i == s.size() || isCssWhitespace(s[i]);
    if (isSpace && inWord) {
      parts.push_back(s.substr(start, i - start));
      inWord = false;
    } else if (!isSpace && !inWord) {
      start = i;
      inWord = true;
    }
  }
  return parts;
}

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "left" || v == "start") return CssTextAlign::Left;
  if (v == "right" || v == "end") return CssTextAlign::Right;
  if (v == "center") return CssTextAlign::Center;
  if (v == "justify") return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "italic" || v == "oblique") return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(const std::string& val) {
  const std::string v = normalized(val);

  // Named values
  if (v == "bold" || v == "bolder") return CssFontWeight::Bold;
  if (v == "normal" || v == "lighter") return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  char* endPtr = nullptr;
  const long numericWeight = std::strtol(v.c_str(), &endPtr, 10);

  // If we parsed a number and consumed the whole string
  if (endPtr != v.c_str() && *endPtr == '\0') {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }

  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(const std::string& val) {
  const std::string v = normalized(val);

  // text-decoration can have multiple space-separated values
  if (v.find("underline") != std::string::npos) {
    return CssTextDecoration::Underline;
  }
  return CssTextDecoration::None;
}

CssLength CssParser::interpretLength(const std::string& val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(const std::string& val, CssLength& out) {
  const std::string v = normalized(val);
  if (v.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  const std::string numPart = v.substr(0, unitStart);
  const std::string unitPart = v.substr(unitStart);

  char* endPtr = nullptr;
  const float numericValue = std::strtof(numPart.c_str(), &endPtr);
  if (endPtr == numPart.c_str()) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  auto unit = CssUnit::Pixels;
  if (unitPart == "em") {
    unit = CssUnit::Em;
  } else if (unitPart == "rem") {
    unit = CssUnit::Rem;
  } else if (unitPart == "pt") {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                          std::string& propValueBuf) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string::npos || colonPos == 0) return;

  normalizedInto(decl.substr(0, colonPos), propNameBuf);
  normalizedInto(decl.substr(colonPos + 1), propValueBuf);

  if (propNameBuf.empty() || propValueBuf.empty()) return;

  if (propNameBuf == "text-align") {
    style.textAlign = interpretAlignment(propValueBuf);
    style.defined.textAlign = 1;
  } else if (propNameBuf == "font-style") {
    style.fontStyle = interpretFontStyle(propValueBuf);
    style.defined.fontStyle = 1;
  } else if (propNameBuf == "font-weight") {
    style.fontWeight = interpretFontWeight(propValueBuf);
    style.defined.fontWeight = 1;
  } else if (propNameBuf == "text-decoration" || propNameBuf == "text-decoration-line") {
    style.textDecoration = interpretDecoration(propValueBuf);
    style.defined.textDecoration = 1;
  } else if (propNameBuf == "text-indent") {
    style.textIndent = interpretLength(propValueBuf);
    style.defined.textIndent = 1;
  } else if (propNameBuf == "margin-top") {
    style.marginTop = interpretLength(propValueBuf);
    style.defined.marginTop = 1;
  } else if (propNameBuf == "margin-bottom") {
    style.marginBottom = interpretLength(propValueBuf);
    style.defined.marginBottom = 1;
  } else if (propNameBuf == "margin-left") {
    style.marginLeft = interpretLength(propValueBuf);
    style.defined.marginLeft = 1;
  } else if (propNameBuf == "margin-right") {
    style.marginRight = interpretLength(propValueBuf);
    style.defined.marginRight = 1;
  } else if (propNameBuf == "margin") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.marginTop = interpretLength(values[0]);
      style.marginRight = values.size() >= 2 ? interpretLength(values[1]) : style.marginTop;
      style.marginBottom = values.size() >= 3 ? interpretLength(values[2]) : style.marginTop;
      style.marginLeft = values.size() >= 4 ? interpretLength(values[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (propNameBuf == "padding-top") {
    style.paddingTop = interpretLength(propValueBuf);
    style.defined.paddingTop = 1;
  } else if (propNameBuf == "padding-bottom") {
    style.paddingBottom = interpretLength(propValueBuf);
    style.defined.paddingBottom = 1;
  } else if (propNameBuf == "padding-left") {
    style.paddingLeft = interpretLength(propValueBuf);
    style.defined.paddingLeft = 1;
  } else if (propNameBuf == "padding-right") {
    style.paddingRight = interpretLength(propValueBuf);
    style.defined.paddingRight = 1;
  } else if (propNameBuf == "padding") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.paddingTop = interpretLength(values[0]);
      style.paddingRight = values.size() >= 2 ? interpretLength(values[1]) : style.paddingTop;
      style.paddingBottom = values.size() >= 3 ? interpretLength(values[2]) : style.paddingTop;
      style.paddingLeft = values.size() >= 4 ? interpretLength(values[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (propNameBuf == "height") {
    CssLength len;
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (propNameBuf == "width") {
    CssLength len;
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (propNameBuf == "display") {
    const std::string_view displayValue = stripTrailingImportant(propValueBuf);
    style.display = (displayValue == "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  }
}

CssStyle CssParser::parseDeclarations(const std::string& declBlock) {
  CssStyle style;
  std::string propNameBuf;
  std::string propValueBuf;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        const size_t len = i - start;
        std::string decl = declBlock.substr(start, len);
        if (!decl.empty()) {
          parseDeclarationIntoStyle(decl, style, propNameBuf, propValueBuf);
        }
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style) {
  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Handle comma-separated selectors
  const auto selectors = splitOnChar(selectorGroup, ',');

  for (const auto& sel : selectors) {
    // Validate selector length before processing
    if (sel.size() > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
      continue;
    }

    // Normalize the selector
    std::string key = normalized(sel);
    if (key.empty()) continue;

    // TODO: Consider adding support for sibling css selectors in the future
    // Ensure no + in selector as we don't support adjacent CSS selectors for now
    if (key.find('+') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for direct nested css selectors in the future
    // Ensure no > in selector as we don't support nested CSS selectors for now
    if (key.find('>') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for attribute css selectors in the future
    // Ensure no [ in selector as we don't support attribute CSS selectors for now
    if (key.find('[') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for pseudo selectors in the future
    // Ensure no : in selector as we don't support pseudo CSS selectors for now
    if (key.find(':') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for ID css selectors in the future
    // Ensure no # in selector as we don't support ID CSS selectors for now
    if (key.find('#') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for general sibling combinator selectors in the future
    // Ensure no ~ in selector as we don't support general sibling combinator CSS selectors for now
    if (key.find('~') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for wildcard css selectors in the future
    // Ensure no * in selector as we don't support wildcard CSS selectors for now
    if (key.find('*') != std::string_view::npos) {
      continue;
    }

    // TODO: Add support for more complex selectors in the future
    // At the moment, we only ever check for `tag`, `tag.class1` or `.class1`
    // If the selector has whitespace in it, then it's either a CSS selector for a descendant element (e.g. `tag1 tag2`)
    // or some other slightly more advanced CSS selector which we don't support yet
    if (key.find(' ') != std::string_view::npos) {
      continue;
    }

    // Skip if this would exceed the rule limit
    if (rulesBySelector_.size() >= MAX_RULES) {
      LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
      return;
    }

    // Store or merge with existing
    auto it = rulesBySelector_.find(key);
    if (it != rulesBySelector_.end()) {
      it->second.applyOver(style);
    } else {
      rulesBySelector_[key] = style;
    }
  }
}

// Main parsing entry point

bool CssParser::loadFromStream(FsFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;
  // Keep these as std::string since they're passed by reference to parseDeclarationIntoStyle
  std::string propNameBuf;
  std::string propValueBuf;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        if (selector.size() > MAX_SELECTOR_LENGTH * 4) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!skippingRule && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector.str(), currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
          declBuffer.clear();
        }
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(const std::string& tagName, const std::string& classAttr) const {
  static bool lowHeapWarningLogged = false;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_CSS) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      LOG_DBG("CSS", "Warning: low heap (%u bytes) below MIN_FREE_HEAP_FOR_CSS (%u), returning empty style",
              ESP.getFreeHeap(), static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    return CssStyle{};
  }

  CssStyle result;
  const std::string tag = normalized(tagName);

  // Check if we're in lazy mode (index loaded) or immediate mode (rules in memory)
  const bool lazyMode = !index_.empty();

  // Helper to look up a selector - uses LRU cache + lazy load in lazy mode, or direct map in immediate mode
  auto lookupStyle = [this, lazyMode](const std::string& selector) -> const CssStyle* {
    if (lazyMode) {
      // Check LRU cache first
      if (CssStyle* cached = lruLookup(selector)) {
        return cached;
      }
      // Load from disk and cache
      if (auto loaded = loadRuleBySelector(selector)) {
        // loadRuleBySelector already inserted into LRU, return pointer from cache
        return lruLookup(selector);
      }
      return nullptr;
    } else {
      // Immediate mode - direct map lookup
      auto it = rulesBySelector_.find(selector);
      return it != rulesBySelector_.end() ? &it->second : nullptr;
    }
  };

  // 1. Apply element-level style (lowest priority)
  if (const CssStyle* tagStyle = lookupStyle(tag)) {
    result.applyOver(*tagStyle);
  }

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority)
  if (!classAttr.empty()) {
    const auto classes = splitWhitespace(classAttr);

    for (const auto& cls : classes) {
      std::string classKey = "." + normalized(cls);
      if (const CssStyle* classStyle = lookupStyle(classKey)) {
        result.applyOver(*classStyle);
      }
    }

    // TODO: Support combinations of classes (e.g. style on p.class1.class2)
    // 3. Apply element.class styles (higher priority)
    for (const auto& cls : classes) {
      std::string combinedKey = tag + "." + normalized(cls);
      if (const CssStyle* combinedStyle = lookupStyle(combinedKey)) {
        result.applyOver(*combinedStyle);
      }
    }
  }

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string& styleValue) { return parseDeclarations(styleValue); }

// Lazy loading: load a single rule from cache file by selector
std::optional<CssStyle> CssParser::loadRuleBySelector(const std::string& selector) const {
  if (index_.empty() || !cacheFile_) {
    return std::nullopt;
  }

  const uint32_t targetHash = hashSelector(selector);

  // Binary search in sorted index
  auto it = std::lower_bound(index_.begin(), index_.end(), targetHash,
                             [](const CssIndexEntry& e, uint32_t h) { return e.selectorHash < h; });

  // Check all entries with matching hash (handle collisions)
  while (it != index_.end() && it->selectorHash == targetHash) {
    if (!cacheFile_.seekSet(it->fileOffset)) {
      ++it;
      continue;
    }

    // Read selector to verify match
    uint16_t len = 0;
    if (cacheFile_.read(&len, sizeof(len)) != sizeof(len) || len == 0 || len > MAX_SELECTOR_LENGTH) {
      ++it;
      continue;
    }

    std::string loadedSelector(len, '\0');
    if (cacheFile_.read(&loadedSelector[0], len) != static_cast<int>(len)) {
      ++it;
      continue;
    }

    if (loadedSelector != selector) {
      ++it;
      continue;
    }

    // Match found - read style
    CssStyle style;
    uint8_t enumVal;

    if (cacheFile_.read(&enumVal, 1) != 1) return std::nullopt;
    style.textAlign = static_cast<CssTextAlign>(enumVal);

    if (cacheFile_.read(&enumVal, 1) != 1) return std::nullopt;
    style.fontStyle = static_cast<CssFontStyle>(enumVal);

    if (cacheFile_.read(&enumVal, 1) != 1) return std::nullopt;
    style.fontWeight = static_cast<CssFontWeight>(enumVal);

    if (cacheFile_.read(&enumVal, 1) != 1) return std::nullopt;
    style.textDecoration = static_cast<CssTextDecoration>(enumVal);

    auto readLength = [this](CssLength& cssLen) -> bool {
      if (cacheFile_.read(&cssLen.value, sizeof(cssLen.value)) != sizeof(cssLen.value)) return false;
      uint8_t unitVal;
      if (cacheFile_.read(&unitVal, 1) != 1) return false;
      cssLen.unit = static_cast<CssUnit>(unitVal);
      return true;
    };

    if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
        !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
        !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
        !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
      return std::nullopt;
    }

    uint8_t displayVal;
    if (cacheFile_.read(&displayVal, 1) != 1) return std::nullopt;
    style.display = static_cast<CssDisplay>(displayVal);

    uint16_t definedBits = 0;
    if (cacheFile_.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) return std::nullopt;
    style.defined.textAlign = (definedBits & (1 << 0)) != 0;
    style.defined.fontStyle = (definedBits & (1 << 1)) != 0;
    style.defined.fontWeight = (definedBits & (1 << 2)) != 0;
    style.defined.textDecoration = (definedBits & (1 << 3)) != 0;
    style.defined.textIndent = (definedBits & (1 << 4)) != 0;
    style.defined.marginTop = (definedBits & (1 << 5)) != 0;
    style.defined.marginBottom = (definedBits & (1 << 6)) != 0;
    style.defined.marginLeft = (definedBits & (1 << 7)) != 0;
    style.defined.marginRight = (definedBits & (1 << 8)) != 0;
    style.defined.paddingTop = (definedBits & (1 << 9)) != 0;
    style.defined.paddingBottom = (definedBits & (1 << 10)) != 0;
    style.defined.paddingLeft = (definedBits & (1 << 11)) != 0;
    style.defined.paddingRight = (definedBits & (1 << 12)) != 0;
    style.defined.imageHeight = (definedBits & (1 << 13)) != 0;
    style.defined.imageWidth = (definedBits & (1 << 14)) != 0;
    style.defined.display = (definedBits & (1 << 15)) != 0;

    // Insert into LRU cache for fast subsequent lookups
    lruInsert(selector, style);
    return style;
  }

  return std::nullopt;
}

// Pre-warm LRU cache with common HTML element selectors
void CssParser::prewarmCommonSelectors() {
  static constexpr const char* COMMON[] = {"p",  "div",  "span", "h1",         "h2",  "h3", "h4",     "h5",
                                           "h6", "a",    "em",   "strong",     "i",   "b",  "li",     "ul",
                                           "ol", "body", "img",  "blockquote", "pre", "br", "section"};
  for (const char* sel : COMMON) {
    (void)loadRuleBySelector(sel);  // Result intentionally discarded; side effect is LRU insertion
  }
}

// Cache serialization

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());

  // Check heap before allocating index vector to avoid OOM crash
  // We only need indexEntries (8 bytes each) + safety margin
  const size_t neededBytes = ruleCount * sizeof(CssIndexEntry) + 8192;
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < neededBytes) {
    LOG_ERR("CSS", "Insufficient heap for cache save (%u free, need %zu), skipping", freeHeap, neededBytes);
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // File format: [header][rules...][index]
  // Header: version(1) + ruleCount(2) + indexOffset(4) + rulesOffset(4) = 11 bytes
  // We write rules first, then index at end, then seek back to update header offsets
  constexpr uint32_t HEADER_SIZE = 11;

  // Write placeholder header (will update offsets later)
  file.write(CSS_CACHE_VERSION);
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));
  uint32_t placeholder = 0;
  file.write(reinterpret_cast<const uint8_t*>(&placeholder), sizeof(placeholder));  // indexOffset
  file.write(reinterpret_cast<const uint8_t*>(&placeholder), sizeof(placeholder));  // rulesOffset

  const uint32_t rulesOffset = HEADER_SIZE;

  // Build index while writing rules - single pass, no second vector needed
  std::vector<CssIndexEntry> indexEntries;
  indexEntries.reserve(ruleCount);

  auto writeLength = [&file](const CssLength& len) {
    file.write(reinterpret_cast<const uint8_t*>(&len.value), sizeof(len.value));
    file.write(static_cast<uint8_t>(len.unit));
  };

  // Write rules and build index simultaneously
  for (const auto& pair : rulesBySelector_) {
    CssIndexEntry entry;
    entry.selectorHash = hashSelector(pair.first);
    entry.fileOffset = static_cast<uint32_t>(file.position());
    indexEntries.push_back(entry);

    // Write selector string (length-prefixed)
    const auto selectorLen = static_cast<uint16_t>(pair.first.size());
    file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    file.write(reinterpret_cast<const uint8_t*>(pair.first.data()), selectorLen);

    // Write CssStyle fields
    const CssStyle& style = pair.second;
    file.write(static_cast<uint8_t>(style.textAlign));
    file.write(static_cast<uint8_t>(style.fontStyle));
    file.write(static_cast<uint8_t>(style.fontWeight));
    file.write(static_cast<uint8_t>(style.textDecoration));

    writeLength(style.textIndent);
    writeLength(style.marginTop);
    writeLength(style.marginBottom);
    writeLength(style.marginLeft);
    writeLength(style.marginRight);
    writeLength(style.paddingTop);
    writeLength(style.paddingBottom);
    writeLength(style.paddingLeft);
    writeLength(style.paddingRight);
    writeLength(style.imageHeight);
    writeLength(style.imageWidth);
    file.write(static_cast<uint8_t>(style.display));

    // Write defined flags as uint16_t
    uint16_t definedBits = 0;
    if (style.defined.textAlign) definedBits |= 1 << 0;
    if (style.defined.fontStyle) definedBits |= 1 << 1;
    if (style.defined.fontWeight) definedBits |= 1 << 2;
    if (style.defined.textDecoration) definedBits |= 1 << 3;
    if (style.defined.textIndent) definedBits |= 1 << 4;
    if (style.defined.marginTop) definedBits |= 1 << 5;
    if (style.defined.marginBottom) definedBits |= 1 << 6;
    if (style.defined.marginLeft) definedBits |= 1 << 7;
    if (style.defined.marginRight) definedBits |= 1 << 8;
    if (style.defined.paddingTop) definedBits |= 1 << 9;
    if (style.defined.paddingBottom) definedBits |= 1 << 10;
    if (style.defined.paddingLeft) definedBits |= 1 << 11;
    if (style.defined.paddingRight) definedBits |= 1 << 12;
    if (style.defined.imageHeight) definedBits |= 1 << 13;
    if (style.defined.imageWidth) definedBits |= 1 << 14;
    if (style.defined.display) definedBits |= 1 << 15;
    file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
  }

  // Sort index by hash for binary search during load
  std::sort(indexEntries.begin(), indexEntries.end(),
            [](const CssIndexEntry& a, const CssIndexEntry& b) { return a.selectorHash < b.selectorHash; });

  // Write index at current position (after all rules)
  const uint32_t indexOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : indexEntries) {
    file.write(reinterpret_cast<const uint8_t*>(&entry.selectorHash), sizeof(entry.selectorHash));
    file.write(reinterpret_cast<const uint8_t*>(&entry.fileOffset), sizeof(entry.fileOffset));
  }

  // Seek back and update header with actual offsets
  file.seekSet(3);  // After version(1) + ruleCount(2)
  file.write(reinterpret_cast<const uint8_t*>(&indexOffset), sizeof(indexOffset));
  file.write(reinterpret_cast<const uint8_t*>(&rulesOffset), sizeof(rulesOffset));

  LOG_DBG("CSS", "Saved %u rules to indexed cache (index: %u bytes, heap: %u)", ruleCount,
          static_cast<unsigned>(ruleCount * sizeof(CssIndexEntry)), ESP.getFreeHeap());
  file.close();
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Clear existing state
  clear();

  // Read and verify version
  uint8_t version = 0;
  if (file.read(&version, 1) != 1) {
    file.close();
    return false;
  }

  if (version < CSS_CACHE_VERSION) {
    // Old format - delete and trigger rebuild
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CSS_CACHE_VERSION);
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  // Read header (version 5+ indexed format)
  uint16_t ruleCount = 0;
  uint32_t indexOffset = 0;
  uint32_t rulesOffset = 0;

  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount) ||
      file.read(&indexOffset, sizeof(indexOffset)) != sizeof(indexOffset) ||
      file.read(&rulesOffset, sizeof(rulesOffset)) != sizeof(rulesOffset)) {
    file.close();
    return false;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    file.close();
    return false;
  }

  // Load ONLY the index into memory
  index_.resize(ruleCount);
  if (!file.seekSet(indexOffset)) {
    index_.clear();
    file.close();
    return false;
  }

  for (uint16_t i = 0; i < ruleCount; ++i) {
    if (file.read(&index_[i].selectorHash, sizeof(index_[i].selectorHash)) != sizeof(index_[i].selectorHash) ||
        file.read(&index_[i].fileOffset, sizeof(index_[i].fileOffset)) != sizeof(index_[i].fileOffset)) {
      index_.clear();
      file.close();
      return false;
    }
  }

  // Store rules data offset for lazy loading
  rulesDataOffset_ = rulesOffset;

  // Keep file open for random access reads
  cacheFile_ = std::move(file);

  // Initialize LRU cache
  lruCache_.clear();
  lruCache_.reserve(LRU_CAPACITY);

  // Pre-warm cache with common selectors
  prewarmCommonSelectors();

  LOG_DBG("CSS", "Loaded index for %u rules (index: %zu bytes, heap free: %d)", ruleCount,
          index_.size() * sizeof(CssIndexEntry), ESP.getFreeHeap());
  return true;
}
