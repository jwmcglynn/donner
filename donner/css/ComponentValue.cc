#include "donner/css/ComponentValue.h"

namespace donner::css {

Function::Function(RcString name, const FileOffset& sourceOffset)
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

SimpleBlock::SimpleBlock(TokenIndex associatedToken, const FileOffset& sourceOffset)
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

std::string Function::toCssText() const {
  std::string result = std::string(name) + "(";
  for (const auto& v : values) {
    result += v.toCssText();
  }
  result += ")";
  return result;
}

std::string SimpleBlock::toCssText() const {
  std::string result;
  if (associatedToken == Token::indexOf<Token::CurlyBracket>()) {
    result = "{";
  } else if (associatedToken == Token::indexOf<Token::SquareBracket>()) {
    result = "[";
  } else if (associatedToken == Token::indexOf<Token::Parenthesis>()) {
    result = "(";
  }
  for (const auto& v : values) {
    result += v.toCssText();
  }
  if (associatedToken == Token::indexOf<Token::CurlyBracket>()) {
    result += "}";
  } else if (associatedToken == Token::indexOf<Token::SquareBracket>()) {
    result += "]";
  } else if (associatedToken == Token::indexOf<Token::Parenthesis>()) {
    result += ")";
  }
  return result;
}

ComponentValue::ComponentValue(ComponentValue::Type&& value) : value(std::move(value)) {}

ComponentValue::~ComponentValue() = default;

bool ComponentValue::operator==(const ComponentValue& other) const {
  return value == other.value;
}

std::string ComponentValue::toCssText() const {
  return std::visit(
      [](auto&& v) -> std::string {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Token>) {
          return v.toCssText();
        } else {
          return v.toCssText();
        }
      },
      value);
}

}  // namespace donner::css
