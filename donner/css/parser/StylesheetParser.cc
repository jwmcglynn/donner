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

}  // namespace

Stylesheet StylesheetParser::Parse(std::string_view str, ParseWarningSink& warningSink) {
  std::vector<Rule> rules = RuleParser::ParseStylesheet(str);

  std::vector<SelectorRule> selectorRules;
  std::vector<FontFace> fontFaces;
  for (auto&& rule : rules) {
    // If the rule is a QualifiedRule, then we need to parse the selector and add it to our list.
    if (QualifiedRule* qualifiedRule = std::get_if<QualifiedRule>(&rule.value)) {
      auto selectorResult = SelectorParser::ParseComponents(qualifiedRule->prelude);
      if (selectorResult.hasError()) {
        warningSink.add(std::move(selectorResult.error()));
        continue;
      }

      std::vector<Declaration> declarations =
          DeclarationListParser::ParseRuleDeclarations(qualifiedRule->block.values);

      SelectorRule selectorRule;
      selectorRule.selector = std::move(selectorResult.result());
      selectorRule.declarations = std::move(declarations);
      PopulateSelectorRuleSourceRanges(str, *qualifiedRule, &selectorRule);
      selectorRules.emplace_back(std::move(selectorRule));
    } else if (AtRule* atRule = std::get_if<AtRule>(&rule.value)) {
      if (atRule->name.equalsLowercase("font-face") && atRule->block) {
        std::vector<Declaration> declarations =
            DeclarationListParser::ParseRuleDeclarations(atRule->block->values);

        FontFace fontFace;

        auto addSrc = [&](std::vector<ComponentValue> items) {
          if (items.empty()) {
            return;
          }

          std::optional<FontFaceSource> source;

          const ComponentValue& first = items.front();
          if (const Function* func = std::get_if<Function>(&first.value)) {
            if (func->name.equalsLowercase("local") && !func->values.empty()) {
              if (const Token* t = std::get_if<Token>(&func->values.front().value)) {
                RcString name;
                if (t->is<Token::Ident>()) {
                  name = t->get<Token::Ident>().value;
                } else if (t->is<Token::String>()) {
                  name = t->get<Token::String>().value;
                }
                source = FontFaceSource{FontFaceSource::Kind::Local, std::move(name), "", {}};
              }
            } else if (func->name.equalsLowercase("url") && !func->values.empty()) {
              RcString url;
              if (const Token* t = std::get_if<Token>(&func->values.front().value)) {
                // The tokenizer will produce a String or Ident for url("...") or url(foo), so we
                // need to handle both.
                if (t->is<Token::String>()) {
                  url = t->get<Token::String>().value;
                } else if (t->is<Token::Ident>()) {
                  url = t->get<Token::Ident>().value;
                } else if (t->is<Token::Url>()) {
                  url = t->get<Token::Url>().value;
                }
              }

              if (auto maybeSource = TryParseFontFaceSourceFromUrl(url)) {
                source = std::move(*maybeSource);
              }
            }
          } else if (const Token* urlTok = std::get_if<Token>(&first.value)) {
            if (urlTok->is<Token::Url>()) {
              const RcString& url = urlTok->get<Token::Url>().value;
              if (auto maybeSource = TryParseFontFaceSourceFromUrl(url)) {
                source = std::move(*maybeSource);
              }
            }
          }

          if (source) {
            // parse additional format() or tech() hints
            for (size_t i = 1; i < items.size(); ++i) {
              const ComponentValue& cv = items[i];
              if (const Function* f = std::get_if<Function>(&cv.value)) {
                if (f->name.equalsLowercase("format") && !f->values.empty()) {
                  if (const Token* tok = std::get_if<Token>(&f->values.front().value)) {
                    if (tok->is<Token::Ident>()) {
                      source->formatHint = tok->get<Token::Ident>().value;
                    } else if (tok->is<Token::String>()) {
                      source->formatHint = tok->get<Token::String>().value;
                    }
                  }
                } else if (f->name.equalsLowercase("tech")) {
                  for (const auto& val : f->values) {
                    if (const Token* tok = std::get_if<Token>(&val.value)) {
                      if (tok->is<Token::Ident>()) {
                        source->techHints.push_back(tok->get<Token::Ident>().value);
                      } else if (tok->is<Token::String>()) {
                        source->techHints.push_back(tok->get<Token::String>().value);
                      }
                    }
                  }
                }
              }
            }

            fontFace.sources.push_back(std::move(source.value()));
          }
        };

        for (const auto& decl : declarations) {
          if (StringUtils::EqualsLowercase(decl.name, std::string_view("font-family")) &&
              !decl.values.empty()) {
            if (const Token* token = std::get_if<Token>(&decl.values.front().value)) {
              if (token->is<Token::Ident>()) {
                fontFace.familyName = token->get<Token::Ident>().value;
              } else if (token->is<Token::String>()) {
                fontFace.familyName = token->get<Token::String>().value;
              }
            }
          } else if (StringUtils::EqualsLowercase(decl.name, std::string_view("src"))) {
            std::vector<ComponentValue> current;
            for (const ComponentValue& cv : decl.values) {
              if (const Token* token = std::get_if<Token>(&cv.value);
                  token && token->is<Token::Comma>()) {
                addSrc(std::move(current));
                current.clear();
              } else {
                current.push_back(cv);
              }
            }
            addSrc(std::move(current));
          }
        }

        if (!fontFace.familyName.empty() && !fontFace.sources.empty()) {
          fontFaces.push_back(std::move(fontFace));
        }
      }
    }
  }

  return Stylesheet(std::move(selectorRules), std::move(fontFaces));
}

}  // namespace donner::css::parser
