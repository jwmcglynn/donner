#include "donner/editor/XmlAutocomplete.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/StringUtils.h"
#include "donner/base/xml/XMLTokenizer.h"
#include "donner/css/Serialize.h"
#include "donner/css/parser/details/ComponentValueParser.h"
#include "donner/css/parser/details/Tokenizer.h"
#include "donner/editor/CssSourceSyntax.h"
#include "donner/svg/SVGElementNames.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {

namespace {

using xml::XMLToken;
using xml::XMLTokenType;

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
    return StringUtils::ToLowerAscii(a) == StringUtils::ToLowerAscii(b);
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

enum class CssCursorMode { Selector, AtRule, DeclarationName, DeclarationValue };
enum class CssCursorBlock { RuleList, Declarations };

struct CssCursorState {
  explicit CssCursorState(CssSourceRegionKind kind)
      : mode(kind == CssSourceRegionKind::Stylesheet ? CssCursorMode::Selector
                                                     : CssCursorMode::DeclarationName),
        rootDeclarations(kind == CssSourceRegionKind::DeclarationList) {
    if (kind == CssSourceRegionKind::PropertyValue) {
      mode = CssCursorMode::DeclarationValue;
    }
  }

  CssCursorMode mode;
  bool rootDeclarations = false;
  std::vector<CssCursorBlock> blocks;
  std::size_t skippedDepth = 0;
  std::vector<int> variableNameDepths;
  std::string propertyName;
  std::string atRuleName;
  std::optional<std::size_t> selectorMarkerStart;
  int parenthesisDepth = 0;
};

bool InDeclarations(const CssCursorState& state) {
  return !state.blocks.empty() ? state.blocks.back() == CssCursorBlock::Declarations
                               : state.rootDeclarations;
}

XmlAutocompleteContextKind CssContextKind(const CssCursorState& state) {
  if (!state.variableNameDepths.empty() &&
      state.variableNameDepths.back() == state.parenthesisDepth) {
    return XmlAutocompleteContextKind::CssVariableName;
  }
  switch (state.mode) {
    case CssCursorMode::Selector: return XmlAutocompleteContextKind::CssSelector;
    case CssCursorMode::AtRule: return XmlAutocompleteContextKind::CssAtRuleName;
    case CssCursorMode::DeclarationName: return XmlAutocompleteContextKind::CssDeclarationName;
    case CssCursorMode::DeclarationValue: return XmlAutocompleteContextKind::CssDeclarationValue;
  }
  return XmlAutocompleteContextKind::Unknown;
}

void AdvanceNamedCssToken(const css::Token& token, CssCursorState& state) {
  if (const auto* ident = token.tryGet<css::Token::Ident>()) {
    if (state.mode == CssCursorMode::DeclarationName) {
      state.propertyName = std::string(ident->value);
    }
    state.selectorMarkerStart.reset();
  } else if (const auto* function = token.tryGet<css::Token::Function>()) {
    ++state.parenthesisDepth;
    if (function->name.equalsLowercase("var")) {
      state.variableNameDepths.push_back(state.parenthesisDepth);
    }
  } else if (const auto* at = token.tryGet<css::Token::AtKeyword>()) {
    state.mode = CssCursorMode::AtRule;
    state.atRuleName = StringUtils::ToLowerAscii(at->value);
  } else if (const auto* delim = token.tryGet<css::Token::Delim>()) {
    if (state.mode == CssCursorMode::Selector && (delim->value == '.' || delim->value == '#')) {
      state.selectorMarkerStart = token.offset().offset;
    }
  }
}

void AdvanceCssBlock(const css::Token& token, CssCursorState& state) {
  if (token.is<css::Token::CurlyBracket>()) {
    const bool declarations =
        state.mode == CssCursorMode::Selector ||
        (state.mode == CssCursorMode::AtRule && state.atRuleName == "font-face");
    state.blocks.push_back(declarations ? CssCursorBlock::Declarations : CssCursorBlock::RuleList);
    state.mode = declarations ? CssCursorMode::DeclarationName : CssCursorMode::Selector;
    state.atRuleName.clear();
  } else if (token.is<css::Token::CloseCurlyBracket>()) {
    if (!state.blocks.empty()) {
      state.blocks.pop_back();
    }
    state.mode = InDeclarations(state) ? CssCursorMode::DeclarationName : CssCursorMode::Selector;
  }
}

void AdvanceCssFunctionPunctuation(const css::Token& token, CssCursorState& state) {
  if (token.is<css::Token::Comma>() && !state.variableNameDepths.empty() &&
      state.variableNameDepths.back() == state.parenthesisDepth) {
    state.variableNameDepths.pop_back();
  } else if (token.is<css::Token::Parenthesis>()) {
    ++state.parenthesisDepth;
  } else if (token.is<css::Token::CloseParenthesis>()) {
    if (!state.variableNameDepths.empty() &&
        state.variableNameDepths.back() == state.parenthesisDepth) {
      state.variableNameDepths.pop_back();
    }
    state.parenthesisDepth = std::max(0, state.parenthesisDepth - 1);
  }
}

void AdvanceCssPunctuation(const css::Token& token, CssCursorState& state) {
  if (token.is<css::Token::Colon>() && state.mode == CssCursorMode::DeclarationName) {
    state.mode = CssCursorMode::DeclarationValue;
  } else if (token.is<css::Token::Semicolon>() && state.parenthesisDepth == 0) {
    state.mode = InDeclarations(state) ? CssCursorMode::DeclarationName : CssCursorMode::Selector;
    state.propertyName.clear();
    state.atRuleName.clear();
  }
  AdvanceCssFunctionPunctuation(token, state);
}

void AdvanceCssToken(const css::Token& token, CssCursorState& state) {
  AdvanceNamedCssToken(token, state);
  AdvanceCssBlock(token, state);
  AdvanceCssPunctuation(token, state);
  if (token.is<css::Token::Whitespace>()) {
    state.selectorMarkerStart.reset();
  }
}

bool IsLoneSelectorMarker(const css::Token& token, const CssCursorState& state) {
  const auto* delim = token.tryGet<css::Token::Delim>();
  return delim && state.mode == CssCursorMode::Selector &&
         (delim->value == '.' || delim->value == '#');
}

bool IsCssAtRulePrefix(const css::Token& token, const CssCursorState& state) {
  const auto* delim = token.tryGet<css::Token::Delim>();
  return token.is<css::Token::AtKeyword>() ||
         (delim && delim->value == '@' && state.mode == CssCursorMode::Selector);
}

bool IsCssCompletionToken(const css::Token& token, const CssCursorState& state) {
  return token.is<css::Token::Ident>() || token.is<css::Token::Hash>() ||
         IsCssAtRulePrefix(token, state) || IsLoneSelectorMarker(token, state);
}

bool CursorCompletesToken(std::size_t cursorOffset, std::size_t tokenEnd, const css::Token& token,
                          const CssCursorState& state) {
  return cursorOffset < tokenEnd ||
         (cursorOffset == tokenEnd && IsCssCompletionToken(token, state));
}

void RememberSelectorMarker(const css::Token& token, std::size_t start, CssCursorState& state) {
  const auto* delim = token.tryGet<css::Token::Delim>();
  if (delim && state.mode == CssCursorMode::Selector &&
      (delim->value == '.' || delim->value == '#')) {
    state.selectorMarkerStart = start;
  }
}

XmlAutocompleteContext MakeCssTokenContext(const css::Token& token, std::size_t tokenStart,
                                           std::size_t tokenEnd, std::string_view source,
                                           std::size_t cursorOffset, const CssCursorState& state) {
  const auto kind = IsCssAtRulePrefix(token, state) ? XmlAutocompleteContextKind::CssAtRuleName
                                                    : CssContextKind(state);
  std::size_t start = tokenStart;
  if (kind == XmlAutocompleteContextKind::CssSelector && state.selectorMarkerStart) {
    start = *state.selectorMarkerStart;
  }
  return XmlAutocompleteContext{
      .kind = kind,
      .replaceStartOffset = start,
      .replaceEndOffset = tokenEnd,
      .prefix = std::string(source.substr(start, cursorOffset - start)),
      .propertyName = state.propertyName,
  };
}

XmlAutocompleteContext MakeCssEmptyContext(std::size_t cursorOffset, const CssCursorState& state) {
  return XmlAutocompleteContext{
      .kind = CssContextKind(state),
      .replaceStartOffset = cursorOffset,
      .replaceEndOffset = cursorOffset,
      .prefix = {},
      .propertyName = state.propertyName,
  };
}

bool SkipExcessCursorNesting(const css::Token& token, CssCursorState& state) {
  const bool opening = token.is<css::Token::CurlyBracket>() ||
                       token.is<css::Token::Parenthesis>() || token.is<css::Token::Function>();
  constexpr auto limit = css::parser::details::ComponentValueParsingContext::kMaximumNestingDepth;
  if (!state.skippedDepth && !(opening && state.blocks.size() + state.parenthesisDepth >= limit)) {
    return false;
  }
  if (opening) {
    ++state.skippedDepth;
  } else if (token.is<css::Token::CloseCurlyBracket>() ||
             token.is<css::Token::CloseParenthesis>()) {
    --state.skippedDepth;
  }
  return true;
}

std::size_t SelectorReplacementEnd(const css::Token& token, const CssCursorState& state,
                                   css::parser::details::Tokenizer& tokenizer, std::size_t base,
                                   std::size_t end) {
  if (!IsLoneSelectorMarker(token, state) || tokenizer.isEOF()) {
    return end;
  }
  const auto next = tokenizer.next();
  if (next.is<css::Token::Ident>() && next.offset().offset == end - base) {
    return base + tokenizer.endOffset();
  }
  return end;
}

XmlAutocompleteContext ContextWithinCssToken(const css::Token& token, const CssCursorState& state,
                                             css::parser::details::Tokenizer& tokenizer,
                                             const CssSourceRegion& region, std::string_view source,
                                             std::size_t start, std::size_t end,
                                             std::size_t caret) {
  if (caret < start) {
    // The tokenizer skipped a CSS comment containing this caret.
    return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, caret);
  }
  // These tokens already own a functional value. Keyword suggestions here would
  // either insert before it or replace only its opening name and parenthesis.
  if (token.is<css::Token::Url>() || token.is<css::Token::Function>() ||
      token.is<css::Token::BadUrl>()) {
    return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, caret);
  }
  if (token.is<css::Token::Whitespace>() ||
      (!IsCssCompletionToken(token, state) && caret == start)) {
    return MakeCssEmptyContext(caret, state);
  }
  const auto replacementEnd =
      SelectorReplacementEnd(token, state, tokenizer, region.startOffset, end);
  return MakeCssTokenContext(token, start, replacementEnd, source, caret, state);
}

