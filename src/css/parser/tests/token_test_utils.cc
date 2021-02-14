#include "src/css/parser/tests/token_test_utils.h"

#include <ostream>

#include "src/css/token.h"

namespace donner {
namespace css {

void PrintTo(const Token& token, std::ostream* os) {
  *os << "Token { ";
  token.visit([&os](auto&& value) { *os << value; });
  *os << " offset: " << token.offset();
  *os << " }";
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

void PrintTo(const ComponentValue& component, std::ostream* os) {
  std::visit([os](auto&& v) { *os << testing::PrintToString(v); }, component.value);
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

void PrintTo(const InvalidRule& invalidRule, std::ostream* os) {
  *os << "InvalidRule";
  if (invalidRule.type == InvalidRule::Type::ExtraInput) {
    *os << "(ExtraInput)";
  }
}

void PrintTo(const DeclarationOrAtRule& declOrAt, std::ostream* os) {
  std::visit([os](auto&& v) { *os << testing::PrintToString(v); }, declOrAt.value);
}

void PrintTo(const QualifiedRule& qualifiedRule, std::ostream* os) {
  *os << "QualifiedRule {\n";
  for (auto& value : qualifiedRule.prelude) {
    *os << "  " << testing::PrintToString(value) << "\n";
  }
  *os << "  { " << testing::PrintToString(qualifiedRule.block) << " }\n";
  *os << "}";
}

void PrintTo(const Rule& rule, std::ostream* os) {
  std::visit([os](auto&& v) { *os << testing::PrintToString(v); }, rule.value);
}

}  // namespace css
}  // namespace donner
