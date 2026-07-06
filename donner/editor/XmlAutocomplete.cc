#include "donner/editor/XmlAutocomplete.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

#include "donner/base/xml/XMLTokenizer.h"
#include "donner/svg/SVGElementNames.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {

namespace {

using xml::XMLToken;
using xml::XMLTokenType;

bool IsCssPropertyNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

bool IsAsciiWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::size_t TokenStart(const XMLToken& token) {
  return token.range.start.offset.value_or(0);
}

std::size_t TokenEnd(const XMLToken& token) {
  return token.range.end.offset.value_or(TokenStart(token));
}

bool ContainsCursor(const XMLToken& token, std::size_t cursorOffset) {
  return TokenStart(token) <= cursorOffset && cursorOffset <= TokenEnd(token);
}

bool StartsWithInsensitive(std::string_view value, std::string_view prefix) {
  if (prefix.size() > value.size()) {
    return false;
  }

  return std::equal(prefix.begin(), prefix.end(), value.begin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

std::string_view TokenText(const XMLToken& token, std::string_view source) {
  return token.text(source);
}

struct TokenState {
  bool inTag = false;
  bool closingTag = false;
  std::string currentTagName;
  std::string currentAttributeName;
  std::vector<std::string> elementStack;
};

void PopElementStack(std::vector<std::string>& stack, std::string_view name) {
  if (stack.empty()) {
    return;
  }

  if (stack.back() == name) {
    stack.pop_back();
    return;
  }

  const auto it = std::find(stack.rbegin(), stack.rend(), name);
  if (it != stack.rend()) {
    stack.erase(it.base() - 1, stack.end());
  }
}

void ApplyTokenToState(const XMLToken& token, std::string_view source, TokenState& state) {
  switch (token.type) {
    case XMLTokenType::TagOpen: {
      const std::string_view text = TokenText(token, source);
      state.inTag = true;
      state.closingTag = text.starts_with("</");
      state.currentTagName.clear();
      state.currentAttributeName.clear();
      break;
    }

    case XMLTokenType::TagName: {
      if (!state.inTag) {
        break;
      }

      const std::string_view name = TokenText(token, source);
      if (state.closingTag) {
        PopElementStack(state.elementStack, name);
      } else {
        state.currentTagName.assign(name);
      }
      break;
    }

    case XMLTokenType::AttributeName:
      if (state.inTag && !state.closingTag) {
        state.currentAttributeName = std::string(TokenText(token, source));
      }
      break;

    case XMLTokenType::AttributeValue:
      if (state.inTag && !state.closingTag) {
        state.currentAttributeName.clear();
      }
      break;

    case XMLTokenType::TagClose:
      if (state.inTag && !state.closingTag && !state.currentTagName.empty()) {
        state.elementStack.push_back(state.currentTagName);
      }
      state.inTag = false;
      state.closingTag = false;
      state.currentTagName.clear();
      state.currentAttributeName.clear();
      break;

    case XMLTokenType::TagSelfClose:
      state.inTag = false;
      state.closingTag = false;
      state.currentTagName.clear();
      state.currentAttributeName.clear();
      break;

    case XMLTokenType::Comment:
    case XMLTokenType::CData:
    case XMLTokenType::TextContent:
    case XMLTokenType::XmlDeclaration:
    case XMLTokenType::Doctype:
    case XMLTokenType::EntityRef:
    case XMLTokenType::ProcessingInstruction:
    case XMLTokenType::Whitespace:
    case XMLTokenType::ErrorRecovery: break;
  }
}

XmlAutocompleteContext MakeNameContext(XmlAutocompleteContextKind kind, std::string_view source,
                                       std::size_t start, std::size_t end,
                                       std::size_t cursorOffset) {
  cursorOffset = std::clamp(cursorOffset, start, end);
  return XmlAutocompleteContext{
      .kind = kind,
      .replaceStartOffset = start,
      .replaceEndOffset = end,
      .prefix = std::string(source.substr(start, cursorOffset - start)),
  };
}

XmlAutocompleteContext MakeEmptyContext(XmlAutocompleteContextKind kind, std::size_t cursorOffset) {
  return XmlAutocompleteContext{
      .kind = kind,
      .replaceStartOffset = cursorOffset,
      .replaceEndOffset = cursorOffset,
      .prefix = std::string(),
  };
}

XmlAutocompleteContext MakeStylePropertyContext(std::string_view source, std::size_t valueStart,
                                                std::size_t valueEnd, std::size_t cursorOffset) {
  cursorOffset = std::clamp(cursorOffset, valueStart, valueEnd);

  std::size_t segmentStart = valueStart;
  for (std::size_t i = valueStart; i < cursorOffset; ++i) {
    if (source[i] == ';' || source[i] == '{') {
      segmentStart = i + 1;
    }
  }

  while (segmentStart < cursorOffset && IsAsciiWhitespace(source[segmentStart])) {
    ++segmentStart;
  }

  const std::size_t colon = source.find(':', segmentStart);
  if (colon != std::string_view::npos && colon < cursorOffset) {
    return XmlAutocompleteContext{
        .kind = XmlAutocompleteContextKind::TextContent,
        .replaceStartOffset = cursorOffset,
        .replaceEndOffset = cursorOffset,
        .prefix = std::string(),
    };
  }

  std::size_t replaceEnd = cursorOffset;
  while (replaceEnd < valueEnd && IsCssPropertyNameChar(source[replaceEnd])) {
    ++replaceEnd;
  }

  return XmlAutocompleteContext{
      .kind = XmlAutocompleteContextKind::StyleValue,
      .replaceStartOffset = segmentStart,
      .replaceEndOffset = replaceEnd,
      .prefix = std::string(source.substr(segmentStart, cursorOffset - segmentStart)),
  };
}

XmlAutocompleteContext MakeStyleAttributeContext(const XMLToken& token, std::string_view source,
                                                 std::size_t cursorOffset) {
  const std::size_t start = TokenStart(token);
  const std::size_t end = TokenEnd(token);
  if (end <= start) {
    return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, cursorOffset);
  }

  const char quote = source[start];
  if (quote != '"' && quote != '\'') {
    return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, cursorOffset);
  }

  const bool hasClosingQuote = end > start + 1 && source[end - 1] == quote;
  const std::size_t valueStart = start + 1;
  const std::size_t valueEnd = hasClosingQuote ? end - 1 : end;
  if (cursorOffset < valueStart || cursorOffset > valueEnd) {
    return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, cursorOffset);
  }