XmlAutocompleteContext DetectCssTokenAtCaret(std::string_view source, std::size_t cursorOffset,
                                             const CssSourceRegion& region) {
  const auto cssText = source.substr(region.startOffset, region.endOffset - region.startOffset);
  css::parser::details::Tokenizer tokenizer(cssText);
  CssCursorState state(region.kind);
  state.propertyName = region.propertyName;
  while (!tokenizer.isEOF()) {
    const css::Token token = tokenizer.next();
    if (token.is<css::Token::EofToken>()) {
      // Comments are skipped before EOF is emitted. The caret can be inside that gap.
      if (cursorOffset < region.startOffset + tokenizer.endOffset()) {
        return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, cursorOffset);
      }
      break;
    }
    const std::size_t start = region.startOffset + token.offset().offset.value_or(0);
    const std::size_t end = region.startOffset + tokenizer.endOffset();
    if (token.is<css::Token::ErrorToken>() || SkipExcessCursorNesting(token, state)) {
      if (cursorOffset <= end) {
        return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, cursorOffset);
      }
      continue;
    }
    if (CursorCompletesToken(cursorOffset, end, token, state)) {
      return ContextWithinCssToken(token, state, tokenizer, region, source, start, end,
                                   cursorOffset);
    }
    AdvanceCssToken(token, state);
    RememberSelectorMarker(token, start, state);
    // At a punctuation boundary the next token can begin directly at the caret.
    // Inspect it so accepting a suggestion replaces the existing word.
  }
  return MakeCssEmptyContext(cursorOffset, state);
}

