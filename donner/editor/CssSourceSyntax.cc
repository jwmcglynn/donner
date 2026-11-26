#include "donner/editor/CssSourceSyntax.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "donner/base/StringUtils.h"
#include "donner/base/xml/XMLTokenizer.h"
#include "donner/css/parser/details/Tokenizer.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {
namespace {

using css::Token;
using xml::XMLToken;
using xml::XMLTokenType;

std::size_t TokenStart(const XMLToken& token) {
  return token.range.start.offset.value_or(0);
}

std::size_t TokenEnd(const XMLToken& token) {
  return token.range.end.offset.value_or(TokenStart(token));
}

struct XmlRegionState {
  bool inTag = false;
  bool closingTag = false;
  std::string tagName;
  std::string attributeName;
  std::vector<std::string> elementStack;
  std::vector<std::size_t> styleStarts;
};

void PopElement(std::vector<std::string>& stack, std::string_view name) {
  // Well-formed XML closes the top element. Keep recovery from an incomplete edit bounded.
  constexpr std::size_t kRecoveryDepth = 32;
  const auto limit = stack.rbegin() + std::min(stack.size(), kRecoveryDepth);
  const auto found = std::find(stack.rbegin(), limit, name);
  if (found != limit) {
    stack.erase(found.base() - 1, stack.end());
  }
}

std::optional<std::pair<std::size_t, std::size_t>> QuotedValueRange(const XMLToken& token,
                                                                    std::string_view source) {
  const std::size_t start = TokenStart(token);
  const std::size_t end = TokenEnd(token);
  if (end <= start || end > source.size()) {
    return std::nullopt;
  }
  const char quote = source[start];
  if (quote != '\'' && quote != '"') {
    return std::nullopt;
  }
  const bool closed = end > start + 1 && source[end - 1] == quote;
  return std::pair(start + 1, closed ? end - 1 : end);
}

void AddAttributeRegion(const XMLToken& token, std::string_view source, const XmlRegionState& state,
                        std::vector<CssSourceRegion>& regions) {
  const auto range = QuotedValueRange(token, source);
  if (!range) {
    return;
  }
  if (state.attributeName == "style") {
    regions.push_back({range->first, range->second, CssSourceRegionKind::DeclarationList, {}});
  } else if (svg::PropertyRegistry::isPresentationAttributeName(state.attributeName)) {
    regions.push_back(
        {range->first, range->second, CssSourceRegionKind::PropertyValue, state.attributeName});
  }
}

void CloseStyleRegion(std::size_t end, XmlRegionState& state,
                      std::vector<CssSourceRegion>& regions) {
  if (state.elementStack.empty() || state.elementStack.back() != "style" ||
      state.styleStarts.empty()) {
    return;
  }
  regions.push_back({state.styleStarts.back(), end, CssSourceRegionKind::Stylesheet, {}});
  state.styleStarts.pop_back();
}

void BeginTag(const XMLToken& token, XmlRegionState& state, std::vector<CssSourceRegion>& regions) {
  const bool closing = TokenEnd(token) - TokenStart(token) == 2;
  if (closing) {
    CloseStyleRegion(TokenStart(token), state, regions);
  }
  state.inTag = true;
  state.closingTag = closing;
  state.tagName.clear();
  state.attributeName.clear();
}

void FinishTag(const XMLToken& token, XmlRegionState& state) {
  if (state.closingTag) {
    PopElement(state.elementStack, state.tagName);
  } else if (!state.tagName.empty()) {
    state.elementStack.push_back(state.tagName);
    if (state.tagName == "style") {
      state.styleStarts.push_back(TokenEnd(token));
    }
  }
  state.inTag = false;
  state.closingTag = false;
  state.tagName.clear();
  state.attributeName.clear();
}

void FinishSelfClosingTag(XmlRegionState& state) {
  state.inTag = false;
  state.closingTag = false;
  state.tagName.clear();
  state.attributeName.clear();
}

void ApplyTagName(const XMLToken& token, std::string_view source, XmlRegionState& state) {
  if (state.inTag) {
    state.tagName = std::string(token.text(source));
  }
}

void ApplyAttributeName(const XMLToken& token, std::string_view source, XmlRegionState& state) {
  if (state.inTag && !state.closingTag) {
    state.attributeName = std::string(token.text(source));
  }
}

void ApplyAttributeValue(const XMLToken& token, std::string_view source, XmlRegionState& state,
                         std::vector<CssSourceRegion>& regions) {
  if (state.inTag && !state.closingTag) {
    AddAttributeRegion(token, source, state, regions);
    state.attributeName.clear();
  }
}

void ApplyMarkupToken(const XMLToken& token, std::string_view source, XmlRegionState& state,
                      std::vector<CssSourceRegion>& regions) {
  switch (token.type) {
    case XMLTokenType::TagOpen: BeginTag(token, state, regions); break;
    case XMLTokenType::TagName: ApplyTagName(token, source, state); break;
    case XMLTokenType::AttributeName: ApplyAttributeName(token, source, state); break;
    case XMLTokenType::AttributeValue: ApplyAttributeValue(token, source, state, regions); break;
    case XMLTokenType::TagClose: FinishTag(token, state); break;
    case XMLTokenType::TagSelfClose: FinishSelfClosingTag(state); break;
    default: break;
  }
}

std::vector<CssSourceRegion> FindRegions(std::string_view source) {
  std::vector<CssSourceRegion> regions;
  XmlRegionState state;
  xml::Tokenize(source, [&](XMLToken token) { ApplyMarkupToken(token, source, state, regions); });
  while (!state.styleStarts.empty()) {
    regions.push_back(
        {state.styleStarts.back(), source.size(), CssSourceRegionKind::Stylesheet, {}});
    state.styleStarts.pop_back();
  }
  std::ranges::sort(regions, {}, &CssSourceRegion::startOffset);
  return regions;
}

bool IsKnownProperty(std::string_view name) {
  const auto names = svg::PropertyRegistry::propertyNames();
  return std::ranges::find(names, name) != names.end();
}

void AppendColor(std::vector<CssColorSpan>& colors, std::size_t start, std::size_t end,
                 ColorIndex color) {
  if (start >= end) {
    return;
  }
  if (!colors.empty() && colors.back().endOffset == start && colors.back().color == color) {
    colors.back().endOffset = end;
  } else {
    colors.push_back({start, end, color});
  }
}

enum class CssMode { SelectorPrelude, AtRulePrelude, DeclarationName, DeclarationValue };
enum class BlockKind { RuleList, Declarations };

struct CssState {
  explicit CssState(CssSourceRegionKind kind)
      : mode(kind == CssSourceRegionKind::Stylesheet ? CssMode::SelectorPrelude
                                                     : CssMode::DeclarationName),
        rootDeclarationList(kind == CssSourceRegionKind::DeclarationList) {
    if (kind == CssSourceRegionKind::PropertyValue) {
      mode = CssMode::DeclarationValue;
    }
  }

