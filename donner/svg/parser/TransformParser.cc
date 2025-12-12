#include "donner/svg/parser/TransformParser.h"

#include "donner/base/parser/details/ParserBase.h"

namespace donner::svg::parser {

namespace {

class TransformParserImpl : public donner::parser::ParserBase {
public:
  TransformParserImpl(std::string_view str, TransformParser::Options options)
      : ParserBase(str), options_(options) {}

  ParseResult<Transformd> parse() {
    bool allowComma = false;
    Transformd transform;

    skipWhitespace();

    while (!remaining_.empty()) {
      if (allowComma) {
        skipCommaWhitespace();
      }

      const FileOffset functionStart = currentOffset();

      ParseResult<std::string_view> maybeFunc = readFunction();
      if (maybeFunc.hasError()) {
        return std::move(maybeFunc.error());
      }

      // Skip whitespace after function open paren, '('.
      skipWhitespace();

      const std::string_view func = maybeFunc.result();
      if (func == "matrix") {
        Transformd t(Transformd::uninitialized);
        if (auto error = readNumbers(t.data)) {
          return std::move(error.value());
        }

        transform = t * transform;

      } else if (func == "translate") {
        // Accept either 1 or 2 numbers.
        auto maybeTx = readNumber();
        if (maybeTx.hasError()) {
          return std::move(maybeTx.error());
        }

        skipWhitespace();
        if (remaining_.starts_with(')')) {
          // Only one parameter provided, so Ty is implicitly 0.0.
          transform = Transformd::Translate(Vector2d(maybeTx.result(), 0.0)) * transform;
        } else {
          skipCommaWhitespace();

          auto maybeTy = readNumber();
          if (maybeTy.hasError()) {
            return std::move(maybeTy.error());
          }

          transform =
              Transformd::Translate(Vector2d(maybeTx.result(), maybeTy.result())) * transform;
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
          transform = Transformd::Scale(Vector2d(maybeSx.result(), maybeSx.result())) * transform;
        } else {
          skipCommaWhitespace();

          auto maybeSy = readNumber();
          if (maybeSy.hasError()) {
            return std::move(maybeSy.error());
          }

          transform = Transformd::Scale(Vector2d(maybeSx.result(), maybeSy.result())) * transform;
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
          transform = Transformd::Rotate(angleToRadians(maybeRotationDegrees.result())) *
                      transform;
        } else {
          skipCommaWhitespace();

          double numbers[2];
          if (auto error = readNumbers(numbers)) {
            return std::move(error.value());
          }

          const Vector2d offset(numbers[0], numbers[1]);
          transform =
              Transformd::Translate(-offset) *
              Transformd::Rotate(angleToRadians(maybeRotationDegrees.result())) *
              Transformd::Translate(offset) * transform;
        }

      } else if (func == "skewX") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform = Transformd::SkewX(angleToRadians(maybeNumber.result())) * transform;

      } else if (func == "skewY") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform = Transformd::SkewY(angleToRadians(maybeNumber.result())) * transform;

      } else {
        ParseError err;
        err.reason = std::string("Unexpected function '").append(func) + "'";
        err.location = functionStart;
        return err;
      }

      // Whitespace before closing ')'
      skipWhitespace();

      if (remaining_.starts_with(')')) {
        remaining_.remove_prefix(1);
        skipWhitespace();
        allowComma = true;
      } else {
        ParseError err;
        err.reason = "Expected ')'";
        err.location = currentOffset();
        return err;
      }
    }

    return transform;
  }

private:
  double angleToRadians(double value) const {
    if (options_.angleUnit == TransformParser::AngleUnit::Radians) {
      return value;
    }

    return value * MathConstants<double>::kDegToRad;
  }

  TransformParser::Options options_;

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
          err.location = currentOffset();
          err.location.offset.value() += i;
          return err;
        }
      }
    }

    ParseError err;
    err.reason = "Unexpected end of string instead of transform function";
    err.location = currentOffset();
    return err;
  }
};

}  // namespace

ParseResult<Transformd> TransformParser::Parse(std::string_view str, const Options& options) {
  TransformParserImpl parser(str, options);
  return parser.parse();
}

}  // namespace donner::svg::parser
