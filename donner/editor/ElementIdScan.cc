#include "donner/editor/ElementIdScan.h"

#include <cctype>

#include "donner/base/RcString.h"

namespace donner::editor {

namespace {

/// Characters allowed in an SVG id (matches the clipboard id scanner).
bool IsIdChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == ':' || c == '.';
}

}  // namespace

void CollectSubtreeElements(const svg::SVGElement& root, std::vector<svg::SVGElement>& out) {
  out.push_back(root);
  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    CollectSubtreeElements(*child, out);
  }
}

void CollectSubtreeIds(const svg::SVGElement& root, std::unordered_set<std::string>& out) {
  const RcString id = root.id();
  if (!id.empty()) {
    out.insert(id.str());
  }
  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    CollectSubtreeIds(*child, out);
  }
}

std::optional<std::string> RewriteIdReferenceInValue(std::string_view value, std::string_view oldId,
                                                     std::string_view newId) {
  // Whole-value local reference, e.g. href="#oldId".
  if (value.size() == oldId.size() + 1u && value.front() == '#' && value.substr(1) == oldId) {
    return "#" + std::string(newId);
  }

  // url(#oldId) occurrences anywhere in the value (covers presentation
  // attributes and inline `style="fill:url(#oldId)"`).
  std::string out;
  bool changed = false;
  std::size_t i = 0;
  while (i < value.size()) {
    if (value.compare(i, 5, "url(#") == 0) {
      const std::size_t start = i + 5;
      std::size_t end = start;
      while (end < value.size() && IsIdChar(value[end])) {
        ++end;
      }
      if (value.substr(start, end - start) == oldId) {
        out.append("url(#").append(newId);
        i = end;
        changed = true;
        continue;
      }
    }
    out.push_back(value[i]);
    ++i;
  }
  return changed ? std::optional<std::string>(std::move(out)) : std::nullopt;
}

std::optional<std::string> RewriteIdSelectorInStyle(std::string_view value, std::string_view oldId,
                                                    std::string_view newId) {
  std::string out;
  bool changed = false;
  std::size_t i = 0;
  // Stack of open blocks: `true` = the block holds nested rules (an at-rule
  // body such as `@media { ... }`), so `#token`s inside it are back in
  // selector position; `false` = a qualified rule's declaration block.
  std::vector<bool> blockHoldsRules;
  // True when the current block context is selector/prelude position.
  const auto inSelectorPosition = [&]() {
    return blockHoldsRules.empty() || blockHoldsRules.back();
  };
  // Whether the prelude currently being scanned starts with '@' (an at-rule,
  // whose `{` opens a rule-holding block rather than declarations).
  bool preludeIsAtRule = false;
  bool preludeSeenNonSpace = false;

  while (i < value.size()) {
    const char c = value[i];
    // Comments copy through verbatim.
    if (c == '/' && i + 1 < value.size() && value[i + 1] == '*') {
      const std::size_t end = value.find("*/", i + 2);
      const std::size_t stop = end == std::string_view::npos ? value.size() : end + 2;
      out.append(value.substr(i, stop - i));
      i = stop;
      continue;
    }
    // Quoted strings copy through verbatim (backslash escapes respected).
    if (c == '"' || c == '\'') {
      std::size_t end = i + 1;
      while (end < value.size() && value[end] != c) {
        end += (value[end] == '\\' && end + 1 < value.size()) ? 2 : 1;
      }
      const std::size_t stop = std::min(end + 1, value.size());
      out.append(value.substr(i, stop - i));
      i = stop;
      continue;
    }
    if (c == '{') {
      blockHoldsRules.push_back(inSelectorPosition() && preludeIsAtRule);
      preludeIsAtRule = false;
      preludeSeenNonSpace = false;
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == '}') {
      if (!blockHoldsRules.empty()) {
        blockHoldsRules.pop_back();
      }
      preludeIsAtRule = false;
      preludeSeenNonSpace = false;
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == ';') {
      preludeIsAtRule = false;
      preludeSeenNonSpace = false;
      out.push_back(c);
      ++i;
      continue;
    }
    if (!preludeSeenNonSpace && !std::isspace(static_cast<unsigned char>(c))) {
      preludeSeenNonSpace = true;
      preludeIsAtRule = c == '@';
    }
    if (c == '#' && value.compare(i + 1, oldId.size(), oldId) == 0) {
      const std::size_t after = i + 1 + oldId.size();
      const bool tokenBoundary = after >= value.size() || !IsIdChar(value[after]);
      // Inside a declaration block, only a `url(#id)` functional reference
      // repoints; a bare `#token` there is a color literal.
      bool isUrlReference = false;
      if (!inSelectorPosition()) {
        std::size_t back = out.size();
        while (back > 0 && std::isspace(static_cast<unsigned char>(out[back - 1]))) {
          --back;
        }
        isUrlReference = back >= 4 && out.compare(back - 4, 4, "url(") == 0;
      }
      if (tokenBoundary && (inSelectorPosition() || isUrlReference)) {
        out.push_back('#');
        out.append(newId);
        i = after;
        changed = true;
        continue;
      }
    }
    out.push_back(c);
    ++i;
  }
  return changed ? std::optional<std::string>(std::move(out)) : std::nullopt;
}

}  // namespace donner::editor
