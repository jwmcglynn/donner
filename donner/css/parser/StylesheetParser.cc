#include "donner/css/parser/StylesheetParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "donner/base/StringUtils.h"
#include "donner/base/Utils.h"
#include "donner/base/encoding/Base64.h"
#include "donner/base/parser/DataUrlParser.h"
#include "donner/css/FontFace.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/SelectorParser.h"

#if defined(__clang__) && defined(__APPLE__) && defined(__OPTIMIZE__)
#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer) && \
    !__has_feature(undefined_behavior_sanitizer)
#define DONNER_APPLE_LAYER_PARSER_MINSIZE 1
#endif
#endif

namespace donner::css::parser {

namespace {

std::optional<std::size_t> ComponentSourceOffset(const ComponentValue& component) {
  if (const Token* token = std::get_if<Token>(&component.value)) {
    return token->offset().offset;
  } else if (const Function* function = std::get_if<Function>(&component.value)) {
    return function->sourceOffset.offset;
  } else if (const SimpleBlock* block = std::get_if<SimpleBlock>(&component.value)) {
    return block->sourceOffset.offset;
  }

  return std::nullopt;
}

/// CSS whitespace: space, tab and newline, where carriage return and form feed are newlines after
/// input preprocessing (https://www.w3.org/TR/css-syntax-3/#whitespace). Vertical tab is not
/// whitespace.
bool IsCssWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

std::size_t TrimStart(std::string_view str, std::size_t start, std::size_t end) {
  while (start < end && IsCssWhitespace(str[start])) {
    ++start;
  }
  return start;
}

std::size_t TrimEnd(std::string_view str, std::size_t start, std::size_t end) {
  while (end > start && IsCssWhitespace(str[end - 1])) {
    --end;
  }
  return end;
}

SourceRange MakeSourceRange(std::size_t start, std::size_t end) {
  return SourceRange{.start = FileOffset::Offset(start), .end = FileOffset::Offset(end)};
}

std::size_t FindMatchingRuleEnd(std::string_view str, std::size_t openBraceOffset) {
  if (openBraceOffset >= str.size() || str[openBraceOffset] != '{') {
    return openBraceOffset;
  }

  int curlyDepth = 0;
  char quote = '\0';
  bool inComment = false;
  for (std::size_t i = openBraceOffset; i < str.size(); ++i) {
    const char ch = str[i];
    const char next = i + 1 < str.size() ? str[i + 1] : '\0';

    if (inComment) {
      if (ch == '*' && next == '/') {
        inComment = false;
        ++i;
      }
      continue;
    }

    if (quote != '\0') {
      if (ch == '\\' && next != '\0') {
        ++i;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }

    if (ch == '/' && next == '*') {
      inComment = true;
      ++i;
    } else if (ch == '\'' || ch == '"') {
      quote = ch;
    } else if (ch == '{') {
      ++curlyDepth;
    } else if (ch == '}') {
      --curlyDepth;
      if (curlyDepth == 0) {
        return i + 1;
      }
    }
  }

  return str.size();
}

std::vector<SourceRange> SplitSelectorEntryRanges(std::string_view str, std::size_t start,
                                                  std::size_t end, std::size_t expectedEntryCount) {
  std::vector<SourceRange> result;
  std::size_t entryStart = start;
  int parenDepth = 0;
  int squareDepth = 0;
  char quote = '\0';
  bool inComment = false;

  for (std::size_t i = start; i < end; ++i) {
    const char ch = str[i];
    const char next = i + 1 < str.size() ? str[i + 1] : '\0';

    if (inComment) {
      if (ch == '*' && next == '/') {
        inComment = false;
        ++i;
      }
      continue;
    }

    if (quote != '\0') {
      if (ch == '\\' && next != '\0') {
        ++i;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }

    if (ch == '/' && next == '*') {
      inComment = true;
      ++i;
    } else if (ch == '\'' || ch == '"') {
      quote = ch;
    } else if (ch == '(') {
      ++parenDepth;
    } else if (ch == ')' && parenDepth > 0) {
      --parenDepth;
    } else if (ch == '[') {
      ++squareDepth;
    } else if (ch == ']' && squareDepth > 0) {
      --squareDepth;
    } else if (ch == ',' && parenDepth == 0 && squareDepth == 0) {
      const std::size_t trimmedStart = TrimStart(str, entryStart, i);
      const std::size_t trimmedEnd = TrimEnd(str, trimmedStart, i);
      result.push_back(MakeSourceRange(trimmedStart, trimmedEnd));
      entryStart = i + 1;
    }
  }

  const std::size_t trimmedStart = TrimStart(str, entryStart, end);
  const std::size_t trimmedEnd = TrimEnd(str, trimmedStart, end);
  result.push_back(MakeSourceRange(trimmedStart, trimmedEnd));

  if (result.size() != expectedEntryCount) {
    result.assign(expectedEntryCount, MakeSourceRange(start, end));
  }

  return result;
}

void PopulateSelectorRuleSourceRanges(std::string_view str, const QualifiedRule& qualifiedRule,
                                      SelectorRule* selectorRule) {
  std::optional<std::size_t> blockStart = qualifiedRule.block.sourceOffset.offset;
  if (!blockStart.has_value()) {
    return;
  }

  std::size_t selectorStart = *blockStart;
  for (const ComponentValue& component : qualifiedRule.prelude) {
    if (std::optional<std::size_t> offset = ComponentSourceOffset(component)) {
      selectorStart = std::min(selectorStart, *offset);
    }
  }

  selectorStart = TrimStart(str, selectorStart, *blockStart);
  const std::size_t selectorEnd = TrimEnd(str, selectorStart, *blockStart);
  const std::size_t ruleEnd = FindMatchingRuleEnd(str, *blockStart);

  selectorRule->selectorSourceRange = MakeSourceRange(selectorStart, selectorEnd);
  selectorRule->ruleSourceRange = MakeSourceRange(selectorStart, ruleEnd);
  selectorRule->selectorEntrySourceRanges = SplitSelectorEntryRanges(
      str, selectorStart, selectorEnd, selectorRule->selector.entries.size());
}

/**
 * Try to parse a `url()` function into either a data URL or an external URL.
 *
 * @param url The URL to parse.
 * @return \ref FontFaceSource if the URL is a data URL, otherwise \c std::nullopt.
 */
std::optional<FontFaceSource> TryParseFontFaceSourceFromUrl(std::string_view url) {
  using donner::parser::DataUrlParser;
  using donner::parser::DataUrlParserError;

  if (url.empty()) {
    return std::nullopt;
  }

  std::variant<DataUrlParser::Result, DataUrlParserError> maybeParsedUrl =
      DataUrlParser::Parse(url);

  if (std::holds_alternative<DataUrlParserError>(maybeParsedUrl)) {
    return std::nullopt;
  }

  DataUrlParser::Result& parsedUrl = std::get<DataUrlParser::Result>(maybeParsedUrl);

  if (parsedUrl.kind == DataUrlParser::Result::Kind::Data) {
    FontFaceSource source;
    source.kind = FontFaceSource::Kind::Data;
    source.payload = std::make_shared<const std::vector<uint8_t>>(
        std::move(std::get<std::vector<uint8_t>>(parsedUrl.payload)));
    source.formatHint = parsedUrl.mimeType;
    return source;
  } else if (parsedUrl.kind == DataUrlParser::Result::Kind::ExternalUrl) {
    FontFaceSource source;
    source.kind = FontFaceSource::Kind::Url;
    source.payload = std::get<RcString>(parsedUrl.payload);

    return source;
  }

  return std::nullopt;
}

bool ChargeDeclarations(Stylesheet::SecurityStats& stylesheetStats,
                        const DeclarationListParser::SecurityStats& declarationStats,
                        std::size_t declarationCount) {
  stylesheetStats.rejected |= declarationStats.rejected;
  if (declarationCount > Stylesheet::kMaximumDeclarations - stylesheetStats.declarations) {
    stylesheetStats.rejected = true;
    return false;
  }
  stylesheetStats.declarations += declarationCount;
  return true;
}

std::optional<RcString> FirstFunctionString(const Function& function) {
  if (function.values.empty()) {
    return std::nullopt;
  }
  const Token* token = std::get_if<Token>(&function.values.front().value);
  if (!token) {
    return std::nullopt;
  }
  if (token->is<Token::Ident>()) {
    return token->get<Token::Ident>().value;
  }
  if (token->is<Token::String>()) {
    return token->get<Token::String>().value;
  }
  if (token->is<Token::Url>()) {
    return token->get<Token::Url>().value;
  }
  return std::nullopt;
}

void ApplyFontFaceHints(std::span<const ComponentValue> items, FontFaceSource& source) {
  for (const ComponentValue& component : items) {
    const Function* function = std::get_if<Function>(&component.value);
    if (!function) {
      continue;
    }
    if (function->name.equalsLowercase("format")) {
      if (const auto hint = FirstFunctionString(*function)) {
        source.formatHint = *hint;
      }
      continue;
    }
    if (!function->name.equalsLowercase("tech")) {
      continue;
    }
    for (const ComponentValue& value : function->values) {
      if (const Token* token = std::get_if<Token>(&value.value)) {
        if (token->is<Token::Ident>()) {
          source.techHints.push_back(token->get<Token::Ident>().value);
        } else if (token->is<Token::String>()) {
          source.techHints.push_back(token->get<Token::String>().value);
        }
      }
    }
  }
}

std::optional<FontFaceSource> ParseFontFaceSourceItem(std::span<const ComponentValue> items) {
  if (items.empty()) {
    return std::nullopt;
  }

  std::optional<FontFaceSource> source;
  if (const Function* function = std::get_if<Function>(&items.front().value)) {
    const auto value = FirstFunctionString(*function);
    if (value && function->name.equalsLowercase("local")) {
      source = FontFaceSource{FontFaceSource::Kind::Local, *value, "", {}};
    } else if (value && function->name.equalsLowercase("url")) {
      source = TryParseFontFaceSourceFromUrl(*value);
    }
  } else if (const Token* token = std::get_if<Token>(&items.front().value);
             token && token->is<Token::Url>()) {
    source = TryParseFontFaceSourceFromUrl(token->get<Token::Url>().value);
  }

  if (source) {
    ApplyFontFaceHints(items.subspan(1), *source);
  }
  return source;
}

void AppendFontFaceSources(std::span<const ComponentValue> values, FontFace& fontFace) {
  std::size_t itemStart = 0;
  for (std::size_t i = 0; i <= values.size(); ++i) {
    const bool atEnd = i == values.size();
    const Token* token = atEnd ? nullptr : std::get_if<Token>(&values[i].value);
    if (!atEnd && (!token || !token->is<Token::Comma>())) {
      continue;
    }
    if (auto source = ParseFontFaceSourceItem(values.subspan(itemStart, i - itemStart))) {
      fontFace.sources.push_back(std::move(*source));
    }
    itemStart = i + 1;
  }
}

std::optional<int> ParseFontFaceWeight(const ComponentValue& value) {
  if (const auto* ident = value.tryGetToken<Token::Ident>()) {
    if (ident->value.equalsLowercase("normal")) {
      return 400;
    }
    if (ident->value.equalsLowercase("bold")) {
      return 700;
    }
  } else if (const auto* number = value.tryGetToken<Token::Number>()) {
    if (number->value >= 1 && number->value <= 1000 && std::trunc(number->value) == number->value) {
      return static_cast<int>(number->value);
    }
  }
  return std::nullopt;
}

template <std::size_t N>
std::optional<int> ParseFontFaceKeyword(const ComponentValue& value,
                                        const std::array<std::string_view, N>& keywords,
                                        int firstValue) {
  if (const auto* ident = value.tryGetToken<Token::Ident>()) {
    for (std::size_t i = 0; i < keywords.size(); ++i) {
      if (ident->value.equalsLowercase(keywords[i])) {
        return firstValue + static_cast<int>(i);
      }
    }
  }
  return std::nullopt;
}

void ApplyFontFaceWeight(std::span<const ComponentValue> values, FontFace& fontFace) {
  std::array<const ComponentValue*, 2> endpoints{};
  std::size_t count = 0;
  for (const ComponentValue& value : values) {
    if (value.isToken<Token::Whitespace>()) {
      continue;
    }
    if (count == endpoints.size()) {
      return;
    }
    endpoints[count++] = &value;
  }
  if (count == 0) {
    return;
  }
  const auto first = ParseFontFaceWeight(*endpoints.front());
  const auto last = ParseFontFaceWeight(*endpoints[count - 1]);
  if (!first || !last) {
    return;
  }
  fontFace.fontWeight = std::min(*first, *last);
  fontFace.fontWeightMaximum = count == 2 ? std::optional(std::max(*first, *last)) : std::nullopt;
}

struct ParsedFontFaceWidth {
  double percentage;
  std::optional<int> keyword;
};

std::optional<ParsedFontFaceWidth> ParseFontFaceWidth(const ComponentValue& value) {
  if (const auto* percentage = value.tryGetToken<Token::Percentage>()) {
    if (std::isfinite(percentage->value) && percentage->value >= 0) {
      return ParsedFontFaceWidth{percentage->value, std::nullopt};
    }
  }
  constexpr std::array<std::string_view, 9> kWidths = {
      "ultra-condensed", "extra-condensed", "condensed",      "semi-condensed", "normal",
      "semi-expanded",   "expanded",        "extra-expanded", "ultra-expanded"};
  if (const auto keyword = ParseFontFaceKeyword(value, kWidths, 1)) {
    return ParsedFontFaceWidth{FontFace::FontStretchPercentage(*keyword), *keyword};
  }
  return std::nullopt;
}

void ApplyFontFaceWidth(std::span<const ComponentValue> values, FontFace& fontFace) {
  std::array<const ComponentValue*, 2> endpoints{};
  std::size_t count = 0;
  for (const ComponentValue& value : values) {
    if (value.isToken<Token::Whitespace>()) {
      continue;
    }
    if (count == endpoints.size()) {
      return;
    }
    endpoints[count++] = &value;
  }
  if (count == 0) {
    return;
  }
  const auto first = ParseFontFaceWidth(*endpoints.front());
  const auto last = ParseFontFaceWidth(*endpoints[count - 1]);
  if (!first || !last) {
    return;
  }
  fontFace.fontStretch = first->keyword.value_or(5);
  fontFace.fontStretchPercentRange =
      count == 1 && first->keyword
          ? std::nullopt
          : std::optional(std::pair{std::min(first->percentage, last->percentage),
                                    std::max(first->percentage, last->percentage)});
}

void ApplyFontFaceMatchingDescriptor(FontFaceDescriptor descriptor, const Declaration& declaration,
                                     FontFace& fontFace) {
  if (declaration.important) {
    return;
  }
  if (descriptor == FontFaceDescriptor::Weight) {
    ApplyFontFaceWeight(declaration.values, fontFace);
    return;
  }
  if (descriptor == FontFaceDescriptor::Width) {
    ApplyFontFaceWidth(declaration.values, fontFace);
    return;
  }
  if (declaration.values.size() != 1) {
    return;
  }
  const ComponentValue& value = declaration.values.front();
  if (descriptor == FontFaceDescriptor::Style) {
    constexpr std::array<std::string_view, 3> kStyles = {"normal", "italic", "oblique"};
    if (const auto style = ParseFontFaceKeyword(value, kStyles, 0)) {
      fontFace.fontStyle = *style;
    }
  }
}

std::optional<FontFace> BuildFontFace(std::span<const Declaration> declarations) {
  FontFace fontFace;
  for (const Declaration& declaration : declarations) {
    const auto descriptor = IdentifyFontFaceDescriptor(declaration.name);
    if (descriptor == FontFaceDescriptor::Family && !declaration.values.empty()) {
      if (const Token* token = std::get_if<Token>(&declaration.values.front().value)) {
        if (token->is<Token::Ident>()) {
          fontFace.familyName = token->get<Token::Ident>().value;
        } else if (token->is<Token::String>()) {
          fontFace.familyName = token->get<Token::String>().value;
        }
      }
    } else if (descriptor == FontFaceDescriptor::Source) {
      AppendFontFaceSources(declaration.values, fontFace);
    } else {
      ApplyFontFaceMatchingDescriptor(descriptor, declaration, fontFace);
    }
  }
  if (fontFace.familyName.empty() || fontFace.sources.empty()) {
    return std::nullopt;
  }
  return fontFace;
}

std::optional<SelectorRule> ParseQualifiedSelectorRule(std::string_view source,
                                                       QualifiedRule& qualifiedRule,
                                                       ParseWarningSink& warningSink,
                                                       Stylesheet::SecurityStats& securityStats) {
  auto selectorResult = SelectorParser::ParseComponents(qualifiedRule.prelude);
  if (selectorResult.hasError()) {
    warningSink.add(std::move(selectorResult.error()));
    return std::nullopt;
  }

  DeclarationListParser::SecurityStats declarationStats;
  std::vector<Declaration> declarations =
      DeclarationListParser::ParseRuleDeclarations(qualifiedRule.block.values, &declarationStats);
  if (!ChargeDeclarations(securityStats, declarationStats, declarations.size())) {
    return std::nullopt;
  }

  SelectorRule selectorRule;
  selectorRule.selector = std::move(selectorResult.result());
  selectorRule.declarations = std::move(declarations);
  PopulateSelectorRuleSourceRanges(source, qualifiedRule, &selectorRule);
  return selectorRule;
}

std::optional<FontFace> ParseFontFaceRule(AtRule& atRule,
                                          Stylesheet::SecurityStats& securityStats) {
  if (!atRule.name.equalsLowercase("font-face") || !atRule.block) {
    return std::nullopt;
  }
  DeclarationListParser::SecurityStats declarationStats;
  std::vector<Declaration> declarations =
      DeclarationListParser::ParseRuleDeclarations(atRule.block->values, &declarationStats);
  if (!ChargeDeclarations(securityStats, declarationStats, declarations.size())) {
    return std::nullopt;
  }
  return BuildFontFace(declarations);
}

bool WarnUnsupportedAtRule(const AtRule& atRule, ParseWarningSink& warningSink) {
  const char* reason;
  if (atRule.name.equalsLowercase("media")) {
    reason = "CSS @media is unsupported; conditional rules are ignored";
  } else if (atRule.name.equalsLowercase("supports")) {
    reason = "CSS @supports is unsupported; conditional rules are ignored";
  } else {
    return false;
  }
  warningSink.add([&] { return ParseDiagnostic::Warning(reason, atRule.sourceOffset); });
  return true;
}

/// How an `@import` prelude is interpreted; see \ref ParseImportPrelude.
enum class ImportPreludeKind : std::uint8_t {
  Unconditional,  ///< Applies unconditionally and is retained.
  Invalid,        ///< Malformed; dropped.
  Supports,       ///< Valid but conditional on `supports()`, which is unsupported.
  Media,          ///< Valid but conditional on a media query list other than `all`.
};

using LayerPath = std::vector<RcString>;

struct ParsedImportPrelude {
  ImportPreludeKind kind = ImportPreludeKind::Invalid;
  RcString url;
  std::optional<LayerPath> layer;  //!< An empty path denotes a fresh anonymous layer.
};

bool IsWhitespaceComponent(const ComponentValue& value) {
  return value.isToken<Token::Whitespace>();
}

std::span<const ComponentValue> TrimLayerWhitespace(std::span<const ComponentValue> values) {
  while (!values.empty() && IsWhitespaceComponent(values.front())) {
    values = values.subspan(1);
  }
  while (!values.empty() && IsWhitespaceComponent(values.back())) {
    values = values.first(values.size() - 1);
  }
  return values;
}

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

bool IsLayerIdent(const RcString& name) {
  constexpr std::array<std::string_view, 5> kReserved{"initial", "inherit", "unset", "revert",
                                                      "revert-layer"};
  return !name.empty() && std::ranges::none_of(kReserved, [&](std::string_view keyword) {
    return name.equalsLowercase(keyword);
  });
}

std::optional<LayerPath> ParseLayerPath(std::span<const ComponentValue> values) {
  values = TrimLayerWhitespace(values);
  if (values.empty() || values.size() % 2 == 0) {
    return std::nullopt;
  }
  LayerPath result;
  for (std::size_t i = 0; i < values.size(); i += 2) {
    const auto* ident = values[i].tryGetToken<Token::Ident>();
    if (!ident || !IsLayerIdent(ident->value)) {
      return std::nullopt;
    }
    if (i != 0) {
      const auto* dot = values[i - 1].tryGetToken<Token::Delim>();
      if (!dot || dot->value != '.') {
        return std::nullopt;
      }
    }
    result.push_back(ident->value);
  }
  return result;
}

std::optional<std::vector<LayerPath>> ParseLayerNames(std::span<const ComponentValue> values) {
  std::vector<LayerPath> result;
  for (;;) {
    const auto comma = std::ranges::find_if(
        values, [](const ComponentValue& value) { return value.isToken<Token::Comma>(); });
    const auto length = static_cast<std::size_t>(comma - values.begin());
    auto path = ParseLayerPath(values.first(length));
    if (!path) {
      return std::nullopt;
    }
    result.push_back(std::move(*path));
    if (comma == values.end()) {
      return result;
    }
    values = values.subspan(length + 1);
  }
}

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute pop
#endif

/// The URL of an `@import` target: a `<string>`, an unquoted `url()` token, or `url(<string>)`.
std::optional<RcString> ImportTargetUrl(const ComponentValue& value) {
  if (const auto* string = value.tryGetToken<Token::String>()) {
    return string->value;
  }
  if (const auto* url = value.tryGetToken<Token::Url>()) {
    return url->value;
  }
  const auto* function = std::get_if<Function>(&value.value);
  if (function == nullptr || !function->name.equalsLowercase("url")) {
    return std::nullopt;
  }
  std::optional<RcString> result;
  for (const ComponentValue& argument : function->values) {
    if (IsWhitespaceComponent(argument)) {
      continue;
    }
    const auto* string = argument.tryGetToken<Token::String>();
    if (string == nullptr || result.has_value()) {
      return std::nullopt;
    }
    result = string->value;
  }
  return result;
}

bool IsNamedComponent(const ComponentValue& value, std::string_view name) {
  if (const auto* ident = value.tryGetToken<Token::Ident>()) {
    return ident->value.equalsLowercase(name);
  }
  const auto* function = std::get_if<Function>(&value.value);
  return function != nullptr && function->name.equalsLowercase(name);
}

bool IsFunctionNamed(const ComponentValue& value, std::string_view name) {
  const auto* function = std::get_if<Function>(&value.value);
  return function != nullptr && function->name.equalsLowercase(name);
}

/// Classify an `@import` media query list: `all` is unconditional, and a second URL is invalid.
ImportPreludeKind ClassifyImportMediaList(std::span<const ComponentValue* const> mediaList) {
  for (const ComponentValue* value : mediaList) {
    if (value->isToken<Token::String>() || value->isToken<Token::Url>()) {
      return ImportPreludeKind::Invalid;
    }
  }
  if (mediaList.size() == 1 && mediaList.front()->isToken<Token::Ident>() &&
      IsNamedComponent(*mediaList.front(), "all")) {
    return ImportPreludeKind::Unconditional;
  }
  return ImportPreludeKind::Media;
}

/// Classify the import conditions following the URL, per the CSS Cascade 5 `@import` grammar:
/// `[ layer | layer(<name>) ]? [ supports(...) ]? <media-query-list>?`.
void ParseImportConditions(std::span<const ComponentValue> conditions,
                           ParsedImportPrelude& parsed) {
  std::vector<const ComponentValue*> significant;
  for (const ComponentValue& value : conditions) {
    if (!IsWhitespaceComponent(value)) {
      significant.push_back(&value);
    }
  }
  std::span<const ComponentValue* const> remainder(significant);
  if (!remainder.empty() && IsNamedComponent(*remainder.front(), "layer")) {
    const auto& layer = *remainder.front();
    if (const auto* function = std::get_if<Function>(&layer.value)) {
      parsed.layer = ParseLayerPath(function->values);
      if (!parsed.layer) {
        // An invalid layer() stays in the media query list, which this parser does not apply.
        parsed.kind = ImportPreludeKind::Media;
        return;
      }
    } else {
      parsed.layer.emplace();
    }
    remainder = remainder.subspan(1);
  }
  if (remainder.empty()) {
    parsed.kind = ImportPreludeKind::Unconditional;
  } else if (IsFunctionNamed(*remainder.front(), "supports")) {
    parsed.kind = ImportPreludeKind::Supports;
  } else {
    parsed.kind = ClassifyImportMediaList(remainder);
  }
}

ParsedImportPrelude ParseImportPrelude(const AtRule& atRule) {
  if (atRule.block.has_value()) {
    return {};
  }
  const std::span<const ComponentValue> prelude = atRule.prelude;
  std::size_t index = 0;
  while (index < prelude.size() && IsWhitespaceComponent(prelude[index])) {
    ++index;
  }
  if (index == prelude.size()) {
    return {};
  }
  std::optional<RcString> url = ImportTargetUrl(prelude[index]);
  if (!url.has_value() || url->empty()) {
    return {};
  }
  ParsedImportPrelude result;
  result.url = std::move(*url);
  ParseImportConditions(prelude.subspan(index + 1), result);
  return result;
}

const char* IgnoredImportReason(ImportPreludeKind kind) {
  switch (kind) {
    case ImportPreludeKind::Unconditional: return nullptr;
    case ImportPreludeKind::Invalid:
      return "CSS @import requires a single non-empty URL or string; this import is ignored";
    case ImportPreludeKind::Supports:
      return "CSS @import supports() conditions are unsupported; this import is ignored";
    case ImportPreludeKind::Media:
      return "CSS @import media conditions other than 'all' are unsupported; this import is "
             "ignored";
  }
  UTILS_UNREACHABLE();
}

/**
 * Whether an at-rule ends the leading section in which `@import` is valid. `@charset` is consumed
 * before rule parsing, `@layer` statements (without a block) may precede imports, and unknown or
 * malformed at-rules are invalid and therefore dropped without effect.
 */
bool AtRuleEndsImportSection(const AtRule& atRule) {
  if (atRule.name.equalsLowercase("namespace")) {
    return !atRule.block.has_value();
  }
  // The remaining known at-rules are valid only with a block; `@layer` without one is a
  // statement that may precede imports.
  if (!atRule.block.has_value()) {
    return false;
  }
  constexpr std::array<std::string_view, 14> kBlockAtRules = {"media",
                                                              "supports",
                                                              "font-face",
                                                              "layer",
                                                              "keyframes",
                                                              "page",
                                                              "counter-style",
                                                              "property",
                                                              "container",
                                                              "font-feature-values",
                                                              "font-palette-values",
                                                              "scope",
                                                              "starting-style",
                                                              "view-transition"};
  return std::any_of(kBlockAtRules.begin(), kBlockAtRules.end(),
                     [&](std::string_view name) { return atRule.name.equalsLowercase(name); });
}

/// Mutable state threaded through top-level rules and layer bodies.
struct StylesheetParseState {
  std::vector<SelectorRule> selectorRules;
  std::vector<FontFace> fontFaces;
  std::vector<ImportRule> imports;
  StylesheetLayers layers;
  std::map<std::pair<LayerId, RcString>, LayerId> namedLayers;
  Stylesheet::SecurityStats securityStats;
  std::size_t nestedRulesRemaining = 0;
  bool importsAllowed = true;
  bool sawImport = false;
};

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

void RecordLayerEvent(StylesheetParseState& state, LayerEvent event) {
  if (state.layers.events.empty()) {
    // Plain import-only sheets can keep the order implicit in imports(). Once a layer needs
    // an explicit stream, record those earlier import positions too.
    for (std::size_t i = 0; i < state.imports.size(); ++i) {
      state.layers.events.push_back({LayerEvent::Kind::Import, static_cast<std::uint32_t>(i)});
    }
  }
  state.layers.events.push_back(event);
}

std::size_t LayerDepth(const StylesheetLayers& layers, LayerId parent) {
  std::size_t depth = 0;
  for (; parent != kUnlayered; parent = layers.nodes[parent].parent) {
    ++depth;
  }
  return depth;
}

std::optional<LayerId> AddLayerSegment(StylesheetParseState& state, LayerId parent,
                                       const RcString& name) {
  const auto key = std::pair(parent, name);
  if (!name.empty()) {
    if (const auto it = state.namedLayers.find(key); it != state.namedLayers.end()) {
      return it->second;
    }
  }
  if (state.layers.nodes.size() == Stylesheet::kMaximumLayers) {
    return std::nullopt;
  }
  const LayerId layer = static_cast<LayerId>(state.layers.nodes.size());
  state.layers.nodes.push_back({parent, name});
  if (!name.empty()) {
    state.namedLayers.emplace(key, layer);
  }
  return layer;
}

std::optional<LayerId> AddLayerPath(StylesheetParseState& state, LayerId parent,
                                    const LayerPath& path) {
  const std::size_t depth = LayerDepth(state.layers, parent);
  if (std::max<std::size_t>(1, path.size()) > Stylesheet::kMaximumLayerDepth - depth) {
    return std::nullopt;
  }
  if (path.empty()) {
    return AddLayerSegment(state, parent, RcString());
  }
  for (const auto& name : path) {
    const auto layer = AddLayerSegment(state, parent, name);
    if (!layer) {
      return std::nullopt;
    }
    parent = *layer;
  }
  return parent;
}

void UndoNewLayers(StylesheetParseState& state, std::size_t checkpoint) {
  while (state.layers.nodes.size() > checkpoint) {
    const auto& node = state.layers.nodes.back();
    if (!node.name.empty()) {
      state.namedLayers.erase({node.parent, node.name});
    }
    state.layers.nodes.pop_back();
  }
}

void WarnLayerLimit(const AtRule& atRule, ParseWarningSink& warningSink) {
  warningSink.add([&] {
    return ParseDiagnostic::Warning("CSS cascade layer limit exceeded; this rule is ignored",
                                    atRule.sourceOffset);
  });
}

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute pop
#endif

void HandleImportRule(const AtRule& atRule, StylesheetParseState& state,
                      ParseWarningSink& warningSink) {
  if (!state.importsAllowed) {
    warningSink.add([&] {
      return ParseDiagnostic::Warning(
          "CSS @import must precede all other rules; this import is ignored", atRule.sourceOffset);
    });
    return;
  }
  ParsedImportPrelude parsed = ParseImportPrelude(atRule);
  state.sawImport |= parsed.kind != ImportPreludeKind::Invalid;
  if (const char* reason = IgnoredImportReason(parsed.kind)) {
    warningSink.add([&] { return ParseDiagnostic::Warning(reason, atRule.sourceOffset); });
    return;
  }
  LayerId layer = kUnlayered;
  if (parsed.layer) {
    const auto checkpoint = state.layers.nodes.size();
    if (const auto id = AddLayerPath(state, kUnlayered, *parsed.layer)) {
      layer = *id;
    } else {
      UndoNewLayers(state, checkpoint);
      WarnLayerLimit(atRule, warningSink);
      return;
    }
  }
  if (!state.layers.nodes.empty()) {
    RecordLayerEvent(state,
                     {LayerEvent::Kind::Import, static_cast<std::uint32_t>(state.imports.size())});
  }
  state.imports.push_back(ImportRule{
      .url = std::move(parsed.url), .sourceOffset = atRule.sourceOffset, .layer = layer});
}

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

SimpleBlock* CurlyBlock(ComponentValue& value) {
  auto* block = std::get_if<SimpleBlock>(&value.value);
  return block && block->associatedToken == Token::indexOf<Token::CurlyBracket>() ? block : nullptr;
}

AtRule TakeNestedAtRule(std::span<ComponentValue> values, std::size_t& index,
                        const Token::AtKeyword& keyword) {
  AtRule rule(keyword.value);
  rule.sourceOffset = values[index++].sourceOffset();
  while (index < values.size()) {
    auto& component = values[index++];
    if (component.isToken<Token::Semicolon>()) {
      break;
    }
    if (auto* block = CurlyBlock(component)) {
      rule.block = std::move(*block);
      break;
    }
    rule.prelude.push_back(std::move(component));
  }
  return rule;
}

std::optional<QualifiedRule> TakeNestedQualifiedRule(std::span<ComponentValue> values,
                                                     std::size_t& index) {
  std::vector<ComponentValue> prelude;
  while (index < values.size()) {
    auto& component = values[index++];
    if (auto* block = CurlyBlock(component)) {
      return QualifiedRule(std::move(prelude), std::move(*block));
    }
    prelude.push_back(std::move(component));
  }
  return std::nullopt;
}

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute pop
#endif

void HandleRule(std::string_view source, Rule& rule, StylesheetParseState& state,
                ParseWarningSink& warningSink, LayerId parent);

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

void HandleLayerBody(std::string_view source, SimpleBlock& block, StylesheetParseState& state,
                     ParseWarningSink& warningSink, LayerId parent) {
  std::span<ComponentValue> values(block.values);
  for (std::size_t i = 0; i < values.size() && !state.securityStats.rejected;) {
    if (IsWhitespaceComponent(values[i])) {
      ++i;
      continue;
    }
    if (state.nestedRulesRemaining == 0) {
      state.securityStats.rejected = true;
      return;
    }
    --state.nestedRulesRemaining;
    if (const auto* keyword = values[i].tryGetToken<Token::AtKeyword>()) {
      Rule rule(TakeNestedAtRule(values, i, *keyword));
      HandleRule(source, rule, state, warningSink, parent);
    } else if (auto qualified = TakeNestedQualifiedRule(values, i)) {
      Rule rule(std::move(*qualified));
      HandleRule(source, rule, state, warningSink, parent);
    }
  }
}

std::optional<std::vector<LayerPath>> ParseLayerRuleNames(const AtRule& rule) {
  if (!rule.block) {
    return ParseLayerNames(rule.prelude);
  }
  if (TrimLayerWhitespace(rule.prelude).empty()) {
    return std::vector<LayerPath>(1);
  }
  auto path = ParseLayerPath(rule.prelude);
  if (!path) {
    return std::nullopt;
  }
  std::vector<LayerPath> names;
  names.push_back(std::move(*path));
  return names;
}

std::optional<std::vector<LayerId>> AddLayerDeclaration(StylesheetParseState& state, LayerId parent,
                                                        std::span<const LayerPath> names) {
  const std::size_t checkpoint = state.layers.nodes.size();
  std::vector<LayerId> ids;
  for (const auto& path : names) {
    const auto layer = AddLayerPath(state, parent, path);
    if (!layer) {
      UndoNewLayers(state, checkpoint);
      return std::nullopt;
    }
    ids.push_back(*layer);
  }
  return ids;
}

void HandleLayerRule(std::string_view source, AtRule& rule, StylesheetParseState& state,
                     ParseWarningSink& warningSink, LayerId parent) {
  const auto names = ParseLayerRuleNames(rule);
  if (!names) {
    warningSink.add([&] {
      return ParseDiagnostic::Warning("Invalid CSS cascade layer name", rule.sourceOffset);
    });
    return;
  }
  if (rule.block || state.sawImport) {
    state.importsAllowed = false;
  }
  const auto ids = AddLayerDeclaration(state, parent, *names);
  if (!ids) {
    WarnLayerLimit(rule, warningSink);
    return;
  }
  const auto kind = rule.block ? LayerEvent::Kind::Block : LayerEvent::Kind::Statement;
  for (LayerId layer : *ids) {
    RecordLayerEvent(state, {kind, layer});
  }
  if (rule.block) {
    HandleLayerBody(source, *rule.block, state, warningSink, ids->front());
  }
}

#if defined(DONNER_APPLE_LAYER_PARSER_MINSIZE)
#pragma clang attribute pop
#undef DONNER_APPLE_LAYER_PARSER_MINSIZE
#endif

void HandleAtRule(std::string_view source, AtRule& atRule, StylesheetParseState& state,
                  ParseWarningSink& warningSink, LayerId parent) {
  if (atRule.name.equalsLowercase("import")) {
    HandleImportRule(atRule, state, warningSink);
    return;
  }
  if (atRule.name.equalsLowercase("layer")) {
    HandleLayerRule(source, atRule, state, warningSink, parent);
    return;
  }
  if (AtRuleEndsImportSection(atRule)) {
    state.importsAllowed = false;
  }
  if (WarnUnsupportedAtRule(atRule, warningSink)) {
    return;
  }
  if (auto fontFace = ParseFontFaceRule(atRule, state.securityStats)) {
    auto& faceLayers = state.layers.fontFaceLayers;
    if (!faceLayers.empty() || parent != kUnlayered) {
      faceLayers.resize(state.fontFaces.size(), kUnlayered);
      faceLayers.push_back(parent);
    }
    state.fontFaces.push_back(std::move(*fontFace));
    ++state.securityStats.rules;
  }
}

void HandleRule(std::string_view source, Rule& rule, StylesheetParseState& state,
                ParseWarningSink& warningSink, LayerId parent) {
  if (auto* qualified = std::get_if<QualifiedRule>(&rule.value)) {
    if (auto parsed =
            ParseQualifiedSelectorRule(source, *qualified, warningSink, state.securityStats)) {
      parsed->layer = parent;
      state.selectorRules.push_back(std::move(*parsed));
      ++state.securityStats.rules;
      state.importsAllowed = false;
    }
  } else if (auto* atRule = std::get_if<AtRule>(&rule.value)) {
    HandleAtRule(source, *atRule, state, warningSink, parent);
  }
}

}  // namespace

Stylesheet StylesheetParser::Parse(std::string_view str, ParseWarningSink& warningSink) {
  RuleParser::SecurityStats ruleStats;
  std::vector<Rule> rules = RuleParser::ParseStylesheet(str, &ruleStats);

  StylesheetParseState state;
  state.securityStats.rejected = ruleStats.rejected;
  state.securityStats.componentValues = ruleStats.componentValues;

  state.nestedRulesRemaining = RuleParser::kMaximumRules - rules.size();
  for (Rule& rule : rules) {
    HandleRule(str, rule, state, warningSink, kUnlayered);
    if (state.securityStats.rejected) {
      break;
    }
  }
  auto layers = state.layers.nodes.empty()
                    ? nullptr
                    : std::make_shared<const StylesheetLayers>(std::move(state.layers));
  return Stylesheet(std::move(state.selectorRules), std::move(state.fontFaces),
                    std::move(state.imports), state.securityStats, std::move(layers));
}

}  // namespace donner::css::parser
