#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "src/css/parser/details/subparsers.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner {
namespace css {

using details::Tokenizer;

namespace {

std::filesystem::path kTestDataDirectory = "external/css-parsing-tests/";

nlohmann::json loadJson(std::filesystem::path file) {
  std::ifstream in(file);
  EXPECT_TRUE(in) << "Failed to open " << file << ", pwd: " << std::filesystem::current_path();

  nlohmann::json result;
  in >> result;
  return result;
}

std::optional<ComponentValue> tryConsumeComponentValue(std::string_view css) {
  Tokenizer tokenizer(css);
  if (tokenizer.isEOF()) {
    return std::nullopt;
  }

  return details::consumeComponentValue(tokenizer, tokenizer.next(), details::ParseMode::Keep);
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
      return {"number", t.value};  // TODO, this needs more values
    } else if constexpr (std::is_same_v<Type, Token::Percentage>) {
      return {"percentage", t.value};  // TODO, this needs more values
    } else if constexpr (std::is_same_v<Type, Token::Dimension>) {
      return {"dimension", t.value, t.suffix};  // TODO, this needs more values
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
      return "]";
    } else if constexpr (std::is_same_v<Type, Token::CloseParenthesis>) {
      return ")";
    } else if constexpr (std::is_same_v<Type, Token::CloseCurlyBracket>) {
      return "}";
    } else if constexpr (std::is_same_v<Type, Token::ErrorToken>) {
      return {"error",
              t.type == Token::ErrorToken::Type::EofInString ? "eof-in-string" : "eof-in-comment"};
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

}  // namespace

TEST(CssParsingTests, ComponentValue) {
  auto json = loadJson(kTestDataDirectory / "component_value_list.json");

  int i = 0;
  int limit = 20;

  for (auto it = json.begin(); i < limit && it != json.end(); ++it, ++i) {
    const std::string css = *it++;
    const nlohmann::json expectedTokens = *it;

    SCOPED_TRACE(testing::Message() << "CSS: " << css);

    if (auto maybeComponentValue = tryConsumeComponentValue(css)) {
      const auto componentValue = std::move(maybeComponentValue.value());

      ASSERT_TRUE(!expectedTokens.empty());
      EXPECT_EQ(expectedTokens[0], componentValueToJson(componentValue));
    } else {
      EXPECT_THAT(expectedTokens, testing::IsEmpty());
    }
  }
}

}  // namespace css
}  // namespace donner
