#include "donner/svg/parser/CssTransformParser.h"

#include "donner/base/FileOffset.h"
#include "donner/svg/parser/AngleParser.h"
#include "donner/svg/parser/LengthPercentageParser.h"

namespace donner::svg::parser {

namespace {

[[gnu::noinline]] ParseDiagnostic TransformErrorAt(std::string_view reason, FileOffset offset) {
  return ParseDiagnostic::Error(RcString(reason), offset);
}

/**
 * Create a Transform from the CSS "skew" function parameters, alpha and theta. Note that skew is
 * deprecated in favor of skewX/skewY.
 *
 * @param thetaAlpha Angle in radians.
 * @param thetaBeta Angle in radians.
 */
static Transform2d Skew(double thetaAlpha, double thetaBeta) {
  const double shearX = std::tan(thetaAlpha);
  const double shearY = std::tan(thetaBeta);

  Transform2d result;
  result.data[2] = shearX;
  result.data[1] = shearY;
  return result;
}

class ComponentValueParser {
public:
  explicit ComponentValueParser(std::span<const css::ComponentValue> components)
      : components_(components) {
    skipWhitespace();
  }

  bool isEOF() const { return components_.empty(); }

  template <typename Type>
  const Type* tryConsume() {
    if (isEOF() || !components_.front().is<Type>()) {
      return nullptr;
    }

    const Type* result = &components_.front().get<Type>();
    components_ = components_.subspan(1);
    return result;
  }

  template <typename TokenType>
  const TokenType* tryConsumeToken() {
    if (isEOF() || !components_.front().isToken<TokenType>()) {
      return nullptr;
    }

    const TokenType* result = &components_.front().get<css::Token>().get<TokenType>();
    components_ = components_.subspan(1);
    return result;
  }

  void skipWhitespace() {
    if (!components_.empty() && components_.front().isToken<css::Token::Whitespace>()) {
      components_ = components_.subspan(1);
    }
  }

  ParseResult<double> readNumber() {
    if (!components_.empty()) {
      if (const auto* number = components_.front().tryGetToken<css::Token::Number>()) {
        components_ = components_.subspan(1);
        return number->value;
      } else {
        return TransformErrorAt("Expected a number", components_.front().sourceOffset());
      }
    }

    return TransformErrorAt("Not enough parameters", FileOffset::EndOfString());
  }

  std::optional<ParseDiagnostic> readNumbers(std::span<double> resultStorage) {
    for (size_t i = 0; i < resultStorage.size(); ++i) {
      if (i != 0) {
        skipWhitespace();
        if (!tryConsumeToken<css::Token::Comma>()) {
          return TransformErrorAt(isEOF() ? "Not enough parameters" : "Expected a comma",
                                  sourceOffset());
        }
        skipWhitespace();
      }

      auto maybeNumber = readNumber();
      if (maybeNumber.hasError()) {
        return std::move(maybeNumber.error());
      }

      resultStorage[i] = maybeNumber.result();
    }

    return std::nullopt;
  }

  ParseResult<Lengthd> readLengthPercentage() {
    if (!components_.empty()) {
      auto result = ParseLengthPercentage(components_.front(), false);
      if (result.hasError()) {
        return std::move(result.error());
      }

      components_ = components_.subspan(1);
      return result.result();
    }

    return TransformErrorAt("Not enough parameters", FileOffset::EndOfString());
  }

  ParseResult<double> readAngle(AngleParseOptions options) {
    if (!components_.empty()) {
      auto result = ParseAngle(components_.front(), options);
      if (result.hasError()) {
        return std::move(result.error());
      }

      components_ = components_.subspan(1);
      return result.result();
    }

    return TransformErrorAt("Not enough parameters", FileOffset::EndOfString());
  }

  FileOffset sourceOffset() const {
    if (isEOF()) {
      return FileOffset::EndOfString();
    } else {
      return components_.front().sourceOffset();
    }
  }

private:
  std::span<const css::ComponentValue> components_;
};

class CssTransformParserImpl {
public:
  CssTransformParserImpl(std::span<const css::ComponentValue> components) : parser_(components) {}

  ParseResult<CssTransform> parse() {
    while (!parser_.isEOF()) {
      const FileOffset functionStart = parser_.sourceOffset();
      if (auto error = parseFunction()) {
        return std::move(error.value());
      }

      // Token values can be infinite (for example `1e400`), and composing functions can overflow
      // even when each value is finite, so check the transform after every function.
      if (!transform_.isFinite()) {
        ParseDiagnostic err;
        err.reason = "Transform out of range";
        err.range = {functionStart, parser_.sourceOffset()};
        return err;
      }

      parser_.skipWhitespace();
    }

    return std::move(transform_);
  }

