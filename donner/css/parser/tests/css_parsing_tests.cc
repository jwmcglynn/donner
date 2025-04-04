#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/details/Subparsers.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser {

using details::Tokenizer;

namespace {

nlohmann::json loadJson(const std::filesystem::path& file) {
  std::ifstream in(file);
  EXPECT_TRUE(in) << "Failed to open " << file << ", pwd: " << std::filesystem::current_path();

  nlohmann::json result;
  in >> result;
  return result;
}

std::vector<ComponentValue> consumeComponentValueList(std::string_view css) {
  Tokenizer tokenizer(css);
  return details::parseListOfComponentValues(tokenizer);
}

std::vector<ComponentValue> removeUntestedTokens(const std::vector<ComponentValue>& vec) {
  std::vector<ComponentValue> result;
  for (const auto& cv : vec) {
    if (const Token* token = std::get_if<Token>(&cv.value)) {
      if (token->is<Token::ErrorToken>() &&
          token->get<Token::ErrorToken>().type == Token::ErrorToken::Type::EofInComment) {
        continue;
      }
    }

    result.push_back(cv);
  }

  return result;
}

// Forward declaration.
nlohmann::json componentValueToJson(const ComponentValue& value);

nlohmann::json tokenToJson(const Token& token) {
  return token.visit([](auto&& t) -> nlohmann::json {
    using Type = std::remove_cvref_t<decltype(t)>;

    if constexpr (std::is_same_v<Type, Token::Ident>) {
      return {"ident", t.value};
    } else if constexpr (std::is_same_v<Type, Token::Function>) {
      return {"function", t.name};
    } else if constexpr (std::is_same_v<Type, Token::AtKeyword>) {
      return {"at-keyword", t.value};
    } else if constexpr (std::is_same_v<Type, Token::Hash>) {
      return {"hash", t.name, t.type == Token::Hash::Type::Unrestricted ? "unrestricted" : "id"};
    } else if constexpr (std::is_same_v<Type, Token::String>) {
      return {"string", t.value};
    } else if constexpr (std::is_same_v<Type, Token::BadString>) {
      return {"error", "bad-string"};
    } else if constexpr (std::is_same_v<Type, Token::Url>) {
      return {"url", t.value};
    } else if constexpr (std::is_same_v<Type, Token::BadUrl>) {
      return {"error", "bad-url"};
    } else if constexpr (std::is_same_v<Type, Token::Delim>) {
      return std::string(&t.value, 1);
    } else if constexpr (std::is_same_v<Type, Token::Number>) {
      return {"number", t.valueString, t.value,
              t.type == NumberType::Integer ? "integer" : "number"};
    } else if constexpr (std::is_same_v<Type, Token::Percentage>) {
      return {"percentage", t.valueString, t.value,
              t.type == NumberType::Integer ? "integer" : "number"};
    } else if constexpr (std::is_same_v<Type, Token::Dimension>) {
      return {"dimension", t.valueString, t.value,
              t.type == NumberType::Integer ? "integer" : "number", t.suffixString};
    } else if constexpr (std::is_same_v<Type, Token::Whitespace>) {
      return " ";
    } else if constexpr (std::is_same_v<Type, Token::CDO>) {
      return "<!--";
    } else if constexpr (std::is_same_v<Type, Token::CDC>) {
      return "-->";
    } else if constexpr (std::is_same_v<Type, Token::Colon>) {
      return ":";
    } else if constexpr (std::is_same_v<Type, Token::Semicolon>) {
      return ";";
    } else if constexpr (std::is_same_v<Type, Token::Comma>) {
      return ",";
    } else if constexpr (std::is_same_v<Type, Token::SquareBracket>) {
      return "[";
    } else if constexpr (std::is_same_v<Type, Token::Parenthesis>) {
      return "(";
    } else if constexpr (std::is_same_v<Type, Token::CurlyBracket>) {
      return "(";
    } else if constexpr (std::is_same_v<Type, Token::CloseSquareBracket>) {
      return {"error", "]"};
    } else if constexpr (std::is_same_v<Type, Token::CloseParenthesis>) {
      return {"error", ")"};
    } else if constexpr (std::is_same_v<Type, Token::CloseCurlyBracket>) {
      return {"error", "}"};
    } else if constexpr (std::is_same_v<Type, Token::ErrorToken>) {
      if (t.type == Token::ErrorToken::Type::EofInString) {
        return {"error", "eof-in-string"};
      } else if (t.type == Token::ErrorToken::Type::EofInComment) {
        return {"error", "eof-in-comment"};
      } else if (t.type == Token::ErrorToken::Type::EofInUrl) {
        return {"error", "eof-in-url"};
      } else {
        ADD_FAILURE() << "Unexpected error type";
        return "<unexpected error>";
      }
    } else if constexpr (std::is_same_v<Type, Token::EofToken>) {
      return "<eof>";
    }
  });
}

nlohmann::json simpleBlockToJson(const SimpleBlock& b) {
  nlohmann::json result;
  if (b.associatedToken == Token::indexOf<Token::SquareBracket>()) {
    result.push_back("[]");
  } else if (b.associatedToken == Token::indexOf<Token::Parenthesis>()) {
    result.push_back("()");
  } else if (b.associatedToken == Token::indexOf<Token::CurlyBracket>()) {
    result.push_back("{}");
  } else {
    ADD_FAILURE() << "Unexpected token " << b.associatedToken;
  }

  for (const auto& v : b.values) {
    result.push_back(componentValueToJson(v));
  }

  return result;
}

nlohmann::json functionToJson(const Function& f) {
  nlohmann::json result = {"function", f.name};
  for (const auto& v : f.values) {
    result.push_back(componentValueToJson(v));
  }

  return result;
}

nlohmann::json componentValueToJson(const ComponentValue& value) {
  return std::visit(
      [](auto&& v) {
        using Type = std::remove_cvref_t<decltype(v)>;

        if constexpr (std::is_same_v<Type, Token>) {
          return tokenToJson(v);
        } else if constexpr (std::is_same_v<Type, SimpleBlock>) {
          return simpleBlockToJson(v);
        } else {
          return functionToJson(v);
        }
      },
      value.value);
}

nlohmann::json atRuleToJson(const AtRule& r) {
  nlohmann::json prelude = nlohmann::json::array();
  for (const auto& p : r.prelude) {
    prelude.push_back(componentValueToJson(p));
  }

  return {"at-rule", r.name, prelude,
          r.block ? simpleBlockToJson(r.block.value()) : nlohmann::json()};
}

nlohmann::json declarationToJson(const Declaration& d) {
  nlohmann::json values = nlohmann::json::array();
  for (const auto& v : d.values) {
    values.push_back(componentValueToJson(v));
  }

  return {"declaration", d.name, values, d.important};
}

nlohmann::json declarationOrAtRuleToJson(const DeclarationOrAtRule& value) {
  return std::visit(
      [](auto&& v) -> nlohmann::json {
        using Type = std::remove_cvref_t<decltype(v)>;

        if constexpr (std::is_same_v<Type, Declaration>) {
          return declarationToJson(v);
        } else if constexpr (std::is_same_v<Type, AtRule>) {
          return atRuleToJson(v);
        } else {
          return {"error", "invalid"};
        }
      },
      value.value);
}

nlohmann::json qualifiedRuleToJson(const QualifiedRule& r) {
  nlohmann::json prelude = nlohmann::json::array();
  for (const auto& p : r.prelude) {
    prelude.push_back(componentValueToJson(p));
  }

  return {"qualified rule", prelude, simpleBlockToJson(r.block)};
}

nlohmann::json ruleToJson(const Rule& value) {
  return std::visit(
      [](auto&& v) -> nlohmann::json {
        using Type = std::remove_cvref_t<decltype(v)>;

        if constexpr (std::is_same_v<Type, AtRule>) {
          return atRuleToJson(v);
        } else if constexpr (std::is_same_v<Type, QualifiedRule>) {
          return qualifiedRuleToJson(v);
        } else if constexpr (std::is_same_v<Type, InvalidRule>) {
          if (v.type == InvalidRule::Type::ExtraInput) {
            return {"error", "extra-input"};
          } else {
            return {"error", "invalid"};
          }
        }
      },
      value.value);
}

nlohmann::json testConsumeComponentValue(std::string_view css) {
  Tokenizer tokenizer(css);
  details::ComponentValueParsingContext parsingContext;

  while (!tokenizer.isEOF()) {
    Token token = tokenizer.next();
    if (token.is<Token::Whitespace>() || token.is<Token::EofToken>()) {
      continue;
    } else {
      auto result = componentValueToJson(details::consumeComponentValue(
          tokenizer, std::move(token), details::ParseMode::Keep, parsingContext));
      if (!tokenizer.isEOF()) {
        return {"error", "extra-input"};
      }

      return result;
    }
  }

  return {"error", "empty"};
}

nlohmann::json testParseDeclarationJson(std::string_view css) {
  Tokenizer tokenizer(css);
  while (!tokenizer.isEOF()) {
    Token token = tokenizer.next();
    if (token.is<Token::Whitespace>()) {
      continue;
    } else if (!token.is<Token::Ident>()) {
      return {"error", "invalid"};
    } else {
      if (auto declaration = details::consumeDeclaration(
              tokenizer, std::move(token.get<Token::Ident>()), token.offset())) {
        return declarationToJson(declaration.value());
      } else {
        return {"error", "invalid"};
      }
    }
  }

  return {"error", "empty"};
}

}  // namespace

