#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/declaration_list_parser.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;

namespace donner {
namespace css {

void PrintTo(const AtRule& rule, std::ostream* os) {
  *os << "AtRule {\n";
  *os << "  " << rule.name << "\n";
  for (auto& value : rule.prelude) {
    *os << "  " << testing::PrintToString(value) << "\n";
  }
  if (rule.block) {
    *os << "  { " << testing::PrintToString(*rule.block) << " }\n";
  }
  *os << "}";
}

void PrintTo(const Function& func, std::ostream* os) {
  *os << "Function { ";
  *os << func.name << "(";
  for (auto& value : func.values) {
    *os << " " << testing::PrintToString(value);
  }
  *os << " ) }";
}

void PrintTo(const SimpleBlock& block, std::ostream* os) {
  *os << "SimpleBlock {\n";
  *os << "  token=";
  switch (block.associatedToken) {
    case Token::indexOf<Token::CurlyBracket>(): *os << "'{'"; break;
    case Token::indexOf<Token::SquareBracket>(): *os << "'['"; break;
    case Token::indexOf<Token::Parenthesis>(): *os << "'('"; break;
    default: *os << "<unknown>"; break;
  }
  *os << "\n";
  for (auto& value : block.values) {
    *os << "  " << testing::PrintToString(value) << "\n";
  }
  *os << "}";
}

void PrintTo(const ComponentValue& value, std::ostream* os) {
  std::visit([os](auto&& v) { *os << testing::PrintToString(v); }, value);
}

void PrintTo(const Declaration& declaration, std::ostream* os) {
  *os << "Declaration { \n";
  *os << "  " << declaration.name << "\n";
  for (auto& value : declaration.values) {
    *os << "  " << testing::PrintToString(value) << "\n";
  }
  if (declaration.important) {
    *os << "  !important\n";
  }
  *os << "}";
}

TEST(DeclarationListParser, Empty) {
  EXPECT_THAT(DeclarationListParser::Parse(""), ParseResultIs(ElementsAre()));
}

TEST(DeclarationListParser, Simple) {
  EXPECT_THAT(DeclarationListParser::Parse("test: test"),
              ParseResultIs(ElementsAre(DeclarationIs("test", ElementsAre(TokenIsIdent("test"))))));
}

TEST(DeclarationListParser, Important) {
  EXPECT_THAT(
      DeclarationListParser::Parse("name: value !important"),
      ParseResultIs(ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")), true))));
  EXPECT_THAT(DeclarationListParser::Parse("test: !important value"),
              ParseResultIs(ElementsAre(
                  DeclarationIs("test", ElementsAre(TokenIsDelim('!'), TokenIsIdent("important"),
                                                    TokenIsIdent("value"))))));
}

TEST(DISABLED_DeclarationListParser, AtRule) {
  // TODO
  EXPECT_THAT(DeclarationListParser::Parse("@test test; @thing {}"), ParseResultIs(ElementsAre()));
}

}  // namespace css
}  // namespace donner
