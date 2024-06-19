#pragma once
/// @file

#include <optional>
#include <ostream>
#include <variant>

#include "src/base/length.h"
#include "src/base/parser/file_offset.h"
#include "src/base/rc_string.h"

namespace donner::css {

/**
 * Type of the token unique identifier, which is returned by \ref Token::tokenIndex() and \ref
 * Token::indexOf<T>().
 */
using TokenIndex = size_t;

/**
 * Indicates if a number is an integer or a floating point number, used for number-containing tokens
 * such as \ref Token::Number and \ref Token::Dimension.
 */
enum class NumberType {
  Integer,  ///< Integer number (no decimal point).
  Number,   ///< Floating point number.
};

/**
 * A CSS token, which are created as a first step when parsing a CSS string. See
 * https://www.w3.org/TR/css-syntax-3/#tokenization for more details.
 */
struct Token {
  /**
   * @defgroup css_tokens CSS Tokens
   *
   * Raw CSS tokens, which are created as a first step when parsing a CSS string. See
   * https://www.w3.org/TR/css-syntax-3/#tokenization for more details.
   *
   * Tokens are created by \ref donner::css::details::Tokenizer, which is automatically used within
   * the CSS parser suite.
   *
   * A full list of tokens can be found in the \ref TokenValue variant list.
   *
   * @{
   */

  /**
   * `<ident-token>`, which represents a CSS identifier, which is an unquoted string. For example,
   * `div`, `color`, `red` are all identifiers.
   *
   * See https://www.w3.org/TR/css-syntax-3/#ident-token-diagram for the railroad diagram.
   *
   * - Identifiers can begin with `a-zA-Z_`, non-ascii, `-`, or `--`.
   * - After the first character, they contain `a-zA-Z0-9_-`, non-ascii, or escape sequences such as
   *   `\u1234`.
   */
  struct Ident {
    /**
     * Create an identifier token.
     *
     * @param value Identifier value.
     */
    explicit Ident(RcString value) : value(std::move(value)) {}

    /// Equality operator.
    bool operator==(const Ident& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Ident(red)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Ident& obj) {
      return os << "Ident(" << obj.value << ")";
    }

    RcString value;  ///< Identifier value.
  };

  /**
   * `<function-token>`, which indicates the start of a function call. For `rgb(255, 0, 0)`, the
   * function token would be created for the `rgb(` part, and the name would be `rgb`.
   *
   * Note that for `url`, an unquoted `url(foo)` is parsed as a \ref Url, while a quoted value like
   * `url("foo")` is parsed as a Function token.
   */
  struct Function {
    /**
     * Create a Function with the given name.
     *
     * @param name Function name, not including the '(' character.
     */
    explicit Function(RcString name) : name(std::move(name)) {}

    /// Equality operator.
    bool operator==(const Function& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Function(url)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Function& obj) {
      return os << "Function(" << obj.name << ")";
    }

    /// Function name, not including the '(' character.
    RcString name;
  };

  /**
   * `<at-keyword-token>`, representing `@` followed by an identifier. For example, `@media` and
   * `@import`.
   *
   * See https://www.w3.org/TR/css-syntax-3/#at-keyword-token-diagram for the railroad diagram.
   */
  struct AtKeyword {
    /**
     * Create an AtKeyword with the given value.
     *
     * @param value Value, not including the '@' character.
     */
    explicit AtKeyword(RcString value) : value(std::move(value)) {}

    /// Equality operator.
    bool operator==(const AtKeyword& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * AtKeyword(media)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const AtKeyword& obj) {
      return os << "AtKeyword(" << obj.value << ")";
    }

    /// The value, not including the '@' character.
    RcString value;
  };

  /**
   * `<hash-token>`, representing a CSS identifier that starts with a `#`. For example, `#foo` and
   * `#fff`.
   *
   * See https://www.w3.org/TR/css-syntax-3/#hash-token-diagram for the railroad diagram.
   */
  struct Hash {
    /**
     * Hash type, which is set to \ref Type::Unrestricted by default, and \ref Type::Id if the hash
     * would be a valid identifier per the rules on \ref Token::Ident.
     */
    enum class Type {
      Unrestricted,  ///< The default type.
      Id,  ///< The hash value is a valid identifier, starting with `a-zA-Z_`, non-ascii, `-`, or
           ///< `--`.
    };