XmlAutocompleteContext DetectCssInRegion(std::string_view source, std::size_t caret,
                                         const CssSourceRegion& region) {
  auto result = DetectCssTokenAtCaret(source, caret, region);
  if (result.kind == XmlAutocompleteContextKind::CssDeclarationName) {
    css::parser::details::Tokenizer after(
        source.substr(result.replaceEndOffset, region.endOffset - result.replaceEndOffset));
    while (!after.isEOF()) {
      const auto token = after.next();
      if (!token.is<css::Token::Whitespace>()) {
        result.hasFollowingColon = token.is<css::Token::Colon>();
        break;
      }
    }
  }
  return result;
}

// Map raw XML boundaries to the decoded CSS stream. A caret inside the spelling of a
// character reference is editing XML; only its endpoints are valid CSS caret positions.
std::optional<std::size_t> DecodedCaret(const CssTextPiece& piece, std::size_t rawCaret,
                                        std::size_t decodedStart) {
  if (rawCaret == piece.sourceStart) {
    return decodedStart;
  }
  if (rawCaret == piece.sourceEnd) {
    return decodedStart + piece.text.size();
  }
  if (piece.sourceStart < rawCaret && rawCaret < piece.sourceEnd &&
      piece.text.size() == piece.sourceEnd - piece.sourceStart) {
    return decodedStart + rawCaret - piece.sourceStart;
  }
  return std::nullopt;
}