  return MakeStylePropertyContext(source, valueStart, valueEnd, cursorOffset);
}

XmlAutocompleteContext ContextForToken(const XMLToken& token, std::string_view source,
                                       std::size_t cursorOffset, const TokenState& state) {
  const std::size_t start = TokenStart(token);
  const std::size_t end = TokenEnd(token);

  switch (token.type) {
    case XMLTokenType::TagOpen:
      if (cursorOffset == end) {
        return MakeEmptyContext(XmlAutocompleteContextKind::ElementName, cursorOffset);
      }
      break;

    case XMLTokenType::TagName:
      if (state.inTag) {
        return MakeNameContext(XmlAutocompleteContextKind::ElementName, source, start, end,
                               cursorOffset);
      }
      break;

    case XMLTokenType::AttributeName:
      if (state.inTag && !state.closingTag) {
        XmlAutocompleteContext context = MakeNameContext(XmlAutocompleteContextKind::AttributeName,
                                                         source, start, end, cursorOffset);
        context.currentTagName = state.currentTagName;
        return context;
      }
      break;

    case XMLTokenType::AttributeValue:
      if (state.inTag && !state.closingTag && state.currentAttributeName == "style") {
        return MakeStyleAttributeContext(token, source, cursorOffset);
      }
      break;

    case XMLTokenType::Whitespace:
      if (state.inTag && !state.closingTag && !state.currentTagName.empty()) {
        XmlAutocompleteContext context =
            MakeEmptyContext(XmlAutocompleteContextKind::AttributeName, cursorOffset);
        context.currentTagName = state.currentTagName;
        return context;
      }
      break;

    case XMLTokenType::TextContent:
      if (!state.elementStack.empty() && state.elementStack.back() == "style") {
        return MakeStylePropertyContext(source, start, end, cursorOffset);
      }
      return MakeEmptyContext(XmlAutocompleteContextKind::TextContent, cursorOffset);

    case XMLTokenType::ErrorRecovery:
      if (state.inTag && state.currentTagName.empty()) {
        return MakeEmptyContext(XmlAutocompleteContextKind::ElementName, cursorOffset);
      }
      if (state.inTag && !state.closingTag) {
        XmlAutocompleteContext context =
            MakeEmptyContext(XmlAutocompleteContextKind::AttributeName, cursorOffset);
        context.currentTagName = state.currentTagName;
        return context;
      }
      break;

    case XMLTokenType::TagClose:
    case XMLTokenType::TagSelfClose:
    case XMLTokenType::Comment:
    case XMLTokenType::CData:
    case XMLTokenType::XmlDeclaration:
    case XMLTokenType::Doctype:
    case XMLTokenType::EntityRef:
    case XMLTokenType::ProcessingInstruction: break;
  }

  return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, cursorOffset);
}