TEST(CssParsingTests, ComponentValue) {
  auto json = loadJson(
      Runfiles::instance().RlocationExternal("css-parsing-tests", "one_component_value.json"));

  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    const auto componentValue = testConsumeComponentValue(css);
    EXPECT_EQ(expectedTokens, componentValue);
  }
}

TEST(CssParsingTests, ComponentValueList) {
  auto json = loadJson(
      Runfiles::instance().RlocationExternal("css-parsing-tests", "component_value_list.json"));

  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    nlohmann::json componentValueList = nlohmann::json::array();
    for (const auto& componentValue : removeUntestedTokens(consumeComponentValueList(css))) {
      componentValueList.push_back(componentValueToJson(componentValue));
    }

    EXPECT_EQ(expectedTokens, componentValueList);
    if (testing::Test::HasFailure()) {
      const size_t minSharedElements = std::min(expectedTokens.size(), componentValueList.size());
      for (size_t i = 0; i < minSharedElements; ++i) {
        ASSERT_EQ(expectedTokens[i], componentValueList[i]) << "At index " << i;
      }
    }
  }
}

TEST(CssParsingTests, DeclarationList) {
  auto json = loadJson(
      Runfiles::instance().RlocationExternal("css-parsing-tests", "declaration_list.json"));

  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    nlohmann::json declarationList = nlohmann::json::array();
    for (const auto& declOrAtRule : DeclarationListParser::Parse(css)) {
      declarationList.push_back(declarationOrAtRuleToJson(declOrAtRule));
    }

    EXPECT_EQ(expectedTokens, declarationList);
    if (testing::Test::HasFailure()) {
      const size_t minSharedElements = std::min(expectedTokens.size(), declarationList.size());
      for (size_t i = 0; i < minSharedElements; ++i) {
        ASSERT_EQ(expectedTokens[i], declarationList[i]) << "At index " << i;
      }
    }
  }
}

