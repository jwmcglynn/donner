#include "src/svg/parser/transform_parser.h"

#include "src/base/parser/details/parser_base.h"

namespace donner::svg {

class TransformParserImpl : public ParserBase {
public:
  TransformParserImpl(std::string_view str) : ParserBase(str) {}

  ParseResult<Transformd> parse() {
    bool hasFunction = false;
    Transformd transform;

    skipWhitespace();

    while (!remaining_.empty()) {
      if (hasFunction && remaining_[0] == ',') {
        // Skip optional comma.
        remaining_.remove_prefix(1);
        skipWhitespace();
      }

      const int functionStart = currentOffset();

      ParseResult<std::string_view> maybeFunc = readFunction();
      if (maybeFunc.hasError()) {
        return std::move(maybeFunc.error());
      }

      // Skip whitespace after function open paren, '('.
      skipWhitespace();

      const std::string_view func = maybeFunc.result();
      if (func == "matrix") {
        Transformd t(uninitialized);
        if (auto error = readNumbers(t.data)) {
          return std::move(error.value());
        }

        transform *= t;

      } else if (func == "translate") {
        // Accept either 1 or 2 numbers.
        auto maybeTx = readNumber();
        if (maybeTx.hasError()) {
          return std::move(maybeTx.error());
        }

        skipWhitespace();
        if (remaining_.starts_with(')')) {
          // Only one parameter provided, so Ty is implicitly 0.0.
          transform *= Transformd::Translate(Vector2d(maybeTx.result(), 0.0));
        } else {
          skipCommaWhitespace();

          auto maybeTy = readNumber();
          if (maybeTy.hasError()) {
            return std::move(maybeTy.error());
          }

          transform *= Transformd::Translate(Vector2d(maybeTx.result(), maybeTy.result()));
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
          transform *= Transformd::Scale(Vector2d(maybeSx.result(), maybeSx.result()));
        } else {
          skipCommaWhitespace();

          auto maybeSy = readNumber();
          if (maybeSy.hasError()) {
            return std::move(maybeSy.error());
          }

          transform *= Transformd::Scale(Vector2d(maybeSx.result(), maybeSy.result()));
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
          transform *= Transformd::Rotation(maybeRotationDegrees.result() *
                                            MathConstants<double>::kDegToRad);
        } else {
          skipCommaWhitespace();

          double numbers[2];
          if (auto error = readNumbers(numbers)) {
            return std::move(error.value());
          }

          const Vector2d offset(numbers[0], numbers[1]);
          transform *= Transformd::Translate(-offset) *
                       Transformd::Rotation(maybeRotationDegrees.result() *
                                            MathConstants<double>::kDegToRad) *
                       Transformd::Translate(offset);
        }

      } else if (func == "skewX") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform *= Transformd::SkewX(maybeNumber.result() * MathConstants<double>::kDegToRad);

      } else if (func == "skewY") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform *= Transformd::SkewY(maybeNumber.result() * MathConstants<double>::kDegToRad);

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
        hasFunction = true;
      } else {
        ParseError err;
        err.reason = "Expected ')'";
        err.offset = currentOffset();
        return err;
      }
    }

    return transform;
  }

private:
  ParseResult<std::string_view> readFunction() {
    for (size_t i = 0; i < remaining_.size(); ++i) {
      if (remaining_[i] == '(') {
        std::string_view func = remaining_.substr(0, i);
        remaining_.remove_prefix(i + 1);
        return func;
      } else if (isWhitespace(remaining_[i])) {
        std::string_view func = take(i);

        // Skip whitespace between function name and '(', such as "matrix ("
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
};

ParseResult<Transformd> TransformParser::Parse(std::string_view str) {
  TransformParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner::svg