struct ReplacementRange {
  std::optional<std::size_t> start;
  std::size_t end = 0;
  bool contiguous = true;

  void add(const CssTextPiece& piece, std::size_t decodedStart,
           const XmlAutocompleteContext& context) {
    const auto first = std::max(decodedStart, context.replaceStartOffset);
    const auto last = std::min(decodedStart + piece.text.size(), context.replaceEndOffset);
    if (first >= last) {
      return;
    }
    const bool literal = piece.sourceEnd - piece.sourceStart == piece.text.size();
    const auto rawStart = literal ? piece.sourceStart + (first - decodedStart) : piece.sourceStart;
    if (start && end != rawStart) {
      contiguous = false;
    }
    if (!start) {
      start = rawStart;
    }
    end = literal ? piece.sourceStart + (last - decodedStart) : piece.sourceEnd;
  }
};

XmlAutocompleteContext ProjectCssContext(std::string_view source, std::size_t caret,
                                         const CssSourceRegion& region) {
  const auto raw = source.substr(region.startOffset, region.endOffset - region.startOffset);
  if (raw.find_first_of("&<") == raw.npos) {
    return DetectCssInRegion(source, caret, region);
  }
  std::string decoded;
  std::optional<std::size_t> cursor;
  VisitDecodedCssText(raw, [&](const CssTextPiece& piece) {
    if (auto offset = DecodedCaret(piece, caret - region.startOffset, decoded.size())) {
      cursor = offset;
    }
    decoded.append(piece.text);
  });
  if (!cursor) {
    return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, caret);
  }
  auto context =
      DetectCssInRegion(decoded, *cursor, {0, decoded.size(), region.kind, region.propertyName});
  if (context.replaceStartOffset == context.replaceEndOffset) {
    context.replaceStartOffset = context.replaceEndOffset = caret;
    return context;
  }
  ReplacementRange range;
  std::size_t offset = 0;
  VisitDecodedCssText(raw, [&](const CssTextPiece& piece) {
    range.add(piece, offset, context);
    offset += piece.text.size();
  });
  if (!range.start || !range.contiguous) {
    // Replacing a token across XML markup would delete the markup as well.
    return MakeEmptyContext(XmlAutocompleteContextKind::Unknown, caret);
  }
  context.replaceStartOffset = region.startOffset + *range.start;
  context.replaceEndOffset = region.startOffset + range.end;
  return context;
}

std::optional<XmlAutocompleteContext> DetectCssAutocompleteContext(std::string_view source,
                                                                   std::size_t cursorOffset) {
  for (const CssSourceRegion& region : FindSvgCssSourceRegions(source)) {
    if (cursorOffset >= region.startOffset && cursorOffset <= region.endOffset) {
      return ProjectCssContext(source, cursorOffset, region);
    }
  }
  return std::nullopt;
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

    case XMLTokenType::AttributeValue: break;

    case XMLTokenType::Whitespace:
      if (state.inTag && !state.closingTag && !state.currentTagName.empty()) {
        return MakeEmptyContext(XmlAutocompleteContextKind::AttributeName, cursorOffset);
      }
      break;

    case XMLTokenType::TextContent:
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
                             std::string_view prefix, bool alsoPresentationAttribute,
                             bool caseSensitive = false) {
  constexpr std::size_t kMaximumSuggestions = 256;
  const bool matches =
      caseSensitive ? name.starts_with(prefix) : StartsWithInsensitive(name, prefix);
  const bool duplicate =
      std::ranges::any_of(suggestions, [name](const XmlAutocompleteSuggestion& suggestion) {
        return suggestion.displayText == name;
      });
  if (!matches || duplicate || suggestions.size() == kMaximumSuggestions) {
    return;
  }

  suggestions.push_back(XmlAutocompleteSuggestion{
      .displayText = std::string(name),
      .insertText = std::string(insertText),
      .alsoPresentationAttribute = alsoPresentationAttribute,
  });
}

