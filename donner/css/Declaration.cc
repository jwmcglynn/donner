#include "donner/css/Declaration.h"

#include <vector>

#include "donner/css/ComponentValue.h"

namespace donner::css {

namespace {

/// Serialize a single ComponentValue to CSS text.
void serializeComponentValue(std::vector<char>& buf, const ComponentValue& cv) {
  if (cv.is<Token>()) {
    const Token& token = cv.get<Token>();

    if (token.is<Token::Ident>()) {
      const auto& str = token.get<Token::Ident>().value;
      buf.insert(buf.end(), str.begin(), str.end());
    } else if (token.is<Token::Number>()) {
      char tmp[32];
      const double val = token.get<Token::Number>().value;
      int len;
      if (val == std::floor(val) && std::abs(val) < 1e15) {
        len = std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(val));
      } else {
        len = std::snprintf(tmp, sizeof(tmp), "%g", val);
      }
      buf.insert(buf.end(), tmp, tmp + len);
    } else if (token.is<Token::Dimension>()) {
      const auto& dim = token.get<Token::Dimension>();
      char tmp[32];
      int len;
      if (dim.value == std::floor(dim.value) && std::abs(dim.value) < 1e15) {
        len = std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(dim.value));
      } else {
        len = std::snprintf(tmp, sizeof(tmp), "%g", dim.value);
      }
      buf.insert(buf.end(), tmp, tmp + len);
      buf.insert(buf.end(), dim.suffixString.begin(), dim.suffixString.end());
    } else if (token.is<Token::Percentage>()) {
      char tmp[32];
      const double val = token.get<Token::Percentage>().value;
      int len;
      if (val == std::floor(val) && std::abs(val) < 1e15) {
        len = std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(val));
      } else {
        len = std::snprintf(tmp, sizeof(tmp), "%g", val);
      }
      buf.insert(buf.end(), tmp, tmp + len);
      buf.push_back('%');
    } else if (token.is<Token::String>()) {
      buf.push_back('"');
      const auto& str = token.get<Token::String>().value;
      buf.insert(buf.end(), str.begin(), str.end());
      buf.push_back('"');
    } else if (token.is<Token::Hash>()) {
      buf.push_back('#');
      const auto& str = token.get<Token::Hash>().name;
      buf.insert(buf.end(), str.begin(), str.end());
    } else if (token.is<Token::Comma>()) {
      buf.push_back(',');
    } else if (token.is<Token::Whitespace>()) {
      buf.push_back(' ');
    } else if (token.is<Token::Delim>()) {
      buf.push_back(token.get<Token::Delim>().value);
    }
  } else if (cv.is<Function>()) {
    const Function& func = cv.get<Function>();
    buf.insert(buf.end(), func.name.begin(), func.name.end());
    buf.push_back('(');
    for (size_t i = 0; i < func.values.size(); ++i) {
      if (i > 0) {
        buf.push_back(' ');
      }
      serializeComponentValue(buf, func.values[i]);
    }
    buf.push_back(')');
  }
}

}  // namespace

RcString Declaration::toRcString() const {
  std::vector<char> buf;
  buf.reserve(name.size() + 32);

  buf.insert(buf.end(), name.begin(), name.end());
  buf.push_back(':');

  for (const auto& cv : values) {
    buf.push_back(' ');
    serializeComponentValue(buf, cv);
  }

  if (important) {
    const std::string_view imp = " !important";
    buf.insert(buf.end(), imp.begin(), imp.end());
  }

  return RcString::fromVector(std::move(buf));
}

DeclarationOrAtRule::DeclarationOrAtRule(DeclarationOrAtRule::Type&& value)
    : value(std::move(value)) {}

bool DeclarationOrAtRule::operator==(const DeclarationOrAtRule& other) const {
  return value == other.value;
}

std::ostream& operator<<(std::ostream& os, const Declaration& declaration) {
  os << "  " << declaration.name << ":";
  for (const auto& value : declaration.values) {
    os << " " << value;
  }
  if (declaration.important) {
    os << " !important";
  }
  return os;
}

}  // namespace donner::css
