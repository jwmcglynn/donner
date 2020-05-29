#include "src/svg/parser/transform_parser.h"

#include <span>

#include "src/svg/parser/number_parser.h"

namespace donner {

class TransformParserImpl {
public:
  TransformParserImpl(std::string_view str) : str_(str), remaining_(str) {}

  ParseResult<Transformd> parse() {
    skipWhitespace();

    while (!remaining_.empty()) {
      const int functionStart = currentOffset();

      ParseResult<std::string_view> maybeFunc = readFunction();
      if (maybeFunc.hasError()) {
        return std::move(maybeFunc.error());
      }

      // Skip whitespace between function name and '(', such as "matrix ("
      skipWhitespace();

      const std::string_view func = maybeFunc.result();
      if (func == "matrix") {
        Transformd t(uninitialized);
        if (auto error = readNumbers(t.data)) {
          return std::move(error.value());
        }

        transform_ *= t;

      } else if (func == "translate") {
        // Accept either 1 or 2 numbers.
        auto maybeTx = readNumber();
        if (maybeTx.hasError()) {
          return std::move(maybeTx.error());
        }

        skipWhitespace();
        if (remaining_.starts_with(')')) {
          // Only one parameter provided, so Ty is implicitly 0.0.
          transform_ *= Transformd::Translate(Vector2d(maybeTx.result(), 0.0));
        } else {
          skipCommaWhitespace();

          auto maybeTy = readNumber();
          if (maybeTy.hasError()) {
            return std::move(maybeTy.error());
          }

          transform_ *= Transformd::Translate(Vector2d(maybeTx.result(), maybeTy.result()));
        }

      } else if (func == "scale") {
        // Accept either 1 or 2 numbers.
        auto maybeSx = readNumber();
        if (maybeSx.hasError()) {
          return std::move(maybeSx.error());
        }

        skipWhitespace();
        if (remaining_.starts_with(')')) {
          // Only one parameter provided, use Sx for both x and y.
          transform_ *= Transformd::Translate(Vector2d(maybeSx.result(), maybeSx.result()));
        } else {
          skipCommaWhitespace();

          auto maybeSy = readNumber();
          if (maybeSy.hasError()) {
            return std::move(maybeSy.error());
          }

          transform_ *= Transformd::Translate(Vector2d(maybeSx.result(), maybeSy.result()));
        }

      } else if (func == "rotate") {
        // Accept either 1 or 3 numbers, if 3 are provided the last two are cx and cy.
        auto maybeRotationDegrees = readNumber();
        if (maybeRotationDegrees.hasError()) {
          return std::move(maybeRotationDegrees.error());
        }

        skipWhitespace();
        if (remaining_.starts_with(')')) {
          // Only one parameter provided, rotation around origin.
          transform_ *= Transformd::Rotation(maybeRotationDegrees.result() *
                                             MathConstants<double>::kDegToRad);
        } else {
          skipCommaWhitespace();

          double numbers[2];
          if (auto error = readNumbers(numbers)) {
            return std::move(error.value());
          }

          const Vector2d offset(numbers[0], numbers[1]);
          transform_ *= Transformd::Translate(offset) *
                        Transformd::Rotation(maybeRotationDegrees.result() *
                                             MathConstants<double>::kDegToRad) *
                        Transformd::Translate(-offset);
        }

      } else if (func == "skewX") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform_ *= Transformd::ShearX(maybeNumber.result());

      } else if (func == "skewY") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform_ *= Transformd::ShearY(maybeNumber.result());

      } else {
        ParseError err;
        err.reason = std::string("Unexpected function '").append(func) + "'";
        err.offset = functionStart;
        return err;
      }

      // Whitespace before closing ')'
      skipWhitespace();

      if (remaining_.starts_with(')')) {
        remaining_.remove_prefix(1);
        skipWhitespace();
      } else {
        ParseError err;
        err.reason = "Expected ')'";
        err.offset = currentOffset();
        return err;
      }
    }

    return transform_;
  }

private:
  void skipWhitespace() {
    while (!remaining_.empty() && isWhitespace(remaining_[0])) {
      remaining_.remove_prefix(1);
    }
  }

  // Returns true if a comma was encountered.
  void skipCommaWhitespace() {
    bool foundComma = false;
    while (!remaining_.empty()) {
      const char ch = remaining_[0];
      if (!foundComma && ch == ',') {
        foundComma = true;
        remaining_.remove_prefix(1);
      } else if (isWhitespace(ch)) {
        remaining_.remove_prefix(1);
      } else {
        break;
      }
    }
  }

  bool isWhitespace(char ch) const {
    // https://www.w3.org/TR/css-transforms-1/#svg-wsp
    // Either a U+000A LINE FEED, U+000D CARRIAGE RETURN, U+0009 CHARACTER TABULATION, or U+0020
    // SPACE.
    return ch == '\t' || ch == ' ' || ch == '\n' || ch == '\r';
  }

  int currentOffset() { return remaining_.data() - str_.data(); }

  ParseResult<std::string_view> readFunction() {
    for (size_t i = 0; i < remaining_.size(); ++i) {
      if (remaining_[i] == '(') {
        std::string_view func = remaining_.substr(0, i);
        remaining_.remove_prefix(i + 1);
        return func;
      } else if (isWhitespace(remaining_[i])) {
        std::string_view func = remaining_.substr(0, i);
        remaining_.remove_prefix(i);
        skipWhitespace();

        if (remaining_.starts_with('(')) {
          remaining_.remove_prefix(1);
          return func;
        } else {
          ParseError err;
          err.reason = "Expected '(' after function name";
          err.offset = currentOffset() + i;
          return err;
        }
      }
    }

    ParseError err;
    err.reason = "Unexpected end of string instead of transform function";
    err.offset = currentOffset();
    return err;
  }

  ParseResult<double> readNumber() {
    skipWhitespace();

    ParseResult<NumberParser::Result> maybeResult = NumberParser::parse(remaining_);
    if (maybeResult.hasError()) {
      ParseError err = std::move(maybeResult.error());
      err.offset += currentOffset();
      return err;
    }

    const NumberParser::Result& result = maybeResult.result();
    remaining_.remove_prefix(result.consumed_chars);
    return result.number;
  }

  std::optional<ParseError> readNumbers(std::span<double> resultStorage) {
    for (size_t i = 0; i < resultStorage.size(); ++i) {
      if (i != 0) {
        skipCommaWhitespace();
      }

      auto maybeNumber = readNumber();
      if (maybeNumber.hasError()) {
        return std::move(maybeNumber.error());
      }

      resultStorage[i] = maybeNumber.result();
    }

    return std::nullopt;
  }

  Transformd transform_;

  const std::string_view str_;
  std::string_view remaining_;
};

ParseResult<Transformd> TransformParser::parse(std::string_view str) {
  TransformParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner
