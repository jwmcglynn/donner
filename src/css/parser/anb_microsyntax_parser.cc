#include "src/css/parser/anb_microsyntax_parser.h"

#include "src/base/string_utils.h"

namespace donner::css {

namespace {

/**
 * Token for the An+B CSS microsyntax parser.
 */
struct AnbToken {
  /**
   * The type of the An+B token, from
   * https://www.w3.org/TR/css-syntax-3/#the-anb-type.
   */
  enum class Type {
    /// The ident 'odd'
    Odd,

    /// The ident 'even'
    Even,

    /// The token is a '+' <delim-token>
    Plus,

    /// The token is a '-' <delim-token>
    Minus,

    /// The token is a 'n' <ident-token>
    N,

    /// The token is a 'n-' <ident-token>
    NMinus,

    /// The token is a '-n' <ident-token>
    MinusN,

    /// The token is a '-n-' <ident-token>
    MinusNMinus,

    /// <n-dimension> is a <dimension-token> with its type flag set to
    /// "integer", and
    /// a unit that is an ASCII case-insensitive match for "n"
    NDimension,

    /// <ndash-dimension> is a <dimension-token> with its type flag set to
    /// "integer", and a unit that is an ASCII case-insensitive match for "n-"
    NDashDimension,

    /// <ndashdigit-dimension> is a <dimension-token> with its type flag set
    /// to "integer", and a unit that is an ASCII case-insensitive match for
    /// "n-*", where "*" is a series of one or more digits
    NDashDigitDimension,

    /// <ndashdigit-ident> is an <ident-token> whose value is an ASCII
    /// case-insensitive match for "n-*", where "*" is a series of one or more
    /// digits
    NDashDigitIdent,

    /// <dashndashdigit-ident> is an <ident-token> whose value is an ASCII
    /// case-insensitive match for "-n-*", where "*" is a series of one or
    /// more digits
    DashNDashDigitIdent,

    /// <signed-integer> is a <number-token> with its type flag set to
    /// "integer", and whose representation starts with "+" or "-"
    SignedInteger,

    /// <signless-integer> is a <number-token> with its type flag set to
    /// "integer", and whose representation starts with a digit
    SignlessInteger
  };

  Type type;                      //!< The type of the An+B token
  std::optional<int> value;       //!< The token value
  std::optional<int> digitValue;  //!< The digit value, if this is a token that has Digit in it.

  /**
   * Token for the An+B CSS microsyntax parser.
   *
   * @param type The type of the An+B token
   * @param value The token value
   */
  AnbToken(Type type, std::optional<int> value = std::nullopt,
           std::optional<int> digitValue = std::nullopt)
      : type(type), value(value), digitValue(digitValue) {
    assertInvariants();
  }

  /**
   * Validates that the type held in \ref type matches the type of the token
   * value in \ref value.
   */
  void assertInvariants() {
    if (value.has_value()) {
      assert(type == Type::NDimension || type == Type::NDashDimension ||
             type == Type::NDashDigitDimension || type == Type::SignedInteger ||
             type == Type::SignlessInteger);
    } else {
      assert(type == Type::Even || type == Type::Odd || type == Type::Plus || type == Type::Minus ||
             type == Type::N || type == Type::NMinus || type == Type::MinusN ||
             type == Type::MinusNMinus || type == Type::NDashDigitIdent ||
             type == Type::DashNDashDigitIdent);
    }

    if (digitValue.has_value()) {
      assert(type == Type::NDashDigitDimension || type == Type::NDashDigitIdent ||
             type == Type::DashNDashDigitIdent);
    }
  }
};

class AnbMicrosyntaxParserImpl {
public:
  AnbMicrosyntaxParserImpl(std::span<const css::ComponentValue> components)
      : components_(components) {}

