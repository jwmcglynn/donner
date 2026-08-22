#include "donner/css/parser/StylesheetParser.h"

#include <cctype>

#include "donner/base/StringUtils.h"
#include "donner/base/encoding/Base64.h"
#include "donner/base/parser/DataUrlParser.h"
#include "donner/css/FontFace.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/SelectorParser.h"

namespace donner::css::parser {

namespace {

bool IsAsciiSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

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

std::size_t TrimStart(std::string_view str, std::size_t start, std::size_t end) {
  while (start < end && IsAsciiSpace(str[start])) {
    ++start;
  }
  return start;
}

std::size_t TrimEnd(std::string_view str, std::size_t start, std::size_t end) {
  while (end > start && IsAsciiSpace(str[end - 1])) {
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

std::optional<FontFace> BuildFontFace(std::span<const Declaration> declarations) {
  FontFace fontFace;
  for (const Declaration& declaration : declarations) {
    if (StringUtils::EqualsLowercase(declaration.name, std::string_view("font-family")) &&
        !declaration.values.empty()) {
      if (const Token* token = std::get_if<Token>(&declaration.values.front().value)) {
        if (token->is<Token::Ident>()) {
          fontFace.familyName = token->get<Token::Ident>().value;
        } else if (token->is<Token::String>()) {
          fontFace.familyName = token->get<Token::String>().value;
        }
      }
    } else if (StringUtils::EqualsLowercase(declaration.name, std::string_view("src"))) {
      AppendFontFaceSources(declaration.values, fontFace);
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

}  // namespace

Stylesheet StylesheetParser::Parse(std::string_view str, ParseWarningSink& warningSink) {
  RuleParser::SecurityStats ruleStats;
  std::vector<Rule> rules = RuleParser::ParseStylesheet(str, &ruleStats);

  Stylesheet::SecurityStats securityStats;
  securityStats.rejected = ruleStats.rejected;
  securityStats.componentValues = ruleStats.componentValues;

  std::vector<SelectorRule> selectorRules;
  std::vector<FontFace> fontFaces;
  for (auto&& rule : rules) {
    if (QualifiedRule* qualifiedRule = std::get_if<QualifiedRule>(&rule.value)) {
      if (auto selectorRule =
              ParseQualifiedSelectorRule(str, *qualifiedRule, warningSink, securityStats)) {
        selectorRules.push_back(std::move(*selectorRule));
        ++securityStats.rules;
      }
      if (securityStats.rejected) {
        break;
      }
      continue;
    }
    if (AtRule* atRule = std::get_if<AtRule>(&rule.value)) {
      if (auto fontFace = ParseFontFaceRule(*atRule, securityStats)) {
        fontFaces.push_back(std::move(*fontFace));
        ++securityStats.rules;
      }
      if (securityStats.rejected) {
        break;
      }
    }
  }

  return Stylesheet(std::move(selectorRules), std::move(fontFaces), securityStats);
}

}  // namespace donner::css::parser