    /**
     * Create a Hash token.
     *
     * @param type Hash type, which should be \ref Type::Unrestricted by default.
     * @param name Hash name, not including the '#' character.
     */
    Hash(Type type, RcString name) : type(type), name(std::move(name)) {}

    /// Equality operator.
    bool operator==(const Hash& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Hash(unrestricted: foo)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Hash& obj) {
      return os << "Hash(" << (obj.type == Hash::Type::Unrestricted ? "unrestricted" : "id") << ": "
                << obj.name << ")";
    }

    /// Hash type, defaults to unrestricted if not otherwise set.
    Type type;

    /// The name, not including the '#' character.
    RcString name;
  };

  /**
   * `<string-token>`, which represents a quoted string, either with double or single quotes
   * (`"foo"` or `'foo'`).
   */
  struct String {
    /**
     * Create a String token.
     *
     * @param value String value, not including quotes.
     */
    explicit String(RcString value) : value(std::move(value)) {}

    /// Equality operator.
    bool operator==(const String& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * String("foo")
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const String& obj) {
      return os << "String(\"" << obj.value << "\")";
    }

    /// String value, not including quotes.
    RcString value;
  };

  /**
   * `<bad-string-token>`, which is generated when a string contains an unescaped newline.
   *
   * For example, tokenizing `'foo\\nbar'` would result in a \ref BadString token with value `foo`.
   */
  struct BadString {
    /**
     * Create a BadString token.
     *
     * @param value The part of the string parsed before hitting the unescaped newline, not
     * including the newline.
     */
    explicit BadString(RcString value) : value(std::move(value)) {}

    /// Equality operator.
    bool operator==(const BadString& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * BadString("foo")
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const BadString& obj) {
      return os << "BadString(\"" << obj.value << "\")";
    }

    /// Valid part of the string before parsing hit an unescaped newline.
    RcString value;
  };

  /**
   * `<url-token>`, which represents a `url()` function. For example, `url(foo.png)`, where the Url
   * value is 'foo.png'.
   *
   * NOTE: This expects that the contents of the `url()` are not quoted, if they are quoted this
   * will tokenize as a \ref Function instead.
   */
  struct Url {
    /**
     * Create a Url token.
     *
     * @param value Url value, not including the surrounding 'url(' and ')' characters.
     */
    explicit Url(RcString value) : value(std::move(value)) {}

    /// Equality operator.
    bool operator==(const Url& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Url(foo.png)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Url& obj) {
      return os << "Url(" << obj.value << ")";
    }

    /// Url value, not including the surrounding 'url(' and ')' characters.
    RcString value;
  };

  /**
   * `<bad-url-token>`, which represents an invalid `url()` function. For example, `url(whitespace
   * in middle)`, `url(()` or `url(not\u001Fprintable)` will all result in a \ref BadUrl token.
   *
   * Bad URLs may be created when:
   * - There is whitespace in the middle of the url, such as `url(foo bar.png)`.
   * - There is an extra '(' in the URL, such as `url(foo(bar.png)`.
   * - There is a non-printable character in the URL, such as `url(foo\u001Fbar.png)`.
   *
   * NOTE: This expects that the contents of the `url()` are not quoted, if they are quoted this
   * will tokenize as a \ref Function instead.
   */
  struct BadUrl {
    /// Equality operator.
    bool operator==(const BadUrl&) const { return true; }

    /// Ostream output operator, which always outputs "BadUrl".
    friend std::ostream& operator<<(std::ostream& os, const BadUrl&) { return os << "BadUrl"; }
  };

  /**
   * `<delim-token>`, which contains a single character. These are typically symbol characters, such
   * as `+`, `-`, `*`, '.', '!', etc, since other characters would create a \ref Token::Ident.
   *
   * Delim tokens include the `!` on `!important` rules and combinators in selector lists, such as
   * `>` in `parent > child`.
   */
  struct Delim {
    /**
     * Create a Delim token.
     *
     * @param value The delim character value.
     */
    explicit Delim(char value) : value(value) {}