  ParseResult<AnbValue> parse() {
    skipWhitespace();

    if (components_.empty()) {
      ParseError err;
      err.reason = "An+B microsyntax expected, found empty list";
      return err;
    }

    auto firstTokenResult = consumeToken();
    if (firstTokenResult.hasError()) {
      return std::move(firstTokenResult.error());
    }

    const bool hasLeadingPlus = firstTokenResult.result().type == AnbToken::Type::Plus;
    if (hasLeadingPlus) {
      // From the spec: When a plus sign (+) precedes an ident starting with "n", there must be no
      // whitespace between the two tokens, or else the tokens do not match the above grammar.
      // Whitespace is valid (and ignored) between any other two tokens.
      firstTokenResult = consumeToken();
    }

    if (firstTokenResult.hasError()) {
      return std::move(firstTokenResult.error());
    }

    const AnbToken firstToken = firstTokenResult.result();
    skipWhitespace();
    const std::optional<AnbToken::Type> maybeSecondTokenType = peekNextTokenType();

    if (!hasLeadingPlus) {
      // odd | even |
      if (firstToken.type == AnbToken::Type::Odd) {
        return AnbValue(2, 1);
      } else if (firstToken.type == AnbToken::Type::Even) {
        return AnbValue(2, 0);
      }
      // <integer> |
      else if (firstToken.type == AnbToken::Type::SignedInteger ||
               firstToken.type == AnbToken::Type::SignlessInteger) {
        return AnbValue(0, firstToken.value.value());
      }

      // <n-dimension> |
      else if (firstToken.type == AnbToken::Type::NDimension) {
        if (!maybeSecondTokenType.has_value()) {
          return AnbValue(firstToken.value.value(), 0);
        }
        // <n-dimension> <signed-integer> |
        else if (maybeSecondTokenType == AnbToken::Type::SignedInteger) {
          return AnbValue(firstToken.value.value(), consumeToken().result().value.value());
        }
        // <n-dimension> ['+' | '-'] <signless-integer>
        else if (maybeSecondTokenType == AnbToken::Type::Plus ||
                 maybeSecondTokenType == AnbToken::Type::Minus) {
          const AnbToken secondToken = consumeToken().result();
          skipWhitespace();

          if (peekNextTokenType() == AnbToken::Type::SignlessInteger) {
            const AnbToken thirdToken = consumeToken().result();

            // A is the dimension's value. B is the integer's value. If a '-'
            // was provided between the two, B is instead the negation of the
            // integer's value.
            return AnbValue(firstToken.value.value(), secondToken.type == AnbToken::Type::Minus
                                                          ? -thirdToken.value.value()
                                                          : thirdToken.value.value());
          }
        }
      }
      // -n |
      else if (firstToken.type == AnbToken::Type::MinusN) {
        if (!maybeSecondTokenType.has_value()) {
          return AnbValue(-1, 0);
        } else {
          // -n <signed-integer> |
          if (maybeSecondTokenType == AnbToken::Type::SignedInteger) {
            const AnbToken secondToken = consumeToken().result();
            return AnbValue(-1, secondToken.value.value());
          }
          // -n ['+' | '-'] <signless-integer>
          else if (maybeSecondTokenType == AnbToken::Type::Plus ||
                   maybeSecondTokenType == AnbToken::Type::Minus) {
            const AnbToken secondToken = consumeToken().result();
            skipWhitespace();

            if (peekNextTokenType() == AnbToken::Type::SignlessInteger) {
              const AnbToken thirdToken = consumeToken().result();

              // A is -1. B is the integer's value. If a '-' was provided between
              // the two, B is instead the negation of the integer's value.
              return AnbValue(-1, secondToken.type == AnbToken::Type::Minus
                                      ? -thirdToken.value.value()
                                      : thirdToken.value.value());
            } else {
              ParseError err;
              err.reason = "An+B microsyntax unexpected end of list";
              err.offset = ParseError::kEndOfString;
              return err;
            }
          }
        }
      }

      // <ndashdigit-dimension> |
      else if (firstToken.type == AnbToken::Type::NDashDigitDimension) {
        return AnbValue(firstToken.value.value(), -firstToken.digitValue.value());
      }
      // <dashndashdigit-ident> |
      else if (firstToken.type == AnbToken::Type::DashNDashDigitIdent) {
        return AnbValue(-1, -firstToken.digitValue.value());
      }

      // <ndash-dimension> <signless-integer> |
      else if (firstToken.type == AnbToken::Type::NDashDimension &&
               maybeSecondTokenType == AnbToken::Type::SignlessInteger) {
        return AnbValue(firstToken.value.value(), -consumeToken().result().value.value());
      }
      // -n- <signless-integer> |
      else if (firstToken.type == AnbToken::Type::MinusNMinus &&
               maybeSecondTokenType == AnbToken::Type::SignlessInteger) {
        return AnbValue(-1, -consumeToken().result().value.value());
      }
    }

    // Either does or does not have leading plus.
    // If there is a leading plus, firstToken will be the first non-'+' token.

    // '+'? <ndashdigit-ident> |
    if (firstToken.type == AnbToken::Type::NDashDigitIdent) {
      return AnbValue(1, -firstToken.digitValue.value());
    } else if (firstToken.type == AnbToken::Type::N) {
      // '+'? n
      if (!maybeSecondTokenType.has_value()) {
        return AnbValue(1, 0);
      }

      // '+'? n <signed-integer> |
      if (maybeSecondTokenType == AnbToken::Type::SignedInteger) {
        const AnbToken secondToken = consumeToken().result();
        return AnbValue(1, secondToken.value.value());
      }
      // '+'? n ['+' | '-'] <signless-integer> |
      else if (maybeSecondTokenType == AnbToken::Type::Plus ||
               maybeSecondTokenType == AnbToken::Type::Minus) {
        const AnbToken secondToken = consumeToken().result();
        skipWhitespace();

        if (const auto maybeThirdTokenType = peekNextTokenType();
            maybeThirdTokenType == AnbToken::Type::SignlessInteger) {
          const AnbToken thirdToken = consumeToken().result();

          // A is 1, respectively. B is the integer's value. If a '-' was provided
          // between the two, B is instead the negation of the integer's value.
          return AnbValue(1, secondToken.type == AnbToken::Type::Minus ? -thirdToken.value.value()
                                                                       : thirdToken.value.value());
        }
      }
    }
    // '+'? n- <signless-integer> |
    else if (firstToken.type == AnbToken::Type::NMinus &&
             maybeSecondTokenType == AnbToken::Type::SignlessInteger) {
      return AnbValue(1, -consumeToken().result().value.value());
    }

    if (components_.empty()) {
      ParseError err;
      err.reason = "An+B microsyntax unexpected end of list";
      err.offset = ParseError::kEndOfString;
      return err;
    } else {
      ParseError err;
      err.reason = "Unexpected token when parsing An+B microsyntax";
      err.offset = components_.front().sourceOffset();
      return err;
    }
  }