  std::optional<ParseDiagnostic> parseFunction() {
    if (const auto* function = parser_.tryConsume<css::Function>()) {
      const RcString& name = function->name;
      ComponentValueParser subparser(function->values);

      if (name.equalsLowercase("matrix")) {
        return parseMatrix(subparser);
      } else if (name.equalsLowercase("translate")) {
        return parseTranslate(subparser);
      } else if (name.equalsLowercase("translatex")) {
        auto result = parseSingleLengthPercentage(subparser);
        if (result.hasError()) {
          return std::move(result.error());
        }

        transform_.appendTranslate(result.result(), Lengthd(0, Lengthd::Unit::None));
      } else if (name.equalsLowercase("translatey")) {
        auto result = parseSingleLengthPercentage(subparser);
        if (result.hasError()) {
          return std::move(result.error());
        }

        transform_.appendTranslate(Lengthd(0, Lengthd::Unit::None), result.result());
      } else if (name.equalsLowercase("scale")) {
        return parseScale(subparser);
      } else if (name.equalsLowercase("scalex")) {
        auto result = parseSingleNumber(subparser);
        if (result.hasError()) {
          return std::move(result.error());
        }

        transform_.appendTransform(Transform2d::Scale(Vector2d(result.result(), 1.0)));
      } else if (name.equalsLowercase("scaley")) {
        auto result = parseSingleNumber(subparser);
        if (result.hasError()) {
          return std::move(result.error());
        }

        transform_.appendTransform(Transform2d::Scale(Vector2d(1.0, result.result())));
      } else if (name.equalsLowercase("rotate")) {
        auto result = parseSingleAngle(subparser, AngleParseOptions::AllowBareZero);
        if (result.hasError()) {
          return std::move(result.error());
        }

        transform_.appendTransform(Transform2d::Rotate(result.result()));
      } else if (name.equalsLowercase("skew")) {
        return parseSkew(subparser);
      } else if (name.equalsLowercase("skewx")) {
        auto result = parseSingleAngle(subparser, AngleParseOptions::AllowBareZero);
        if (result.hasError()) {
          return std::move(result.error());
        }

        transform_.appendTransform(Transform2d::SkewX(result.result()));
      } else if (name.equalsLowercase("skewy")) {
        auto result = parseSingleAngle(subparser, AngleParseOptions::AllowBareZero);
        if (result.hasError()) {
          return std::move(result.error());
        }

        transform_.appendTransform(Transform2d::SkewY(result.result()));
      } else {
        ParseDiagnostic err;
        err.reason = "Unexpected function '" + name + "'";
        err.range = {parser_.sourceOffset(), parser_.sourceOffset()};
        return err;
      }
    } else {
      return TransformErrorAt("Expected a function, found unexpected token",
                              parser_.sourceOffset());
    }

    return std::nullopt;
  }

  ParseResult<double> parseSingleNumber(ComponentValueParser& subparser) {
    auto maybeNumber = subparser.readNumber();
    if (maybeNumber.hasError()) {
      return std::move(maybeNumber.error());
    }

    subparser.skipWhitespace();
    if (!subparser.isEOF()) {
      return TransformErrorAt("Expected only one parameter", subparser.sourceOffset());
    }

    return maybeNumber;
  }

  ParseResult<Lengthd> parseSingleLengthPercentage(ComponentValueParser& subparser) {
    auto maybeLength = subparser.readLengthPercentage();
    if (maybeLength.hasError()) {
      return std::move(maybeLength.error());
    }

    subparser.skipWhitespace();
    if (!subparser.isEOF()) {
      return TransformErrorAt("Expected only one parameter", subparser.sourceOffset());
    }

    return maybeLength;
  }

  ParseResult<double> parseSingleAngle(ComponentValueParser& subparser,
                                       AngleParseOptions options = AngleParseOptions::None) {
    auto maybeAngle = subparser.readAngle(options);
    if (maybeAngle.hasError()) {
      return std::move(maybeAngle.error());
    }

    subparser.skipWhitespace();
    if (!subparser.isEOF()) {
      return TransformErrorAt("Expected only one parameter", subparser.sourceOffset());
    }

    return maybeAngle;
  }