void AddElementSuggestions(const XmlAutocompleteContext& context,
                           std::vector<XmlAutocompleteSuggestion>& suggestions) {
  for (std::string_view name : svg::kSVGElementNames) {
    AddSuggestionIfMatching(suggestions, name, name, context.prefix, false);
  }
}

void AddAttributeSuggestions(const XmlAutocompleteContext& context,
                             std::vector<XmlAutocompleteSuggestion>& suggestions) {
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
    AddSuggestionIfMatching(suggestions, name, name, context.prefix,
                            svg::PropertyRegistry::isPresentationAttributeName(name));
  }
}

// CSS hex escapes for XML markup characters work in both normal text and CDATA.
std::string SerializeSvgIdentifier(std::string_view name) {
  const std::string ident = css::SerializeIdentifier(name);
  std::string result;
  for (std::size_t i = 0; i < ident.size(); ++i) {
    if (ident[i] != '\\' || i + 1 == ident.size()) {
      result += ident[i];
      continue;
    }
    const char escaped = ident[++i];
    switch (escaped) {
      case '"': result += "\\22 "; break;
      case '\'': result += "\\27 "; break;
      case '&': result += "\\26 "; break;
      case '<': result += "\\3c "; break;
      case '>': result += "\\3e "; break;
      default:
        result += '\\';
        result += escaped;
        break;
    }
  }
  return result;
}

void AddSelectorSuggestions(const XmlAutocompleteContext& context, const XmlAutocompleteData& data,
                            std::vector<XmlAutocompleteSuggestion>& suggestions) {
  AddElementSuggestions(context, suggestions);
  const auto add = [&](char marker, const std::string& name) {
    const std::string display = marker + name;
    const std::string insert = marker + SerializeSvgIdentifier(name);
    if (display.starts_with(context.prefix) || insert.starts_with(context.prefix)) {
      AddSuggestionIfMatching(suggestions, display, insert, {}, false, true);
    }
  };
  for (const auto& name : data.classNames) {
    add('.', name);
  }
  for (const auto& name : data.ids) {
    add('#', name);
  }
}

void AddDeclarationNameSuggestions(const XmlAutocompleteContext& context,
                                   const XmlAutocompleteData& data,
                                   std::vector<XmlAutocompleteSuggestion>& suggestions) {
  const char* suffix = context.hasFollowingColon ? "" : ": ";
  for (std::string_view name : svg::PropertyRegistry::propertyNames()) {
    const std::string insertText = std::string(name) + suffix;
    AddSuggestionIfMatching(suggestions, name, insertText, context.prefix,
                            svg::PropertyRegistry::isPresentationAttributeName(name));
  }
  for (const std::string& name : data.customPropertyNames) {
    const auto serialized = SerializeSvgIdentifier(name);
    if (name.starts_with(context.prefix) || serialized.starts_with(context.prefix)) {
      AddSuggestionIfMatching(suggestions, name, serialized + suffix, {}, false, true);
    }
  }
}

void AddValueSuggestions(const XmlAutocompleteContext& context, const XmlAutocompleteData& data,
                         std::vector<XmlAutocompleteSuggestion>& suggestions) {
  std::span<const std::string_view> keywords = data.propertyKeywords;
  if (keywords.empty()) {
    const std::string name = std::string_view(context.propertyName).starts_with("--")
                                 ? context.propertyName
                                 : StringUtils::ToLowerAscii(context.propertyName);
    keywords = svg::PropertyRegistry::keywordValues(name);
  }
  for (std::string_view keyword : keywords) {
    AddSuggestionIfMatching(suggestions, keyword, keyword, context.prefix, false);
  }
}

void AddVariableSuggestions(const XmlAutocompleteContext& context, const XmlAutocompleteData& data,
                            std::vector<XmlAutocompleteSuggestion>& suggestions) {
  for (const std::string& name : data.customPropertyNames) {
    const auto serialized = SerializeSvgIdentifier(name);
    if (name.starts_with(context.prefix) || serialized.starts_with(context.prefix)) {
      AddSuggestionIfMatching(suggestions, name, serialized, {}, false, true);
    }
  }
}

void AddAtRuleSuggestions(const XmlAutocompleteContext& context,
                          std::vector<XmlAutocompleteSuggestion>& suggestions) {
  for (std::string_view name : {"@import", "@namespace", "@font-face", "@layer"}) {
    AddSuggestionIfMatching(suggestions, name, name, context.prefix, false);
  }
}

}  // namespace

