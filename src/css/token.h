#pragma once

#include <ostream>
#include <string_view>
#include <variant>

#include "src/base/length.h"
#include "src/base/rc_string.h"

namespace donner::css {

using TokenIndex = size_t;

enum class NumberType { Integer, Number };

struct Token {
  /// `<ident-token>`
  struct Ident {
    explicit Ident(RcString value) : value(std::move(value)) {}

    bool operator==(const Ident& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Ident& obj) {
      return os << "Ident(" << obj.value << ")";
    }

    RcString value;
  };

  /// `<function-token>`
  struct Function {
    explicit Function(RcString name) : name(std::move(name)) {}

    bool operator==(const Function& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Function& obj) {
      return os << "Function(" << obj.name << ")";
    }

    /// Does not include the '(' character.
    RcString name;
  };

  /// `<at-keyword-token>`
  struct AtKeyword {
    explicit AtKeyword(RcString value) : value(std::move(value)) {}

    bool operator==(const AtKeyword& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const AtKeyword& obj) {
      return os << "AtKeyword(" << obj.value << ")";
    }

    /// The value, not including the '@' character.
    RcString value;
  };

  /// `<hash-token>`
  struct Hash {
    enum class Type { Unrestricted, Id };

    Hash(Type type, RcString name) : type(type), name(std::move(name)) {}

    bool operator==(const Hash& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Hash& obj) {
      return os << "Hash(" << (obj.type == Hash::Type::Unrestricted ? "unrestricted" : "id") << ": "
                << obj.name << ")";
    }

    /// Hash type, defaults to unrestricted if not otherwise set.
    Type type;

    /// The name, not including the '#' character.
    RcString name;
  };

  /// `<string-token>`
  struct String {
    explicit String(RcString value) : value(std::move(value)) {}

    bool operator==(const String& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const String& obj) {
      return os << "String(\"" << obj.value << "\")";
    }

    RcString value;
  };

  /// `<bad-string-token>`
  struct BadString {
    explicit BadString(RcString value) : value(std::move(value)) {}

    bool operator==(const BadString& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const BadString& obj) {
      return os << "BadString(\"" << obj.value << "\")";
    }

    RcString value;
  };

  /// `<url-token>`
  struct Url {
    explicit Url(RcString value) : value(std::move(value)) {}

    bool operator==(const Url& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Url& obj) {
      return os << "Url(" << obj.value << ")";
    }

    RcString value;
  };

  /// `<bad-url-token>`
  struct BadUrl {
    bool operator==(const BadUrl&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const BadUrl&) { return os << "BadUrl"; }
  };

  /// `<delim-token>`
  struct Delim {
    explicit Delim(char value) : value(value) {}

    bool operator==(const Delim& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Delim& obj) {
      return os << "Delim(" << obj.value << ")";
    }

    char value;
  };

  /// `<number-token>`
  struct Number {
    Number(double value, RcString valueString, NumberType type)
        : value(value), valueString(std::move(valueString)), type(type) {}

    bool operator==(const Number& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Number& obj) {
      return os << "Number(" << obj.value << ", str='" << obj.valueString << "', "
                << (obj.type == NumberType::Integer ? "integer" : "number") << ")";
    }

    double value;
    RcString valueString;
    NumberType type;
  };

  /// `<percentage-token>`
  struct Percentage {
    Percentage(double value, RcString valueString, NumberType type)
        : value(value), valueString(std::move(valueString)), type(type) {}

    bool operator==(const Percentage& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Percentage& obj) {
      return os << "Percentage(" << obj.value << ", str='" << obj.valueString << "', "
                << (obj.type == NumberType::Integer ? "integer" : "number") << ")";
    }

    double value;  //< The percentage multiplied by 100, 100% -> 100.0
    RcString valueString;
    NumberType type;
  };

  /// `<dimension-token>`
  struct Dimension {
    Dimension(double value, RcString suffix, std::optional<Lengthd::Unit> suffixUnit,
              RcString valueString, NumberType type)
        : value(value),
          suffix(std::move(suffix)),
          suffixUnit(suffixUnit),
          valueString(std::move(valueString)),
          type(type) {}

    bool operator==(const Dimension& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Dimension& obj) {
      return os << "Dimension(" << obj.value << obj.suffix << ", str='" << obj.valueString << "', "
                << (obj.type == NumberType::Integer ? "integer" : "number") << ")";
    }

    double value;
    RcString suffix;
    std::optional<Lengthd::Unit> suffixUnit;  //!< The parsed unit of the suffix, if known.
    RcString valueString;
    NumberType type;
  };

  /// `<whitespace-token>`
  struct Whitespace {
    explicit Whitespace(RcString value) : value(std::move(value)) {}

    bool operator==(const Whitespace& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Whitespace& obj) {
      return os << "Whitespace('" << obj.value << "', len=" << obj.value.size() << ")";
    }

    RcString value;
  };

  /// `<CDO-token>`, '<!--'
  struct CDO {
    bool operator==(const CDO&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const CDO&) { return os << "CDO"; }
  };

