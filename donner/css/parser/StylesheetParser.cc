#include "donner/css/parser/StylesheetParser.h"

#include "donner/base/StringUtils.h"
#include "donner/base/encoding/Base64.h"
#include "donner/base/parser/DataUrlParser.h"
#include "donner/css/FontFace.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/SelectorParser.h"

namespace donner::css::parser {

namespace {

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
    source.payload = std::move(std::get<std::vector<uint8_t>>(parsedUrl.payload));
    source.formatHint = parsedUrl.mimeType;
    return source;
  } else if (parsedUrl.kind == DataUrlParser::Result::Kind::ExternalUrl) {
    FontFaceSource source;
    source.kind = FontFaceSource::Kind::Url;
    source.payload = parsedUrl.payload;

    return source;
  }

  return std::nullopt;
}

}  // namespace

Stylesheet StylesheetParser::Parse(std::string_view str) {
  std::vector<Rule> rules = RuleParser::ParseStylesheet(str);

  std::vector<SelectorRule> selectorRules;
  std::vector<FontFace> fontFaces;
  for (auto&& rule : rules) {
    // If the rule is a QualifiedRule, then we need to parse the selector and add it to our list.
    if (QualifiedRule* qualifiedRule = std::get_if<QualifiedRule>(&rule.value)) {
      auto selectorResult = SelectorParser::ParseComponents(qualifiedRule->prelude);
      // Ignore errors.
      if (selectorResult.hasError()) {
        continue;
      }

      std::vector<Declaration> declarations =
          DeclarationListParser::ParseRuleDeclarations(qualifiedRule->block.values);

      SelectorRule selectorRule;
      selectorRule.selector = std::move(selectorResult.result());
      selectorRule.declarations = std::move(declarations);
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
