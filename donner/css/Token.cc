#include "donner/css/Token.h"

namespace donner::css {

// The special members are defaulted here rather than in the header so the
// 26-alternative `std::variant` copy/move/destroy machinery is instantiated once,
// in this translation unit, instead of in every TU that includes Token.h.
// See the declarations in Token.h for the rationale.

static_assert(std::is_nothrow_move_constructible_v<Token::TokenValue>,
              "TokenValue must be nothrow-move-constructible: Token's out-of-line move "
              "constructor is declared noexcept, and containers rely on noexcept moves.");
static_assert(std::is_nothrow_move_assignable_v<Token::TokenValue>,
              "TokenValue must be nothrow-move-assignable: Token's out-of-line move "
              "assignment is declared noexcept.");

Token::Token(const Token& other) = default;
Token::Token(Token&& other) noexcept = default;
Token& Token::operator=(const Token& other) = default;
Token& Token::operator=(Token&& other) noexcept = default;
Token::~Token() = default;

bool Token::isParseError() const {
  switch (value_.index()) {
    case indexOf<BadUrl>(): [[fallthrough]];
    case indexOf<BadString>(): [[fallthrough]];
    case indexOf<CloseParenthesis>(): [[fallthrough]];
    case indexOf<CloseSquareBracket>(): [[fallthrough]];
    case indexOf<CloseCurlyBracket>(): return true;
    default: return false;
  }
}

std::string Token::toCssText() const {
  return visit([](auto&& t) -> std::string {
    using T = std::remove_cvref_t<decltype(t)>;

    if constexpr (std::is_same_v<T, Token::Ident>) {
      return std::string(t.value);
    } else if constexpr (std::is_same_v<T, Token::Function>) {
      return std::string(t.name) + "(";
    } else if constexpr (std::is_same_v<T, Token::AtKeyword>) {
      return "@" + std::string(t.value);
    } else if constexpr (std::is_same_v<T, Token::Hash>) {
      return "#" + std::string(t.name);
    } else if constexpr (std::is_same_v<T, Token::String>) {
      return "\"" + std::string(t.value) + "\"";
    } else if constexpr (std::is_same_v<T, Token::Url>) {
      return "url(" + std::string(t.value) + ")";
    } else if constexpr (std::is_same_v<T, Token::Delim>) {
      return std::string(1, t.value);
    } else if constexpr (std::is_same_v<T, Token::Number>) {
      return std::string(t.valueString);
    } else if constexpr (std::is_same_v<T, Token::Percentage>) {
      return std::string(t.valueString) + "%";
    } else if constexpr (std::is_same_v<T, Token::Dimension>) {
      return std::string(t.valueString) + std::string(t.suffixString);
    } else if constexpr (std::is_same_v<T, Token::Whitespace>) {
      return " ";
    } else if constexpr (std::is_same_v<T, Token::CDO>) {
      return "<!--";
    } else if constexpr (std::is_same_v<T, Token::CDC>) {
      return "-->";
    } else if constexpr (std::is_same_v<T, Token::Colon>) {
      return ":";
    } else if constexpr (std::is_same_v<T, Token::Semicolon>) {
      return ";";
    } else if constexpr (std::is_same_v<T, Token::Comma>) {
      return ",";
    } else if constexpr (std::is_same_v<T, Token::SquareBracket>) {
      return "[";
    } else if constexpr (std::is_same_v<T, Token::Parenthesis>) {
      return "(";
    } else if constexpr (std::is_same_v<T, Token::CurlyBracket>) {
      return "{";
    } else if constexpr (std::is_same_v<T, Token::CloseSquareBracket>) {
      return "]";
    } else if constexpr (std::is_same_v<T, Token::CloseParenthesis>) {
      return ")";
    } else if constexpr (std::is_same_v<T, Token::CloseCurlyBracket>) {
      return "}";
    } else {
      // BadString, BadUrl, ErrorToken, EofToken
      return "";
    }
  });
}

bool Token::operator==(const Token& other) const {
  return value_ == other.value_ && offset_ == other.offset_;
}

std::ostream& operator<<(std::ostream& os, const Token& token) {
  os << "Token { ";
  token.visit([&os](auto&& value) { os << value; });
  os << " offset: " << token.offset();
  os << " }";
  return os;
}

}  // namespace donner::css