std::size_t XmlAutocompleteSelectorCatalog::admitBytes(std::size_t requested) {
  const auto accepted = std::min(requested, limits_.scanBytes - inspectedBytes_);
  inspectedBytes_ += accepted;
  return accepted;
}

bool XmlAutocompleteSelectorCatalog::addUnique(std::vector<std::string>& destination,
                                               NameIndex& index, std::string_view value) {
  if (full_ || inspectedTokens_ == limits_.scanTokens) {
    full_ = true;
    return false;
  }
  if (value.empty()) {
    return true;
  }
  full_ = ++inspectedTokens_ == limits_.scanTokens;
  const auto hash = std::hash<std::string_view>{}(value);
  const auto [first, last] = index.equal_range(hash);
  for (auto it = first; it != last; ++it) {
    if (destination[it->second] == value) {
      return !full_;
    }
  }
  const auto count = classNames_.size() + ids_.size();
  if (count >= limits_.values || value.size() > limits_.bytes - retainedBytes_) {
    full_ = true;
    return false;
  }
  index.emplace(hash, destination.size());
  destination.emplace_back(value);
  retainedBytes_ += value.size();
  full_ |= count + 1 == limits_.values || retainedBytes_ == limits_.bytes;
  return !full_;
}

void XmlAutocompleteSelectorCatalog::addClassList(std::string_view classList) {
  if (full_) {
    return;
  }
  const auto end = admitBytes(classList.size());
  for (std::size_t offset = 0; offset < end && !full_;) {
    while (offset < end && StringUtils::IsAsciiWhitespace(classList[offset])) {
      ++offset;
    }
    const std::size_t start = offset;
    while (offset < end && !StringUtils::IsAsciiWhitespace(classList[offset])) {
      ++offset;
    }
    if (offset == end && end < classList.size() &&
        !StringUtils::IsAsciiWhitespace(classList[end])) {
      break;  // The inspected prefix ends inside this class name.
    }
    addUnique(classNames_, classIndex_, classList.substr(start, offset - start));
  }
  full_ |= inspectedBytes_ == limits_.scanBytes;
}

void XmlAutocompleteSelectorCatalog::addId(std::string_view id) {
  if (!full_ && admitBytes(id.size()) == id.size()) {
    addUnique(ids_, idIndex_, id);
  }
  full_ |= inspectedBytes_ == limits_.scanBytes;
}

void XmlAutocompleteSelectorCatalog::sortAndIndex(std::vector<std::string>& names,
                                                  NameIndex& index) {
  std::ranges::sort(names);
  index.clear();
  for (std::size_t i = 0; i < names.size(); ++i) {
    index.emplace(std::hash<std::string_view>{}(names[i]), i);
  }
}

void XmlAutocompleteSelectorCatalog::finish() {
  sortAndIndex(classNames_, classIndex_);
  sortAndIndex(ids_, idIndex_);
}

XmlAutocompleteContext DetectXmlAutocompleteContext(std::string_view source,
                                                    std::size_t cursorOffset) {
  cursorOffset = std::min(cursorOffset, source.size());

  if (auto css = DetectCssAutocompleteContext(source, cursorOffset)) {
    return std::move(*css);
  }

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
    const XmlAutocompleteContext& context, const XmlAutocompleteData& data) {
  std::vector<XmlAutocompleteSuggestion> suggestions;

  switch (context.kind) {
    case XmlAutocompleteContextKind::ElementName:
      AddElementSuggestions(context, suggestions);
      break;
    case XmlAutocompleteContextKind::AttributeName:
      AddAttributeSuggestions(context, suggestions);
      break;
    case XmlAutocompleteContextKind::CssSelector:
      AddSelectorSuggestions(context, data, suggestions);
      break;
    case XmlAutocompleteContextKind::CssDeclarationName:
      AddDeclarationNameSuggestions(context, data, suggestions);
      break;
    case XmlAutocompleteContextKind::CssDeclarationValue:
      AddValueSuggestions(context, data, suggestions);
      break;
    case XmlAutocompleteContextKind::CssVariableName:
      AddVariableSuggestions(context, data, suggestions);
      break;
    case XmlAutocompleteContextKind::CssAtRuleName:
      AddAtRuleSuggestions(context, suggestions);
      break;

    case XmlAutocompleteContextKind::TextContent:
    case XmlAutocompleteContextKind::Unknown: break;
  }

  return suggestions;
}

}  // namespace donner::editor