    /// Equality operator.
    bool operator==(const Delim& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Delim(+)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Delim& obj) {
      return os << "Delim(" << obj.value << ")";
    }

    char value;  ///< The delim character value.
  };

  /**
   * `<number-token>`, which represents a number, either integer or floating point. The token
   * captures both the parsed number and the original string value.
   */
  struct Number {
    /**
     * Create a Number token.
     *
     * @param value The parsed number value.
     * @param valueString The original string value.
     * @param type The type of number, either integer or floating point.
     */
    Number(double value, RcString valueString, NumberType type)
        : value(value), valueString(std::move(valueString)), type(type) {}

    /// Equality operator.
    bool operator==(const Number& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Number(1.5, str='1.5', number)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Number& obj) {
      return os << "Number(" << obj.value << ", str='" << obj.valueString << "', "
                << (obj.type == NumberType::Integer ? "integer" : "number") << ")";
    }

    double value;          ///< The parsed number value.
    RcString valueString;  ///< The original string value.
    NumberType type;       ///< The type of number, either integer or floating point.
  };

  /**
   * `<percentage-token>`, which represents a percentage such as '50%'. The token's value scaled so
   * that 100 is equivalent to '100%'.
   */
  struct Percentage {
    /**
     * Create a Percentage token.
     *
     * @param value The percentage multiplied by 100, 100% -> 100.0
     * @param valueString The original string value.
     * @param type The type of number, either integer or floating point.
     */
    Percentage(double value, RcString valueString, NumberType type)
        : value(value), valueString(std::move(valueString)), type(type) {}

    /// Equality operator.
    bool operator==(const Percentage& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Percentage(100, str='100%', integer)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Percentage& obj) {
      return os << "Percentage(" << obj.value << ", str='" << obj.valueString << "', "
                << (obj.type == NumberType::Integer ? "integer" : "number") << ")";
    }

    double value;          ///< The percentage multiplied by 100, 100% -> 100.0
    RcString valueString;  ///< The original string value.
    NumberType type;       ///< The type of number, either integer or floating point.
  };

  /**
   * `<dimension-token>`, which represents a dimension such as '50px'. The token contains the parsed
   * number, parsed unit (if it is a known suffix), as well as the raw strings for both the number
   * and unit suffix.
   */
  struct Dimension {
    /**
     * Create a Dimension token.
     *
     * @param value The parsed number value.
     * @param suffixString The unit suffix, such as 'px'.
     * @param suffixUnit The parsed unit, if it is a known suffix.
     * @param valueString The original string value.
     * @param type The type of number, either integer or floating point.
     */
    Dimension(double value, RcString suffixString, std::optional<Lengthd::Unit> suffixUnit,
              RcString valueString, NumberType type)
        : value(value),
          suffixString(std::move(suffixString)),
          suffixUnit(suffixUnit),
          valueString(std::move(valueString)),
          type(type) {}

    /// Equality operator.
    bool operator==(const Dimension& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Dimension(1.5px, str='1.5px', number)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Dimension& obj) {
      return os << "Dimension(" << obj.value << obj.suffixString << ", str='" << obj.valueString
                << "', " << (obj.type == NumberType::Integer ? "integer" : "number") << ")";
    }

    double value;           ///< The parsed number value.
    RcString suffixString;  //!< Raw string of the unit suffix, e.g. 'px'.
    std::optional<Lengthd::Unit>
        suffixUnit;  //!< The parsed unit of the suffix, if known. If the input string has an
                     //!< invalid suffix, and \ref LengthParser failed to identify it, this will
                     //!< `std::nullopt`.
    RcString valueString;  ///< The original string of the \ref value number.
    NumberType type;       ///< The type of number, either integer or floating point.
  };