/// Whether \p tagName's open tag accepts a `viewBox` attribute. Small, fixed
/// set (SVG2 lists exactly these five elements), so this is gated regardless
/// of the current element - unlike the shape-only geometry gating below, a
/// wrong guess here can't be excused as "unknown element, be permissive".
bool TagAcceptsViewBox(std::string_view tagName) {
  return tagName == "svg" || tagName == "symbol" || tagName == "marker" ||
         tagName == "pattern" || tagName == "view";
}

/// Whether \p tagName's open tag accepts a `points` attribute (also a small,
/// fixed set).
bool TagAcceptsPoints(std::string_view tagName) {
  return tagName == "polygon" || tagName == "polyline";
}

/// The seven SVG basic-shape elements that get positive geometry-attribute
/// gating below. Every other element (gradients, containers, text, `<image>`,
/// `<use>`, etc. - several of which also use `x`/`y`/`width`/`height`) keeps
/// today's permissive, element-insensitive suggestions: building a complete
/// attribute-by-element matrix for all of SVG is out of scope here.
bool IsGatedShapeElement(std::string_view tagName) {
  return tagName == "path" || tagName == "rect" || tagName == "circle" || tagName == "ellipse" ||
         tagName == "line" || tagName == "polygon" || tagName == "polyline";
}

/// The basic-shape geometry attribute names gated by \ref IsGatedShapeElement
/// (a subset of \ref donner::svg::kSVGPresentationAttributeNames, which lists
/// them all in one flat, element-agnostic pool).
bool IsShapeGeometryAttributeName(std::string_view name) {
  static constexpr std::array<std::string_view, 10> kNames{
      {"d", "x", "y", "width", "height", "rx", "ry", "cx", "cy", "r"}};
  return std::find(kNames.begin(), kNames.end(), name) != kNames.end();
}

/// Whether \p tagName (one of the seven \ref IsGatedShapeElement tags) accepts
/// geometry attribute \p attributeName, per the SVG2 basic-shape geometry
/// attribute definitions:
///  - `path`: `d`.
///  - `rect`: `x`, `y`, `width`, `height`, `rx`, `ry`.
///  - `circle`: `cx`, `cy`, `r`.
///  - `ellipse`: `cx`, `cy`, `rx`, `ry`.
///  - `line`: `x1`, `y1`, `x2`, `y2`.
///  - `polygon` / `polyline`: `points`.
bool ShapeGeometryAttributeAllowed(std::string_view tagName, std::string_view attributeName) {
  if (tagName == "path") {
    return attributeName == "d";
  }
  if (tagName == "rect") {
    return attributeName == "x" || attributeName == "y" || attributeName == "width" ||
           attributeName == "height" || attributeName == "rx" || attributeName == "ry";
  }
  if (tagName == "circle") {
    return attributeName == "cx" || attributeName == "cy" || attributeName == "r";
  }
  if (tagName == "ellipse") {
    return attributeName == "cx" || attributeName == "cy" || attributeName == "rx" ||
           attributeName == "ry";
  }
  if (tagName == "line") {
    return attributeName == "x1" || attributeName == "y1" || attributeName == "x2" ||
           attributeName == "y2";
  }
  if (tagName == "polygon" || tagName == "polyline") {
    return attributeName == "points";
  }
  return false;
}

/// Whether \p name should be suppressed as an attribute suggestion given the
/// enclosing element \p tagName (empty when unknown).
bool SuppressAttributeForElement(std::string_view tagName, std::string_view name) {
  if (name == "viewBox") {
    return !TagAcceptsViewBox(tagName);
  }
  if (name == "points") {
    return !TagAcceptsPoints(tagName);
  }
  if (IsShapeGeometryAttributeName(name) && IsGatedShapeElement(tagName)) {
    return !ShapeGeometryAttributeAllowed(tagName, name);
  }
  return false;
}

