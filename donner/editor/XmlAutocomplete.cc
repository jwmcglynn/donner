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
        return MakeNameContext(XmlAutocompleteContextKind::AttributeName, source, start, end,
                               cursorOffset);
      }
      break;

    case XMLTokenType::AttributeValue:
      if (state.inTag && !state.closingTag && state.currentAttributeName == "style") {
        return MakeStyleAttributeContext(token, source, cursorOffset);
      }
      break;

    case XMLTokenType::Whitespace:
      if (state.inTag && !state.closingTag && !state.currentTagName.empty()) {
        return MakeEmptyContext(XmlAutocompleteContextKind::AttributeName, cursorOffset);
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
        return MakeEmptyContext(XmlAutocompleteContextKind::AttributeName, cursorOffset);
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
        AddSuggestionIfMatching(suggestions, name, name, context.prefix,
                                svg::PropertyRegistry::isPresentationAttributeName(name));
      }
      for (std::string_view name : svg::kSVGPresentationAttributeNames) {
        const auto alreadyAdded = std::any_of(suggestions.begin(), suggestions.end(),
                                              [name](const XmlAutocompleteSuggestion& suggestion) {
                                                return suggestion.displayText == name;
                                              });
        if (!alreadyAdded) {
          AddSuggestionIfMatching(suggestions, name, name, context.prefix,
                                  svg::PropertyRegistry::isPresentationAttributeName(name));
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