  /**
   * `<whitespace-token>`, which contains one or more whitespace characters in the source. These
   * include ' ', '\\t', '\\n', '\\r\\n', '\\r', and '\\f'.
   *
   * See https://www.w3.org/TR/css-syntax-3/#whitespace for the railroad diagram.
   */
  struct Whitespace {
    /**
     * Create a Whitespace token.
     *
     * @param value The whitespace characters.
     */
    explicit Whitespace(RcString value) : value(std::move(value)) {}

    /// Equality operator.
    bool operator==(const Whitespace& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * Whitespace(' ', len=1)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const Whitespace& obj) {
      return os << "Whitespace('" << obj.value << "', len=" << obj.value.size() << ")";
    }

    RcString value;  ///< The whitespace characters.
  };

  /**
   * `<CDO-token>`, which represents `<!--` in the source.
   */
  struct CDO {
    /// Equality operator.
    bool operator==(const CDO&) const { return true; }

    /// Ostream output operator, which outputs `CDO`.
    friend std::ostream& operator<<(std::ostream& os, const CDO&) { return os << "CDO"; }
  };

  /**
   * `<CDC-token>`, which represents `-->` in the source.
   */
  struct CDC {
    /// Equality operator.
    bool operator==(const CDC&) const { return true; }

    /// Ostream output operator, which outputs `CDC`.
    friend std::ostream& operator<<(std::ostream& os, const CDC&) { return os << "CDC"; }
  };

  /// `<colon-token>`, which represents ':' in the source.
  struct Colon {
    /// Equality operator.
    bool operator==(const Colon&) const { return true; }

    /// Ostream output operator, which outputs `Colon`.
    friend std::ostream& operator<<(std::ostream& os, const Colon&) { return os << "Colon"; }
  };

  /**
   * `<semicolon-token>`, which represents ';' in the source.
   */
  struct Semicolon {
    /// Equality operator.
    bool operator==(const Semicolon&) const { return true; }

    /// Ostream output operator, which outputs `Semicolon`.
    friend std::ostream& operator<<(std::ostream& os, const Semicolon&) {
      return os << "Semicolon";
    }
  };

  /**
   * `<comma-token>`, which represents ',' in the source.
   */
  struct Comma {
    /// Equality operator.
    bool operator==(const Comma&) const { return true; }

    /// Ostream output operator, which outputs `Comma`.
    friend std::ostream& operator<<(std::ostream& os, const Comma&) {
      os << "Comma";
      return os;
    }
  };

  /**
   * `<[-token>`, which represents `[` in the source.
   */
  struct SquareBracket {
    /// Equality operator.
    bool operator==(const SquareBracket&) const { return true; }

    /// Ostream output operator, which outputs `SquareBracket`.
    friend std::ostream& operator<<(std::ostream& os, const SquareBracket&) {
      return os << "SquareBracket";
    }
  };

  /**
   * `<(-token>`, which represents `(` in the source.
   */
  struct Parenthesis {
    /// Equality operator.
    bool operator==(const Parenthesis&) const { return true; }

    /// Ostream output operator, which outputs `Parenthesis`.
    friend std::ostream& operator<<(std::ostream& os, const Parenthesis&) {
      return os << "Parenthesis";
    }
  };

  /**
   * `<{-token>`, which represents `{` in the source.
   */
  struct CurlyBracket {
    /// Equality operator.
    bool operator==(const CurlyBracket&) const { return true; }

    /// Ostream output operator, which outputs `CurlyBracket`.
    friend std::ostream& operator<<(std::ostream& os, const CurlyBracket&) {
      return os << "CurlyBracket";
    }
  };

  /**
   * `<]-token>`, which represents `]` in the source.
   */
  struct CloseSquareBracket {
    /// Equality operator.
    bool operator==(const CloseSquareBracket&) const { return true; }

    /// Ostream output operator, which outputs `CloseSquareBracket`.
    friend std::ostream& operator<<(std::ostream& os, const CloseSquareBracket&) {
      return os << "CloseSquareBracket";
    }
  };

  /**
   * `<)-token>`, which represents `)` in the source.
   */
  struct CloseParenthesis {
    /// Equality operator.
    bool operator==(const CloseParenthesis&) const { return true; }