  /// `<CDC-token>`, '-->'
  struct CDC {
    bool operator==(const CDC&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const CDC&) { return os << "CDC"; }
  };

  /// `<colon-token>`
  struct Colon {
    bool operator==(const Colon&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const Colon&) { return os << "Colon"; }
  };

  /// `<semicolon-token>`
  struct Semicolon {
    bool operator==(const Semicolon&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const Semicolon&) {
      return os << "Semicolon";
    }
  };

  /// `<comma-token>`
  struct Comma {
    bool operator==(const Comma&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const Comma&) {
      os << "Comma";
      return os;
    }
  };

  /// `<[-token>`
  struct SquareBracket {
    bool operator==(const SquareBracket&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const SquareBracket&) {
      return os << "SquareBracket";
    }
  };

  /// `<(-token>`
  struct Parenthesis {
    bool operator==(const Parenthesis&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const Parenthesis&) {
      return os << "Parenthesis";
    }
  };

  /// `<{-token>`
  struct CurlyBracket {
    bool operator==(const CurlyBracket&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const CurlyBracket&) {
      return os << "CurlyBracket";
    }
  };

  /// `<]-token>`
  struct CloseSquareBracket {
    bool operator==(const CloseSquareBracket&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const CloseSquareBracket&) {
      return os << "CloseSquareBracket";
    }
  };

  /// `<)-token>`
  struct CloseParenthesis {
    bool operator==(const CloseParenthesis&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const CloseParenthesis&) {
      return os << "CloseParenthesis";
    }
  };

  /// `<}-token>`
  struct CloseCurlyBracket {
    bool operator==(const CloseCurlyBracket&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const CloseCurlyBracket&) {
      return os << "CloseCurlyBracket";
    }
  };

  /// Special error token, used to mark named parsing errors.
  struct ErrorToken {
    enum class Type { EofInString, EofInComment, EofInUrl };

    explicit ErrorToken(Type type) : type(type) {}

    bool operator==(const ErrorToken& other) const = default;
    friend std::ostream& operator<<(std::ostream& os, const ErrorToken& obj) {
      os << "ErrorToken(";
      switch (obj.type) {
        case Type::EofInString: os << "EofInString"; break;
        case Type::EofInComment: os << "EofInComment"; break;
        case Type::EofInUrl: os << "EofInUrl"; break;
      }
      return os << ")";
    }

    Type type;
  };

  /// `<EOF-token>`.
  struct EofToken {
    bool operator==(const EofToken&) const { return true; }
    friend std::ostream& operator<<(std::ostream& os, const EofToken&) { return os << "EofToken"; }
  };

  using TokenValue =
      std::variant<Ident, Function, AtKeyword, Hash, String, BadString, Url, BadUrl, Delim, Number,
                   Percentage, Dimension, Whitespace, CDO, CDC, Colon, Semicolon, Comma,
                   SquareBracket, Parenthesis, CurlyBracket, CloseSquareBracket, CloseParenthesis,
                   CloseCurlyBracket, ErrorToken, EofToken>;

  Token(TokenValue&& value, size_t offset) : value_(std::move(value)), offset_(offset) {}

  TokenIndex tokenIndex() const { return value_.index(); }
  size_t offset() const { return offset_; }

  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(value_);
  }

  template <typename T>
  T& get() & {
    return std::get<T>(value_);
  }

  template <typename T>
  const T& get() const& {
    return std::get<T>(value_);
  }

  template <typename T>
  T&& get() && {
    return std::move(std::get<T>(value_));
  }

  template <typename T>
  T* tryGet() {
    return std::get_if<T>(&value_);
  }

  template <typename T>
  const T* tryGet() const {
    return std::get_if<T>(&value_);
  }

  template <typename Visitor>
  auto visit(Visitor&& visitor) const {
    return std::visit(std::forward<Visitor>(visitor), value_);
  }

  template <typename T, TokenIndex index = 0>
  static constexpr TokenIndex indexOf() {
    if constexpr (index == std::variant_size_v<TokenValue>) {
      return index;
    } else if constexpr (std::is_same_v<std::variant_alternative_t<index, TokenValue>, T>) {
      return index;
    } else {
      return indexOf<T, index + 1>();
    }
  }

  bool isParseError() const {
    switch (value_.index()) {
      case indexOf<BadUrl>():
      case indexOf<BadString>():
      case indexOf<CloseParenthesis>():
      case indexOf<CloseSquareBracket>():
      case indexOf<CloseCurlyBracket>(): return true;
    }

    return false;
  }

  bool operator==(const Token& other) const {
    return value_ == other.value_ && offset_ == other.offset_;
  }

  friend std::ostream& operator<<(std::ostream& os, const Token& token) {
    os << "Token { ";
    token.visit([&os](auto&& value) { os << value; });
    os << " offset: " << token.offset();
    os << " }";
    return os;
  }

private:
  TokenValue value_;
  size_t offset_;
};

}  // namespace donner::css