TEST(CssParsingTests, OneDeclaration) {
  auto json =
      loadJson(Runfiles::instance().RlocationExternal("css-parsing-tests", "one_declaration.json"));

  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    nlohmann::json declaration = testParseDeclarationJson(css);
    EXPECT_EQ(expectedTokens, declaration);
  }
}

TEST(CssParsingTests, OneRule) {
  auto json =
      loadJson(Runfiles::instance().RlocationExternal("css-parsing-tests", "one_rule.json"));

  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    if (auto maybeRule = RuleParser::ParseRule(css)) {
      nlohmann::json rule = ruleToJson(maybeRule.value());
      EXPECT_EQ(expectedTokens, rule);
    } else {
      EXPECT_EQ(expectedTokens, nlohmann::json::array({"error", "empty"}));
    }
  }
}

TEST(CssParsingTests, RuleList) {
  auto json =
      loadJson(Runfiles::instance().RlocationExternal("css-parsing-tests", "rule_list.json"));

  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    nlohmann::json ruleList = nlohmann::json::array();
    for (const auto& rule : parser::RuleParser::ParseListOfRules(css)) {
      ruleList.push_back(ruleToJson(rule));
    }

    EXPECT_EQ(expectedTokens, ruleList);
    if (testing::Test::HasFailure()) {
      const size_t minSharedElements = std::min(expectedTokens.size(), ruleList.size());
      for (size_t i = 0; i < minSharedElements; ++i) {
        ASSERT_EQ(expectedTokens[i], ruleList[i]) << "At index " << i;
      }
    }
  }
}

TEST(CssParsingTests, Stylesheet) {
  auto json =
      loadJson(Runfiles::instance().RlocationExternal("css-parsing-tests", "stylesheet.json"));

  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    nlohmann::json ruleList = nlohmann::json::array();
    for (const auto& rule : RuleParser::ParseStylesheet(css)) {
      ruleList.push_back(ruleToJson(rule));
    }

    EXPECT_EQ(expectedTokens, ruleList);
    if (testing::Test::HasFailure()) {
      const size_t minSharedElements = std::min(expectedTokens.size(), ruleList.size());
      for (size_t i = 0; i < minSharedElements; ++i) {
        ASSERT_EQ(expectedTokens[i], ruleList[i]) << "At index " << i;
      }
    }
  }
}

}  // namespace donner::css::parser
