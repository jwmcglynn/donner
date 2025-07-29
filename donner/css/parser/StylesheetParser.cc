#include "donner/css/parser/StylesheetParser.h"

#include "donner/base/StringUtils.h"
#include "donner/base/encoding/Base64.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/SelectorParser.h"

namespace donner::css::parser {

namespace {

/**
 * Try to parse a `url()` function that contains a data URL.
 *
 * @param url The URL to parse.
 * @return A `FontFaceSource` if the URL is a data URL, otherwise `std::nullopt`.
 */
std::optional<FontFaceSource> TryParseDataUrl(std::string_view url) {
  if (StringUtils::StartsWith(url, std::string_view("data:"))) {
    FontFaceSource source;
    source.kind = FontFaceSource::Kind::Data;

    // TODO: Add support for regular non-base64 data URLs, share logic with UrlLoader.
    if (size_t pos = StringUtils::Find(url, std::string_view("base64,"));
        pos != std::string::npos) {
      std::string_view b64 = url.substr(pos + 7);
      if (auto res = DecodeBase64Data(b64); res.hasResult()) {
        source.payload = std::move(res.result());
      }
    }

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

              if (!url.empty()) {
                if (auto dataUrl = TryParseDataUrl(url)) {
                  source = std::move(dataUrl);
                } else {
                  source = FontFaceSource{FontFaceSource::Kind::Url, std::move(url), "", {}};
                }
              }
            }
          } else if (const Token* urlTok = std::get_if<Token>(&first.value)) {
            if (urlTok->is<Token::Url>()) {
              const RcString url = urlTok->get<Token::Url>().value;
              if (auto dataUrl = TryParseDataUrl(url)) {
                source = std::move(dataUrl);
              } else {
                source = FontFaceSource{FontFaceSource::Kind::Url, url, "", {}};
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
                      source->formatHint = std::string(tok->get<Token::Ident>().value);
                    } else if (tok->is<Token::String>()) {
                      source->formatHint = std::string(tok->get<Token::String>().value);
                    }
                  }
                } else if (f->name.equalsLowercase("tech")) {
                  for (const auto& val : f->values) {
                    if (const Token* tok = std::get_if<Token>(&val.value)) {
                      if (tok->is<Token::Ident>()) {
                        source->techHints.push_back(std::string(tok->get<Token::Ident>().value));
                      } else if (tok->is<Token::String>()) {
                        source->techHints.push_back(std::string(tok->get<Token::String>().value));
                      }
                    }
                  }
                }
              }
            }

            fontFace.sources_.push_back(std::move(source.value()));
          }
        };

        for (const auto& decl : declarations) {
          if (StringUtils::EqualsLowercase(decl.name, std::string_view("font-family")) &&
              !decl.values.empty()) {
            if (const Token* token = std::get_if<Token>(&decl.values.front().value)) {
              if (token->is<Token::Ident>()) {
                fontFace.familyName_ = std::string(token->get<Token::Ident>().value);
              } else if (token->is<Token::String>()) {
                fontFace.familyName_ = std::string(token->get<Token::String>().value);
              }
            }
          } else if (StringUtils::EqualsLowercase(decl.name, std::string_view("src"))) {
            std::vector<ComponentValue> current;
            for (const ComponentValue& cv : decl.values) {
              if (const Token* tok = std::get_if<Token>(&cv.value);
                  tok && tok->is<Token::Comma>()) {
                addSrc(std::move(current));
                current.clear();
              } else {
                current.push_back(cv);
              }
            }
            addSrc(std::move(current));
          } else if (StringUtils::EqualsLowercase(decl.name, std::string_view("font-display")) &&
                     !decl.values.empty()) {
            if (const Token* token = std::get_if<Token>(&decl.values.front().value)) {
              if (token->is<Token::Ident>()) {
                auto val = token->get<Token::Ident>().value;
                if (val.equalsLowercase("block")) {
                  fontFace.display_ = FontDisplay::Block;
                } else if (val.equalsLowercase("swap")) {
                  fontFace.display_ = FontDisplay::Swap;
                } else if (val.equalsLowercase("fallback")) {
                  fontFace.display_ = FontDisplay::Fallback;
                } else if (val.equalsLowercase("optional")) {
                  fontFace.display_ = FontDisplay::Optional;
                } else {
                  fontFace.display_ = FontDisplay::Auto;
                }
              }
            }
          }
        }

        if (!fontFace.familyName_.empty() && !fontFace.sources_.empty()) {
          fontFaces.push_back(std::move(fontFace));
        }
      }
    }
  }

  return Stylesheet(std::move(selectorRules), std::move(fontFaces));
}

}  // namespace donner::css::parser
