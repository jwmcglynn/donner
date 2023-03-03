#include "src/css/component_value.h"

namespace donner::css {

Function::Function(RcString name, size_t sourceOffset)
    : name(std::move(name)), sourceOffset(sourceOffset) {}

bool Function::operator==(const Function& other) const {
  return name == other.name && values == other.values;
}

std::ostream& operator<<(std::ostream& os, const Function& func) {
  os << "Function { ";
  os << func.name << "(";
  for (const auto& value : func.values) {
    os << " " << value;
  }
  os << " ) }";
  return os;
}

SimpleBlock::SimpleBlock(TokenIndex associatedToken, size_t sourceOffset)
    : associatedToken(associatedToken), sourceOffset(sourceOffset) {}

bool SimpleBlock::operator==(const SimpleBlock& other) const {
  return associatedToken == other.associatedToken && values == other.values;
}

std::ostream& operator<<(std::ostream& os, const SimpleBlock& block) {
  os << "SimpleBlock {\n";
  os << "  token=";
  switch (block.associatedToken) {
    case Token::indexOf<Token::CurlyBracket>(): os << "'{'"; break;
    case Token::indexOf<Token::SquareBracket>(): os << "'['"; break;
    case Token::indexOf<Token::Parenthesis>(): os << "'('"; break;
    default: os << "<unknown>"; break;
  }
  os << "\n";
  for (const auto& value : block.values) {
    os << "  " << value << "\n";
  }
  os << "}";
  return os;
}

ComponentValue::ComponentValue(ComponentValue::Type&& value) : value(std::move(value)) {}

bool ComponentValue::operator==(const ComponentValue& other) const {
  return value == other.value;
}

}  // namespace donner::css