void AddSuggestionIfMatching(std::vector<XmlAutocompleteSuggestion>& suggestions,
                             std::string_view name, std::string_view insertText,
                             std::string_view prefix, bool alsoPresentationAttribute) {
  if (!StartsWithInsensitive(name, prefix)) {
    return;
  }

  suggestions.push_back(XmlAutocompleteSuggestion{
      .displayText = std::string(name),
      .insertText = std::string(insertText),
      .alsoPresentationAttribute = alsoPresentationAttribute,
  });
}

}  // namespace

XmlAutocompleteContext DetectXmlAutocompleteContext(std::string_view source,
                                                    std::size_t cursorOffset) {
  cursorOffset = std::min(cursorOffset, source.size());

  TokenState state;
  XmlAutocompleteContext result =
      MakeEmptyContext(XmlAutocompleteContextKind::Unknown, cursorOffset);
  bool found = false;

  xml::Tokenize(source, [&](XMLToken token) {
    if (found) {
      return;
    }

    if (ContainsCursor(token, cursorOffset)) {
      result = ContextForToken(token, source, cursorOffset, state);
      found = true;
      return;
    }

    ApplyTokenToState(token, source, state);
  });

  return result;
}

std::vector<XmlAutocompleteSuggestion> BuildXmlAutocompleteSuggestions(
    const XmlAutocompleteContext& context) {
  std::vector<XmlAutocompleteSuggestion> suggestions;

  switch (context.kind) {
    case XmlAutocompleteContextKind::ElementName:
      for (std::string_view name : svg::kSVGElementNames) {
        AddSuggestionIfMatching(suggestions, name, name, context.prefix,
                                /*alsoPresentationAttribute=*/false);
      }
      break;

    case XmlAutocompleteContextKind::AttributeName: {
      static constexpr std::array<std::string_view, 12> kStructuralAttributeNames{{
          "id",
          "class",
          "style",
          "viewBox",
          "xmlns",
          "xmlns:xlink",
          "href",
          "xlink:href",
          "preserveAspectRatio",
          "offset",
          "points",
          "filterUnits",
      }};

      for (std::string_view name : kStructuralAttributeNames) {
        if (SuppressAttributeForElement(context.currentTagName, name)) {
          continue;
        }
        AddSuggestionIfMatching(suggestions, name, name, context.prefix,
                                svg::PropertyRegistry::isPresentationAttributeName(name));
      }
      for (std::string_view name : svg::kSVGPresentationAttributeNames) {
        if (SuppressAttributeForElement(context.currentTagName, name)) {
          continue;
        }
        const auto alreadyAdded = std::any_of(suggestions.begin(), suggestions.end(),
                                              [name](const XmlAutocompleteSuggestion& suggestion) {
                                                return suggestion.displayText == name;
                                              });
        if (!alreadyAdded) {
          AddSuggestionIfMatching(suggestions, name, name, context.prefix,
                                  svg::PropertyRegistry::isPresentationAttributeName(name));
        }
      }
      // `x1`/`y1`/`x2`/`y2` are `<line>`-only geometry attributes with no
      // entry in the shared presentation-attribute pool above (unlike the
      // other gated geometry names, which are already listed there for every
      // other element to draw from).
      if (context.currentTagName == "line") {
        for (std::string_view name : {"x1", "y1", "x2", "y2"}) {
          AddSuggestionIfMatching(suggestions, name, name, context.prefix,
                                  /*alsoPresentationAttribute=*/false);
        }
      }
      break;
    }

    case XmlAutocompleteContextKind::StyleValue:
      for (std::string_view name : svg::PropertyRegistry::propertyNames()) {
        const std::string insertText = std::string(name) + ": ";
        AddSuggestionIfMatching(suggestions, name, insertText, context.prefix,
                                svg::PropertyRegistry::isPresentationAttributeName(name));
      }
      break;

    case XmlAutocompleteContextKind::TextContent:
    case XmlAutocompleteContextKind::Unknown: break;
  }

  return suggestions;
}

}  // namespace donner::editor