  std::span<const css::ComponentValue> remainingComponents() const { return components_; }

private:
  ParseResult<AnbToken> consumeToken() {
    auto result = parseNextToken();
    if (result.hasError()) {
      return result;
    } else {
      components_ = components_.subspan(1);
      return result;
    }
  }

  void skipWhitespace() {
    const int numSkipped = skipWhitespace(0);
    components_ = components_.subspan(numSkipped);
  }

  std::optional<AnbToken::Type> peekNextTokenType() {
    if (!components_.empty()) {
      if (const auto anbTokenResult = parseNextToken(); anbTokenResult.hasResult()) {
        return anbTokenResult.result().type;
      }
    }

    return std::nullopt;
  }

  ParseResult<AnbToken> parseNextToken() {
    if (components_.empty()) {
      ParseError err;
      err.reason = "An+B microsyntax unexpected end of list";
      return err;
    }

    const ComponentValue& component = components_[0];

    if (!component.is<Token>()) {
      ParseError err;
      err.reason = "Expected CSS token when parsing An+B microsyntax";
      err.offset = component.sourceOffset();
      return err;
    }

    auto token = component.get<Token>();
    if (token.is<Token::Delim>()) {
      const auto& delim = token.get<Token::Delim>();

      // '+' or '-'
      if (delim.value == '+') {
        return AnbToken(AnbToken::Type::Plus);
      } else if (delim.value == '-') {
        return AnbToken(AnbToken::Type::Minus);
      }
    } else if (token.is<Token::Dimension>()) {
      auto dimension = std::move(token.get<Token::Dimension>());

      if (dimension.type == NumberType::Integer) {
        // <n-dimension> is a <dimension-token> with its type flag set to
        // "integer", and a unit that is an ASCII case-insensitive match for "n"
        if (dimension.suffixString.equalsLowercase("n")) {
          return AnbToken(AnbToken::Type::NDimension, dimension.value);
        }
        // <ndashdigit-dimension> is a <dimension-token> with its type flag set
        // to "integer", and a unit that is an ASCII case-insensitive match for
        // "n-*", where "*" is a series of one or more digits
        else if (StringUtils::StartsWith<StringComparison::IgnoreCase>(dimension.suffixString,
                                                                       std::string_view("n-")) &&
                 isAllDigits(dimension.suffixString.substr(2))) {
          return AnbToken(AnbToken::Type::NDashDigitDimension, dimension.value,
                          parseDigits(dimension.suffixString.substr(2)));
        }
        // <ndash-dimension> is a <dimension-token> with its type flag set to
        // "integer", and a unit that is an ASCII case-insensitive match for
        // "n-"
        else if (StringUtils::EqualsLowercase(dimension.suffixString, std::string_view("n-"))) {
          return AnbToken(AnbToken::Type::NDashDimension, dimension.value);
        }
      }
    } else if (token.is<Token::Ident>()) {
      auto ident = std::move(token.get<Token::Ident>());

      // Parse strings such as 'odd' and 'even'
      if (ident.value.equalsLowercase("odd")) {
        return AnbToken(AnbToken::Type::Odd, std::nullopt);
      } else if (ident.value.equalsLowercase("even")) {
        return AnbToken(AnbToken::Type::Even, std::nullopt);
      }
      // Parse constant ident strings, such as 'n', '-n', and '-n-'
      else if (ident.value.equalsLowercase("n")) {
        return AnbToken(AnbToken::Type::N, std::nullopt);
      } else if (ident.value.equalsLowercase("-n")) {
        return AnbToken(AnbToken::Type::MinusN, std::nullopt);
      } else if (ident.value.equalsLowercase("-n-")) {
        return AnbToken(AnbToken::Type::MinusNMinus, std::nullopt);
      } else if (ident.value.equalsLowercase("n-")) {
        return AnbToken(AnbToken::Type::NMinus, std::nullopt);
      }
      // <ndashdigit-ident> is an <ident-token> whose value is an ASCII
      // case-insensitive match for "n-*", where "*" is a series of one or more
      // digits
      else if (StringUtils::StartsWith(ident.value, std::string_view("n-")) &&
               isAllDigits(ident.value.substr(2))) {
        return AnbToken(AnbToken::Type::NDashDigitIdent, std::nullopt,
                        parseDigits(ident.value.substr(2)));
      }
      // <dashndashdigit-ident> is an <ident-token> whose value is an ASCII
      // case-insensitive match for "-n-*", where "*" is a series of one or more
      // digits
      else if (StringUtils::StartsWith(ident.value, std::string_view("-n-")) &&
               isAllDigits(ident.value.substr(3))) {
        return AnbToken(AnbToken::Type::DashNDashDigitIdent, std::nullopt,
                        parseDigits(ident.value.substr(3)));
      }

    } else if (token.is<Token::Number>()) {
      auto number = std::move(token.get<Token::Number>());
      assert(!number.valueString.empty() && "Parsed number should not have empty valueString");

      const std::string_view numberStr(number.valueString);

      if (number.type == NumberType::Integer) {
        // <signed-integer> is a <number-token> with its type flag set to
        // "integer", and whose representation starts with "+" or "-"
        if (number.type == NumberType::Integer &&
            (numberStr.starts_with('+') || numberStr.starts_with('-'))) {
          return AnbToken(AnbToken::Type::SignedInteger, number.value);
        }
        // <signless-integer> is a <number-token> with its type flag set to
        // "integer", and whose representation starts with a digit
        else {
          assert(std::isdigit(numberStr[0]));
          return AnbToken(AnbToken::Type::SignlessInteger, number.value);
        }
      }
    }

    ParseError err;
    err.reason = "Unexpected token when parsing An+B microsyntax";
    err.offset = token.offset();
    return err;
  }

  int skipWhitespace(int nextToken) {
    while (nextToken < components_.size() && components_[nextToken].isToken<Token::Whitespace>()) {
      nextToken++;
    }

    return nextToken;
  }

private:
  bool isAllDigits(std::string_view str) {
    if (str.empty()) {
      return false;
    }

    for (int i = 0; i < str.size(); i++) {
      if (!std::isdigit(str[i])) {
        return false;
      }
    }

    return true;
  }

  int parseDigits(std::string_view str) {
    assert(!str.empty() && std::isdigit(str[0]) && "First character must be a digit");

    int value = 0;
    for (int i = 0; i < str.size(); i++) {
      assert(std::isdigit(str[i]));
      value = value * 10 + (str[i] - '0');
    }

    return value;
  }

  std::span<const css::ComponentValue> components_;
};

}  // namespace

ParseResult<AnbMicrosyntaxParser::Result> AnbMicrosyntaxParser::Parse(
    std::span<const css::ComponentValue> components) {
  AnbMicrosyntaxParserImpl parser(components);
  return parser.parse().map<AnbMicrosyntaxParser::Result>([&parser](const AnbValue& value) {
    return AnbMicrosyntaxParser::Result{value, parser.remainingComponents()};
  });
}

}  // namespace donner::css