  CssMode mode;
  bool rootDeclarationList = false;
  std::vector<BlockKind> blocks;
  std::string atRuleName;
  bool classNameNext = false;
  int parenthesisDepth = 0;
  int stringFunctionDepth = -1;
};

bool InDeclarationList(const CssState& state) {
  return !state.blocks.empty() ? state.blocks.back() == BlockKind::Declarations
                               : state.rootDeclarationList;
}

ColorIndex IdentColor(const Token::Ident& ident, CssState& state) {
  const std::string_view name = ident.value;
  if (state.classNameNext) {
    state.classNameNext = false;
    return ColorIndex::Identifier;
  }
  switch (state.mode) {
    case CssMode::SelectorPrelude: return ColorIndex::Keyword;
    case CssMode::AtRulePrelude: return ColorIndex::Identifier;
    case CssMode::DeclarationName:
      if (name.starts_with("--")) {
        return ColorIndex::Identifier;
      }
      return IsKnownProperty(name) ? ColorIndex::KnownIdentifier : ColorIndex::ErrorMarker;
    case CssMode::DeclarationValue:
      return name.starts_with("--") ? ColorIndex::Identifier : ColorIndex::Keyword;
  }
  return ColorIndex::Default;
}

void OpenBlock(CssState& state) {
  const bool declarations =
      state.mode == CssMode::SelectorPrelude ||
      (state.mode == CssMode::AtRulePrelude && state.atRuleName == "font-face");
  state.blocks.push_back(declarations ? BlockKind::Declarations : BlockKind::RuleList);
  state.mode = declarations ? CssMode::DeclarationName : CssMode::SelectorPrelude;
  state.atRuleName.clear();
}

void CloseBlock(CssState& state) {
  if (!state.blocks.empty()) {
    state.blocks.pop_back();
  }
  state.mode = InDeclarationList(state) ? CssMode::DeclarationName : CssMode::SelectorPrelude;
}

void EndDeclaration(CssState& state) {
  state.mode = InDeclarationList(state) ? CssMode::DeclarationName : CssMode::SelectorPrelude;
  state.atRuleName.clear();
}

ColorIndex StructuralColor(const Token& token, CssState& state) {
  if (token.is<Token::CurlyBracket>()) {
    OpenBlock(state);
  } else if (token.is<Token::CloseCurlyBracket>()) {
    CloseBlock(state);
  } else if (token.is<Token::Colon>() && state.mode == CssMode::DeclarationName) {
    state.mode = CssMode::DeclarationValue;
  } else if (token.is<Token::Semicolon>() && state.parenthesisDepth == 0) {
    EndDeclaration(state);
  }
  return ColorIndex::Punctuation;
}

std::optional<ColorIndex> NestedStringColor(const Token& token, CssState& state) {
  if (state.stringFunctionDepth < 0) {
    return std::nullopt;
  }
  if (token.is<Token::Parenthesis>()) {
    ++state.parenthesisDepth;
  } else if (token.is<Token::CloseParenthesis>()) {
    --state.parenthesisDepth;
    if (state.parenthesisDepth < state.stringFunctionDepth) {
      state.stringFunctionDepth = -1;
    }
  }
  return ColorIndex::String;
}

std::optional<ColorIndex> NamedTokenColor(const Token& token, CssState& state) {
  if (const auto* ident = token.tryGet<Token::Ident>()) {
    return IdentColor(*ident, state);
  }
  if (const auto* function = token.tryGet<Token::Function>()) {
    ++state.parenthesisDepth;
    if (function->name.equalsLowercase("url")) {
      state.stringFunctionDepth = state.parenthesisDepth;
      return ColorIndex::String;
    }
    return ColorIndex::Keyword;
  }
  if (const auto* at = token.tryGet<Token::AtKeyword>()) {
    state.mode = CssMode::AtRulePrelude;
    state.atRuleName = StringUtils::ToLowerAscii(at->value);
    return ColorIndex::Keyword;
  }
  if (token.is<Token::Hash>()) {
    return ColorIndex::Identifier;
  }
  if (const auto* delim = token.tryGet<Token::Delim>()) {
    state.classNameNext = state.mode == CssMode::SelectorPrelude && delim->value == '.';
    return ColorIndex::Punctuation;
  }
  return std::nullopt;
}

std::optional<ColorIndex> LiteralTokenColor(const Token& token) {
  if (token.is<Token::Number>() || token.is<Token::Percentage>() || token.is<Token::Dimension>()) {
    return ColorIndex::Number;
  }
  if (token.is<Token::String>() || token.is<Token::Url>()) {
    return ColorIndex::String;
  }
  if (token.is<Token::BadString>() || token.is<Token::BadUrl>() || token.is<Token::ErrorToken>()) {
    return ColorIndex::ErrorMarker;
  }
  if (token.is<Token::Whitespace>()) {
    return ColorIndex::Default;
  }
  return std::nullopt;
}

ColorIndex TokenColor(const Token& token, CssState& state) {
  if (const auto color = NestedStringColor(token, state)) {
    return *color;
  }
  if (const auto color = NamedTokenColor(token, state)) {
    return *color;
  }
  if (const auto color = LiteralTokenColor(token)) {
    return *color;
  }
  if (token.is<Token::Parenthesis>()) {
    ++state.parenthesisDepth;
  } else if (token.is<Token::CloseParenthesis>()) {
    state.parenthesisDepth = std::max(0, state.parenthesisDepth - 1);
  }
  return StructuralColor(token, state);
}

}  // namespace

std::vector<CssSourceRegion> FindSvgCssSourceRegions(std::string_view source) {
  return FindRegions(source);
}

void ClassifySvgCssRegion(std::string_view source, const CssSourceRegion& region,
                          std::vector<CssColorSpan>& colors) {
  const std::string_view cssText =
      source.substr(region.startOffset, region.endOffset - region.startOffset);
  css::parser::details::Tokenizer tokenizer(cssText);
  CssState state(region.kind);
  std::size_t consumed = 0;
  while (!tokenizer.isEOF()) {
    const Token token = tokenizer.next();
    if (token.is<Token::EofToken>()) {
      break;
    }
    const std::size_t start = token.offset().offset.value_or(consumed);
    const std::size_t end = tokenizer.endOffset();
    AppendColor(colors, region.startOffset + consumed, region.startOffset + start,
                ColorIndex::Comment);
    AppendColor(colors, region.startOffset + start, region.startOffset + end,
                TokenColor(token, state));
    consumed = end;
  }
  AppendColor(colors, region.startOffset + consumed, region.endOffset, ColorIndex::Comment);
}

}  // namespace donner::editor