    /// Ostream output operator, which outputs `CloseParenthesis`.
    friend std::ostream& operator<<(std::ostream& os, const CloseParenthesis&) {
      return os << "CloseParenthesis";
    }
  };

  /**
   * `<}-token>`, which represents `}` in the source.
   */
  struct CloseCurlyBracket {
    /// Equality operator.
    bool operator==(const CloseCurlyBracket&) const { return true; }

    /// Ostream output operator, which outputs `CloseCurlyBracket`.
    friend std::ostream& operator<<(std::ostream& os, const CloseCurlyBracket&) {
      return os << "CloseCurlyBracket";
    }
  };

  /**
   * Special error token, used to mark named parsing errors.
   */
  struct ErrorToken {
    /// Error type.
    enum class Type {
      EofInString,   ///< An EOF was hit when parsing a quoted string, such as `"string<eof>`.
      EofInComment,  ///< An EOF was hit when parsing a comment, such as `/* comment<eof>`.
      EofInUrl       ///< An EOF was hit when parsing a URL, such as `url(<eof>`.
    };

    /**
     * @brief Construct a new Error Token object
     *
     * @param type Error type.
     */
    explicit ErrorToken(Type type) : type(type) {}

    /// Equality operator.
    bool operator==(const ErrorToken& other) const = default;

    /**
     * Ostream output operator.
     *
     * Example output:
     * ```
     * ErrorToken(EofInString)
     * ```
     *
     * @param os Output stream.
     * @param obj Object to output.
     */
    friend std::ostream& operator<<(std::ostream& os, const ErrorToken& obj) {
      os << "ErrorToken(";
      switch (obj.type) {
        case Type::EofInString: os << "EofInString"; break;
        case Type::EofInComment: os << "EofInComment"; break;
        case Type::EofInUrl: os << "EofInUrl"; break;
      }
      return os << ")";
    }

    Type type;  ///< Error type.
  };

  /**
   * `<EOF-token>`, which marks the end of the input stream and is always output at the end of a
   * token list.
   */
  struct EofToken {
    /// Equality operator.
    bool operator==(const EofToken&) const { return true; }

    /// Ostream output operator, which outputs `EofToken`.
    friend std::ostream& operator<<(std::ostream& os, const EofToken&) { return os << "EofToken"; }
  };

  /** @} */

  /**
   * Variant containing all supported token types.
   */
  using TokenValue =
      std::variant<Ident, Function, AtKeyword, Hash, String, BadString, Url, BadUrl, Delim, Number,
                   Percentage, Dimension, Whitespace, CDO, CDC, Colon, Semicolon, Comma,
                   SquareBracket, Parenthesis, CurlyBracket, CloseSquareBracket, CloseParenthesis,
                   CloseCurlyBracket, ErrorToken, EofToken>;

  /**
   * Construct a new Token object, taking ownership of a \ref TokenValue, at a given \p offset
   * within the source string.
   *
   * This allows creating a Token from any \ref css_tokens documented above.
   *
   * For example, to create a token of a given type:
   * ```
   * auto token = Token(Token::String("test"), 0);
   * ```
   *
   * The tokenizer automatically creates tokens using this API.
   *
   * @param value Token value.
   * @param offset Offset within the source string where this token starts.
   */
  Token(TokenValue&& value, size_t offset) : value_(std::move(value)), offset_(offset) {}

  /**
   * Returns the token type.
   *
   * For example, to compare this token type against a known type:
   * ```
   * if (token.tokenIndex() == Token::indexOf<Token::Ident>()) {
   *  // ...
   * }
   * ```
   */
  TokenIndex tokenIndex() const { return value_.index(); }

  /**
   * Returns the offset within the source string where this token starts.
   */
  parser::FileOffset offset() const { return parser::FileOffset::Offset(offset_); }

  /**
   * Check if the token is of the given type.
   *
   * Example usage:
   * ```
   * if (token.is<Token::Ident>()) {
   *   const Token::Ident& ident = token.get<Token::Ident>();
   *   // ...
   * }
   * ```
   *
   * @tparam T Token type, which must be one of the values in the \ref TokenValue variant.
   */
  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(value_);
  }