  std::optional<ParseDiagnostic> parseMatrix(ComponentValueParser& subparser) {
    Transform2d t(Transform2d::uninitialized);
    if (auto error = subparser.readNumbers(t.data)) {
      return std::move(error.value());
    }

    subparser.skipWhitespace();
    if (!subparser.isEOF()) {
      return TransformErrorAt("Unexpected parameters when parsing 'matrix'",
                              subparser.sourceOffset());
    }

    transform_.appendTransform(t);
    return std::nullopt;
  }

  std::optional<ParseDiagnostic> parseTranslate(ComponentValueParser& subparser) {
    // Accept either 1 or 2 lengths.
    auto maybeTx = subparser.readLengthPercentage();
    if (maybeTx.hasError()) {
      return std::move(maybeTx.error());
    }

    subparser.skipWhitespace();

    if (subparser.isEOF()) {
      // Only one parameter provided, use zero for Ty.
      transform_.appendTranslate(maybeTx.result(), Lengthd(0.0, Lengthd::Unit::None));
    } else {
      if (subparser.tryConsumeToken<css::Token::Comma>()) {
        subparser.skipWhitespace();
      } else {
        return TransformErrorAt("Expected a comma", subparser.sourceOffset());
      }

      auto maybeTy = subparser.readLengthPercentage();
      if (maybeTy.hasError()) {
        return std::move(maybeTy.error());
      }

      transform_.appendTranslate(maybeTx.result(), maybeTy.result());
    }

    subparser.skipWhitespace();

    if (!subparser.isEOF()) {
      return TransformErrorAt("Unexpected parameters when parsing 'translate'",
                              subparser.sourceOffset());
    }

    return std::nullopt;
  }

  std::optional<ParseDiagnostic> parseScale(ComponentValueParser& subparser) {
    // Accept either 1 or 2 numbers.
    auto maybeSx = subparser.readNumber();
    if (maybeSx.hasError()) {
      return std::move(maybeSx.error());
    }

    subparser.skipWhitespace();

    if (subparser.isEOF()) {
      // Only one parameter provided, use Sx for both x and y.
      transform_.appendTransform(Transform2d::Scale(Vector2d(maybeSx.result(), maybeSx.result())));
    } else {
      if (subparser.tryConsumeToken<css::Token::Comma>()) {
        subparser.skipWhitespace();
      } else {
        return TransformErrorAt("Expected a comma", subparser.sourceOffset());
      }

      auto maybeSy = subparser.readNumber();
      if (maybeSy.hasError()) {
        return std::move(maybeSy.error());
      }

      transform_.appendTransform(Transform2d::Scale(Vector2d(maybeSx.result(), maybeSy.result())));
    }

    subparser.skipWhitespace();

    if (!subparser.isEOF()) {
      return TransformErrorAt("Unexpected parameters when parsing 'scale'",
                              subparser.sourceOffset());
    }

    return std::nullopt;
  }

  std::optional<ParseDiagnostic> parseSkew(ComponentValueParser& subparser) {
    // Accept either 1 or 2 angles.
    auto maybeAlpha = subparser.readAngle(AngleParseOptions::AllowBareZero);
    if (maybeAlpha.hasError()) {
      return std::move(maybeAlpha.error());
    }

    subparser.skipWhitespace();

    if (subparser.isEOF()) {
      // Only one parameter provided, use zero for theta.
      transform_.appendTransform(Skew(maybeAlpha.result(), 0.0));
    } else {
      if (subparser.tryConsumeToken<css::Token::Comma>()) {
        subparser.skipWhitespace();
      } else {
        return TransformErrorAt("Expected a comma", subparser.sourceOffset());
      }

      auto maybeTheta = subparser.readAngle(AngleParseOptions::AllowBareZero);
      if (maybeTheta.hasError()) {
        return std::move(maybeTheta.error());
      }

      transform_.appendTransform(Skew(maybeAlpha.result(), maybeTheta.result()));
    }

    subparser.skipWhitespace();

    if (!subparser.isEOF()) {
      return TransformErrorAt("Unexpected parameters when parsing 'skew'",
                              subparser.sourceOffset());
    }

    return std::nullopt;
  }

private:
  ComponentValueParser parser_;
  CssTransform transform_;
};

}  // namespace

ParseResult<CssTransform> CssTransformParser::Parse(
    std::span<const css::ComponentValue> components) {
  CssTransformParserImpl parser(components);
  return parser.parse();
}

}  // namespace donner::svg::parser
