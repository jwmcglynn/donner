#include "donner/svg/parser/TransformParser.h"

#include "donner/base/parser/details/ParserBase.h"

namespace donner::svg::parser {

namespace {

class TransformParserImpl : public donner::parser::ParserBase {
public:
  TransformParserImpl(std::string_view str) : ParserBase(str) {}

  ParseResult<Transform2d> parse() {
    bool allowComma = false;
    Transform2d transform;

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
        Transform2d t(Transform2d::uninitialized);
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
          transform = Transform2d::Translate(Vector2d(maybeTx.result(), 0.0)) * transform;
        } else {
          skipCommaWhitespace();

          auto maybeTy = readNumber();
          if (maybeTy.hasError()) {
            return std::move(maybeTy.error());
          }

          transform =
              Transform2d::Translate(Vector2d(maybeTx.result(), maybeTy.result())) * transform;
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
          transform = Transform2d::Scale(Vector2d(maybeSx.result(), maybeSx.result())) * transform;
        } else {
          skipCommaWhitespace();

          auto maybeSy = readNumber();
          if (maybeSy.hasError()) {
            return std::move(maybeSy.error());
          }

          transform = Transform2d::Scale(Vector2d(maybeSx.result(), maybeSy.result())) * transform;
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
          transform =
              Transform2d::Rotate(maybeRotationDegrees.result() * MathConstants<double>::kDegToRad) *
              transform;
        } else {
          skipCommaWhitespace();

          double numbers[2];
          if (auto error = readNumbers(numbers)) {
            return std::move(error.value());
          }

          const Vector2d offset(numbers[0], numbers[1]);
          transform =
              Transform2d::Translate(-offset) *
              Transform2d::Rotate(maybeRotationDegrees.result() * MathConstants<double>::kDegToRad) *
              Transform2d::Translate(offset) * transform;
        }

      } else if (func == "skewX") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform =
            Transform2d::SkewX(maybeNumber.result() * MathConstants<double>::kDegToRad) * transform;

      } else if (func == "skewY") {
        auto maybeNumber = readNumber();
        if (maybeNumber.hasError()) {
          return std::move(maybeNumber.error());
        }

        transform =
            Transform2d::SkewY(maybeNumber.result() * MathConstants<double>::kDegToRad) * transform;

      } else {
        ParseDiagnostic err;
        err.reason = std::string("Unexpected function '").append(func) + "'";
        err.range = rangeFrom(functionStart.offset.value());
        return err;
      }

      // Whitespace before closing ')'
      skipWhitespace();

      if (remaining_.starts_with(')')) {
        remaining_.remove_prefix(1);
        skipWhitespace();
        allowComma = true;
      } else {
        ParseDiagnostic err;
        err.reason = "Expected ')'";
        err.range = currentRange(0, 1);
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
          ParseDiagnostic err;
          err.reason = "Expected '(' after function name";
          err.range = currentRange(0, 1);
          return err;
        }
      }
    }

    ParseDiagnostic err;
    err.reason = "Unexpected end of string instead of transform function";
    err.range = {FileOffset::EndOfString(), FileOffset::EndOfString()};
    return err;
  }
};

}  // namespace

ParseResult<Transform2d> TransformParser::Parse(std::string_view str) {
  TransformParserImpl parser(str);
  return parser.parse();
}

}  // namespace donner::svg::parser