  /**
   * Get the token value as a reference.
   *
   * Example usage:
   * ```
   * Token::Ident& ident = token.get<Token::Ident>();
   * ```
   *
   * @tparam T Token type, which must be one of the values in the \ref TokenValue variant.
   * @pre The token must be of the given type, i.e. `is<T>()` must be true.
   */
  template <typename T>
  T& get() & {
    return std::get<T>(value_);
  }

  /**
   * Get the token value as a const-reference.
   *
   * Example usage:
   * ```
   * const RcString value = token.get<Token::Ident>().value;
   * ```
   *
   * @tparam T Token type, which must be one of the values in the \ref TokenValue variant.
   * @pre The token must be of the given type, i.e. `is<T>()` must be true.
   */
  template <typename T>
  const T& get() const& {
    return std::get<T>(value_);
  }

  /**
   * Get the token value as an rvalue-reference for move semantics.
   *
   * Example usage:
   * ```
   * Token::Ident ident = std::move(token.get<Token::Ident>());
   * ```
   *
   * @tparam T Token type, which must be one of the values in the \ref TokenValue variant.
   * @pre The token must be of the given type, i.e. `is<T>()` must be true.
   */
  template <typename T>
  T&& get() && {
    return std::move(std::get<T>(value_));
  }

  /**
   * Get the token value as a pointer, or `nullptr` if the token is not of the given type.
   * This is a convenience method for `is<T>() ? &get<T>() : nullptr`.
   *
   * Example usage:
   * ```
   * if (Token::Ident* ident = token.tryGet<Token::Ident>()) {
   *  // ident is mutable here.
   *  // ...
   * }
   * ```
   *
   * @tparam T Token type, which must be one of the values in the \ref TokenValue variant.
   */
  template <typename T>
  T* tryGet() {
    return std::get_if<T>(&value_);
  }

  /**
   * Get the token value as a const-pointer, or `nullptr` if the token is not of the given type.
   * This is a convenience method for `is<T>() ? &get<T>() : nullptr`.
   *
   * Example usage:
   * ```
   * if (const Token::Ident* ident = token.tryGet<Token::Ident>()) {
   *  // ...
   * }
   * ```
   *
   * @tparam T Token type, which must be one of the values in the \ref TokenValue variant.
   */
  template <typename T>
  const T* tryGet() const {
    return std::get_if<T>(&value_);
  }

  /**
   * Visit the token value using a visitor, which internally uses `std::visit`.
   *
   * For example:
   * ```
   * token.visit([](auto&& t) {
   *   using Type = std::remove_cvref_t<decltype(t)>;
   *
   *  if constexpr (std::is_same_v<Type, Token::Ident>) {
   *    // t is an Ident
   *  } else if constexpr (std::is_same_v<Type, Token::Function>) {
   *    // ...
   *  }
   * });
   * ```
   *
   * @tparam Visitor Visitor type, which should be a callable type that accepts a reference to the
   *   token, for example: `void(auto&&)`.
   * @param visitor Visitor callback, which will be invoked with the token as a parameter. For
   *   example: `[](auto&& t) { ... }`.
   */
  template <typename Visitor>
  auto visit(Visitor&& visitor) const {
    return std::visit(std::forward<Visitor>(visitor), value_);
  }

  /**
   * At compile-time, return the TokenIndex of a given token type, which can be used to uniquely
   * identify a token.
   *
   * Example usage:
   * ```
   * const TokenIndex index = Token::indexOf<Token::Ident>();
   * ```
   *
   * @tparam T Token class, such as `Token::Ident` or `Token::Function`. For a full list of valid
   *   values, see the members of the \ref TokenValue variant.
   * @tparam index Internally used to track the current index, do not manually specify.
   * @return constexpr TokenIndex The index of the given token type.
   */
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

  /**
   * Returns true if this token is a type of parse error.
   */
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

  /// Equality operator.
  bool operator==(const Token& other) const {
    return value_ == other.value_ && offset_ == other.offset_;
  }

  /// Ostream output operator.
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
