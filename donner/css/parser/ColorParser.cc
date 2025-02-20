#include "donner/css/parser/ColorParser.h"

#include <algorithm>
#include <numeric>

#include "donner/base/MathUtils.h"
#include "donner/css/parser/details/ComponentValueParser.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser {

namespace {

std::span<const css::ComponentValue> trimWhitespace(
    std::span<const css::ComponentValue> components) {
  while (!components.empty() && components.front().isToken<Token::Whitespace>()) {
    components = components.subspan(1);
  }

  while (!components.empty() && components.back().isToken<Token::Whitespace>()) {
    components = components.subspan(0, components.size() - 1);
  }

  return components;
}

class FunctionParameterParser {
public:
  explicit FunctionParameterParser(const RcString& functionName,
                                   std::span<const css::ComponentValue> components)
      : functionName_(functionName), components_(components) {
    advance();
  }

  ParseResult<css::Token> next() {
    if (next_) {
      auto result = std::move(next_.value());
      next_.reset();
      advance();
      return result;
    } else {
      ParseError err;
      err.reason = "Unexpected EOF when parsing function '" + functionName_ + "'";
      err.location = lastOffset_;
      return err;
    }
  }

  template <typename TokenType>
  ParseResult<TokenType> nextAs() {
    auto result = next();
    if (result.hasError()) {
      return std::move(result.error());
    }

    const auto& resultToken = result.result();
    if (resultToken.is<TokenType>()) {
      return resultToken.get<TokenType>();
    } else {
      ParseError err;
      err.reason = "Unexpected token when parsing function '" + functionName_ + "'";
      err.location = resultToken.offset();
      return err;
    }
  }

  /// @return true if a comma was found and skipped.
  bool trySkipComma() {
    const bool foundComma =
        next_ && next_.value().hasResult() && next_.value().result().is<Token::Comma>();
    if (foundComma) {
      next_.reset();
      advance();
    }
    return foundComma;
  }

  std::optional<ParseError> requireComma() {
    if (!trySkipComma()) {
      ParseError err;
      err.reason = "Missing comma when parsing function '" + functionName_ + "'";
      err.location = lastOffset_;
      return err;
    }

    return std::nullopt;
  }

  /// @return true if a slash was skipped.
  std::optional<ParseError> requireSlash() {
    if (next_ && next_.value().hasResult()) {
      const auto& nextResult = next_.value().result();
      if (nextResult.is<Token::Delim>() && nextResult.get<Token::Delim>().value == '/') {
        next_.reset();
        advance();
        return std::nullopt;
      }
    }

    ParseError err;
    err.reason = "Missing delimiter for alpha when parsing function '" + functionName_ + "'";
    err.location = lastOffset_;
    return err;
  }

  std::optional<ParseError> requireEOF() {
    if (!isEOF()) {
      ParseError err;
      err.reason = "Additional tokens when parsing function '" + functionName_ + "'";
      err.location = lastOffset_;
      return err;
    }

    return std::nullopt;
  }

  bool isEOF() const { return !next_; }

private:
  void advance() {
    while (!components_.empty()) {
      const css::ComponentValue& component(components_.front());
      if (component.is<Token>()) {
        const auto& token = component.get<Token>();
        lastOffset_ = token.offset();

        if (token.is<Token::Whitespace>()) {
          // Skip.
          components_ = components_.subspan(1);
        } else {
          next_ = std::move(token);
          components_ = components_.subspan(1);
          break;
        }

      } else {
        ParseError err;
        err.reason = "Unexpected token when parsing function '" + functionName_ + "'";
        err.location = component.sourceOffset();
        next_ = std::move(err);
        break;
      }
    }
  }

  RcString functionName_;
  std::span<const css::ComponentValue> components_;
  std::optional<ParseResult<css::Token>> next_;
  FileOffset lastOffset_ = FileOffset::Offset(0);
};

class ColorParserImpl {
public:
  ColorParserImpl(std::span<const css::ComponentValue> components)
      : components_(trimWhitespace(components)) {}

  ParseResult<Color> parseColor() {
    if (components_.empty()) {
      ParseError err;
      err.reason = "No color found";
      return err;
    } else if (components_.size() != 1) {
      ParseError err;
      err.reason = "Expected a single color";
      err.location = components_.front().sourceOffset();
      return err;
    }

    const auto& component = components_.front();

    if (component.is<Token>()) {
      auto token = std::move(component.get<Token>());
      if (token.is<Token::Hash>()) {
        return parseHash(token.get<Token::Hash>().name);
      } else if (token.is<Token::Ident>()) {
        auto ident = std::move(token.get<Token::Ident>());

        // Comparisons are case-insensitive, convert token to lowercase.
        std::string name = ident.value.str();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // TODO: <system-color>
        if (auto color = Color::ByName(name)) {
          return color.value();
        } else {
          ParseError err;
          err.reason = "Invalid color '" + name + "'";
          err.location = token.offset();
          return err;
        }

      } else {
        ParseError err;
        err.reason = "Unexpected token when parsing color";
        err.location = token.offset();
        return err;
      }
    } else if (component.is<css::Function>()) {
      const auto& f = component.get<css::Function>();
      const RcString& name = f.name;

      if (name.equalsLowercase("rgb") || name.equalsLowercase("rgba")) {
        return parseRgb(name, f.values);
      } else if (name.equalsLowercase("hsl") || name.equalsLowercase("hsla")) {
        return parseHsl(name, f.values);
      } else if (name.equalsLowercase("hwb")) {
        return parseHwb(name, f.values);
      } else if (name.equalsLowercase("lab")) {
        return parseLab(name, f.values);
      } else if (name.equalsLowercase("lch")) {
        return parseLch(name, f.values);
      } else if (name.equalsLowercase("color")) {
        return parseColorFunction(name, f.values);
      } else if (name.equalsLowercase("device-cmyk")) {
        return parseDeviceCmyk(name, f.values);
      } else {
        ParseError err;
        err.reason = "Unsupported color function '" + name + "'";
        err.location = f.sourceOffset;
        return err;
      }

    } else {
      ParseError err;
      err.reason = "Unexpected block when parsing color";
      err.location = component.sourceOffset();
      return err;
    }
  }

  ParseResult<Color> parseHash(std::string_view value) {
    if (!std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isxdigit(ch); })) {
      ParseError err;
      err.reason = "'#" + std::string(value) + "' is not a hex number";
      return err;
    }

    if (value.size() == 3) {
      return Color(RGBA::RGB(fromHex(value[0]) * 17,  //
                             fromHex(value[1]) * 17,  //
                             fromHex(value[2]) * 17));
    } else if (value.size() == 4) {
      return Color(RGBA(fromHex(value[0]) * 17,  //
                        fromHex(value[1]) * 17,  //
                        fromHex(value[2]) * 17,  //
                        fromHex(value[3]) * 17));
    } else if (value.size() == 6) {
      return Color(RGBA::RGB(fromHex(value[0]) * 16 + fromHex(value[1]),  //
                             fromHex(value[2]) * 16 + fromHex(value[3]),  //
                             fromHex(value[4]) * 16 + fromHex(value[5])));
    } else if (value.size() == 8) {
      return Color(RGBA(fromHex(value[0]) * 16 + fromHex(value[1]),  //
                        fromHex(value[2]) * 16 + fromHex(value[3]),  //
                        fromHex(value[4]) * 16 + fromHex(value[5]),  //
                        fromHex(value[6]) * 16 + fromHex(value[7])));
    } else {
      ParseError err;
      err.reason = "'#" + std::string(value) + "' is not a color";
      return err;
    }
  }

  ParseResult<Color> parseRgb(const RcString& functionName,
                              std::span<const css::ComponentValue> components) {
    FunctionParameterParser rgbParams(functionName, components);

    auto firstTokenResult = rgbParams.next();
    if (firstTokenResult.hasError()) {
      return std::move(firstTokenResult.error());
    }

    auto firstToken = std::move(firstTokenResult.result());
    const bool requiresCommas = rgbParams.trySkipComma();
    if (!firstToken.is<Token::Number>() && !firstToken.is<Token::Percentage>()) {
      return unexpectedTokenError(functionName, firstToken);
    }

    // Parse the RGB components first.
    auto rgbResult =
        firstToken.is<Token::Number>()
            ? parseGreenBlueAs<Token::Number>(firstToken, rgbParams, requiresCommas)
            : parseGreenBlueAs<Token::Percentage>(firstToken, rgbParams, requiresCommas);
    if (rgbResult.hasError()) {
      return std::move(rgbResult.error());
    }

    const RGBA rgb = rgbResult.result();
    auto alphaResult = tryParseOptionalAlpha(rgbParams, requiresCommas);
    if (alphaResult.hasError()) {
      return std::move(alphaResult.error());
    }

    return Color(RGBA(rgb.r, rgb.g, rgb.b, alphaResult.result()));
  }

  template <typename TokenType>
  ParseResult<RGBA> parseGreenBlueAs(const Token& firstToken, FunctionParameterParser& rgbParams,
                                     bool requiresCommas) {
    const double red = firstToken.get<TokenType>().value;

    auto greenResult = rgbParams.nextAs<TokenType>();
    if (greenResult.hasError()) {
      return std::move(greenResult.error());
    }

    if (requiresCommas) {
      if (auto error = rgbParams.requireComma()) {
        return std::move(error.value());
      }
    }

    auto blueResult = rgbParams.nextAs<TokenType>();
    if (blueResult.hasError()) {
      return std::move(blueResult.error());
    }

    if constexpr (std::is_same_v<TokenType, Token::Number>) {
      return RGBA::RGB(numberToChannel(red), numberToChannel(greenResult.result().value),
                       numberToChannel(blueResult.result().value));
    } else {
      return RGBA::RGB(percentageToChannel(red), percentageToChannel(greenResult.result().value),
                       percentageToChannel(blueResult.result().value));
    }
  }

  // Returns the hue in degrees if set.
  // Based on https://www.w3.org/TR/2021/WD-css-color-4-20210601/#hue-syntax and
  // https://www.w3.org/TR/css-values-3/#angles
  ParseResult<double> parseHue(FunctionParameterParser& params) {
    auto angleResult = params.next();
    if (angleResult.hasError()) {
      return std::move(angleResult.error());
    }

    auto angleToken = std::move(angleResult.result());
    if (angleToken.is<Token::Number>()) {
      return angleToken.get<Token::Number>().value;
    } else if (angleToken.is<Token::Dimension>()) {
      // TODO: Factor this out into a DimensionUtils or Angle type.
      const auto& dimension = angleToken.get<Token::Dimension>();
      const RcString suffix = dimension.suffixString;

      if (suffix.equalsLowercase("deg")) {
        return dimension.value;
      } else if (suffix.equalsLowercase("grad")) {
        return dimension.value / 400.0 * 360.0;
      } else if (suffix.equalsLowercase("rad")) {
        return dimension.value * MathConstants<double>::kRadToDeg;
      } else if (suffix.equalsLowercase("turn")) {
        return dimension.value * 360.0;
      } else {
        ParseError err;
        err.reason = "Angle has unexpected dimension '" + dimension.suffixString + "'";
        err.location = angleToken.offset();
        return err;
      }
    }

    ParseError err;
    err.reason = "Unexpected token when parsing angle";
    err.location = angleToken.offset();
    return err;
  }

  ParseResult<Color> parseHsl(const RcString& functionName,
                              std::span<const css::ComponentValue> components) {
    FunctionParameterParser hslParams(functionName, components);

    ParseResult<double> hueResult = parseHue(hslParams);
    if (hueResult.hasError()) {
      return std::move(hueResult.error());
    }

    const bool requiresCommas = hslParams.trySkipComma();

    // Parse the RGB components first.
    auto saturationResult = hslParams.nextAs<Token::Percentage>();
    if (saturationResult.hasError()) {
      return std::move(saturationResult.error());
    }

    if (requiresCommas) {
      if (auto error = hslParams.requireComma()) {
        return std::move(error.value());
      }
    }

    auto lightnessResult = hslParams.nextAs<Token::Percentage>();
    if (lightnessResult.hasError()) {
      return std::move(lightnessResult.error());
    }

    HSLA hsl =
        HSLA::HSL(static_cast<float>(normalizeAngleDegrees(hueResult.result())),
                  static_cast<float>(Clamp(saturationResult.result().value / 100.0, 0.0, 1.0)),
                  static_cast<float>(Clamp(lightnessResult.result().value / 100.0, 0.0, 1.0)));
    auto alphaResult = tryParseOptionalAlpha(hslParams, requiresCommas);
    if (alphaResult.hasError()) {
      return std::move(alphaResult.error());
    }

    hsl.a = alphaResult.result();

    return Color(hsl);
  }

  ParseResult<uint8_t> tryParseOptionalAlpha(FunctionParameterParser& params, bool requiresCommas) {
    if (params.isEOF()) {
      return 0xFF;
    }

    // Parse alpha, but first skip either a comma if commas are used, or a '/' if not.
    if (auto error = (requiresCommas ? params.requireComma() : params.requireSlash())) {
      return std::move(error.value());
    }

    auto alphaResult = parseAlpha(params);
    if (alphaResult.hasError()) {
      return std::move(alphaResult.error());
    }

    if (auto error = params.requireEOF()) {
      return std::move(error.value());
    }

    return alphaResult.result();
  }

  ParseResult<Color> parseHwb(const RcString& functionName,
                              std::span<const css::ComponentValue> components) {
    FunctionParameterParser hwbParams(functionName, components);

    ParseResult<double> hueResult = parseHue(hwbParams);
    if (hueResult.hasError()) {
      return std::move(hueResult.error());
    }

    const bool requiresCommas = hwbParams.trySkipComma();

    auto whitenessResult = hwbParams.nextAs<Token::Percentage>();
    if (whitenessResult.hasError()) {
      return std::move(whitenessResult.error());
    }

    if (requiresCommas) {
      if (auto error = hwbParams.requireComma()) {
        return std::move(error.value());
      }
    }

    auto blacknessResult = hwbParams.nextAs<Token::Percentage>();
    if (blacknessResult.hasError()) {
      return std::move(blacknessResult.error());
    }

    const RGBA rgb = hwbToRgb(normalizeAngleDegrees(hueResult.result()),
                              Clamp(whitenessResult.result().value / 100.0, 0.0, 1.0),
                              Clamp(blacknessResult.result().value / 100.0, 0.0, 1.0));
    auto alphaResult = tryParseOptionalAlpha(hwbParams, requiresCommas);
    if (alphaResult.hasError()) {
      return std::move(alphaResult.error());
    }

    return Color(RGBA(rgb.r, rgb.g, rgb.b, alphaResult.result()));
  }

  ParseResult<Color> parseLab(const RcString& functionName,
                              std::span<const css::ComponentValue> components) {
    FunctionParameterParser labParams(functionName, components);

    // Parse L component
    auto LResult = labParams.next();
    if (LResult.hasError()) {
      return std::move(LResult.error());
    }

    double L;
    if (LResult.result().is<Token::Percentage>()) {
      L = Clamp(LResult.result().get<Token::Percentage>().value, 0.0, 100.0);
    } else if (LResult.result().is<Token::Number>()) {
      L = Clamp(LResult.result().get<Token::Number>().value, 0.0, 100.0);
    } else {
      return unexpectedTokenError(functionName, LResult.result());
    }

    // Parse A component
    auto aResult = labParams.next();
    if (aResult.hasError()) {
      return std::move(aResult.error());
    }

    double a;
    if (aResult.result().is<Token::Percentage>()) {
      a = aResult.result().get<Token::Percentage>().value / 100.0 * 125.0;
      a = Clamp(a, -125.0, 125.0);
    } else if (aResult.result().is<Token::Number>()) {
      a = aResult.result().get<Token::Number>().value;
    } else {
      return unexpectedTokenError(functionName, aResult.result());
    }

    // Parse B component
    auto bResult = labParams.next();
    if (bResult.hasError()) {
      return std::move(bResult.error());
    }

    double b;
    if (bResult.result().is<Token::Percentage>()) {
      b = bResult.result().get<Token::Percentage>().value / 100.0 * 125.0;
      b = Clamp(b, -125.0, 125.0);
    } else if (bResult.result().is<Token::Number>()) {
      b = bResult.result().get<Token::Number>().value;
    } else {
      return unexpectedTokenError(functionName, bResult.result());
    }

    // Parse optional alpha
    double alpha = 1.0;
    if (!labParams.isEOF()) {
      if (auto error = labParams.requireSlash()) {
        return std::move(error.value());
      }
      auto alphaResult = parseAlpha(labParams);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result() / 255.0;
    }

    if (auto error = labParams.requireEOF()) {
      return std::move(error.value());
    }

    // Convert Lab to RGB
    const RGBA rgb = labToSRGB(L, a, b, alpha);
    return Color(rgb);
  }

  ParseResult<Color> parseLch(const RcString& functionName,
                              std::span<const css::ComponentValue> components) {
    FunctionParameterParser lchParams(functionName, components);

    // Parse L component
    auto LResult = lchParams.next();
    if (LResult.hasError()) {
      return std::move(LResult.error());
    }

    double L;
    if (LResult.result().is<Token::Percentage>()) {
      L = Clamp(LResult.result().get<Token::Percentage>().value, 0.0, 100.0);
    } else if (LResult.result().is<Token::Number>()) {
      L = Clamp(LResult.result().get<Token::Number>().value, 0.0, 100.0);
    } else {
      return unexpectedTokenError(functionName, LResult.result());
    }

    // Parse C component
    auto CResult = lchParams.next();
    if (CResult.hasError()) {
      return std::move(CResult.error());
    }

    double C;
    if (CResult.result().is<Token::Percentage>()) {
      C = Clamp(CResult.result().get<Token::Percentage>().value / 100.0 * 150.0, 0.0, 150.0);
    } else if (CResult.result().is<Token::Number>()) {
      C = std::max(0.0, CResult.result().get<Token::Number>().value);
    } else {
      return unexpectedTokenError(functionName, CResult.result());
    }

    // Parse H component
    auto hueResult = parseHue(lchParams);
    if (hueResult.hasError()) {
      return std::move(hueResult.error());
    }
    const double H = normalizeAngleDegrees(hueResult.result());

    // Parse optional alpha
    double alpha = 1.0;
    if (!lchParams.isEOF()) {
      if (auto error = lchParams.requireSlash()) {
        return std::move(error.value());
      }
      auto alphaResult = parseAlpha(lchParams);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result() / 255.0;
    }

    if (auto error = lchParams.requireEOF()) {
      return std::move(error.value());
    }

    // Convert LCH to Lab
    double a, b;
    lchToLab(L, C, H, a, b);
    // Convert Lab to RGB
    const RGBA rgb = labToSRGB(L, a, b, alpha);
    return Color(rgb);
  }

  void lchToLab(double L, double C, double H_deg, double& a, double& b) {
    const double H_rad = H_deg * MathConstants<double>::kDegToRad;
    a = C * cos(H_rad);
    b = C * sin(H_rad);
  }

  ParseResult<Color> parseColorFunction(const RcString& functionName,
                                        std::span<const css::ComponentValue> components) {
    ParseError err;
    err.reason = "Not implemented";
    return err;
  }

  ParseResult<Color> parseDeviceCmyk(const RcString& functionName,
                                     std::span<const css::ComponentValue> components) {
    ParseError err;
    err.reason = "Not implemented";
    return err;
  }

  ParseResult<uint8_t> parseAlpha(FunctionParameterParser& params) {
    auto alphaResult = params.next();
    if (alphaResult.hasError()) {
      return std::move(alphaResult.error());
    }

    auto alphaToken = std::move(alphaResult.result());
    if (alphaToken.is<Token::Number>()) {
      return numberToAlpha(alphaToken.get<Token::Number>().value);
    } else if (alphaToken.is<Token::Percentage>()) {
      return percentageToChannel(alphaToken.get<Token::Percentage>().value);
    } else {
      ParseError err;
      err.reason = "Unexpected alpha value";
      err.location = alphaToken.offset();
      return err;
    }
  }

private:
  ParseError unexpectedTokenError(const RcString& functionName, const Token& token) {
    ParseError err;
    err.reason = "Unexpected token when parsing function '" + functionName + "'";
    err.location = token.offset();
    return err;
  }

  static RGBA hwbToRgb(double hue, double white, double black) {
    if (white + black >= 1) {
      const uint8_t gray = numberToChannel(white / (white + black));
      return RGBA::RGB(gray, gray, gray);
    }

    HSLA hsl = HSLA::HSL(static_cast<float>(hue), 1.0f, 0.5f);
    float* hslComponents[3] = {&hsl.hDeg, &hsl.s, &hsl.l};

    for (int i = 0; i < 3; i++) {
      *hslComponents[i] *= static_cast<float>(1 - white - black);
      *hslComponents[i] += static_cast<float>(white);
    }

    return hsl.toRGBA();
  }

  static double normalizeAngleDegrees(double angleDegrees) {
    return angleDegrees - std::floor(angleDegrees / 360.0) * 360.0;
  }

  static uint8_t numberToChannel(double number) {
    return static_cast<uint8_t>(Clamp(Round(number), 0.0, 255.0));
  }

  static uint8_t percentageToChannel(double number) {
    // Convert 100 -> 255.
    return numberToChannel(number * 2.55);
  }

  static uint8_t numberToAlpha(double number) {
    // Like numberToChannel, except in the range of [0, 1].
    return static_cast<uint8_t>(Clamp(Round(number * 255.0), 0.0, 255.0));
  }

  static unsigned int fromHex(unsigned char ch) {
    assert(std::isxdigit(ch));

    if (ch >= 'a' && ch <= 'f') {
      return 10 + ch - 'a';
    } else if (ch >= 'A' && ch <= 'F') {
      return 10 + ch - 'A';
    } else {
      return ch - '0';
    }
  }

  // Helper function to convert Lab to sRGB
  RGBA labToSRGB(double L, double a, double b, double alpha) {
    // Convert Lab to XYZ (D50)
    double X_D50, Y_D50, Z_D50;
    labToXYZ(L, a, b, X_D50, Y_D50, Z_D50);

    // Adapt XYZ from D50 to D65
    double X_D65, Y_D65, Z_D65;
    adaptD50toD65(X_D50, Y_D50, Z_D50, X_D65, Y_D65, Z_D65);

    // Convert XYZ (D65) to linear sRGB
    double r_lin, g_lin, b_lin;
    xyzToLinearRGB(X_D65, Y_D65, Z_D65, r_lin, g_lin, b_lin);

    // Convert linear sRGB to sRGB
    const uint8_t R = linearToSRGB(r_lin);
    const uint8_t G = linearToSRGB(g_lin);
    const uint8_t B = linearToSRGB(b_lin);

    return RGBA(R, G, B, static_cast<uint8_t>(alpha * 255.0));
  }

  // Convert Lab to XYZ
  void labToXYZ(double L, double a, double b, double& X, double& Y, double& Z) {
    // Normalize L*, a*, b*
    const double κ = 24389.0 / 27.0;   // 29^3 / 3^3
    const double ε = 216.0 / 24389.0;  // 6^3 / 29^3

    const double fy = (L + 16.0) / 116.0;
    const double fx = fy + (a / 500.0);
    const double fz = fy - (b / 200.0);

    const double fx3 = fx * fx * fx;
    const double fz3 = fz * fz * fz;

    const double xr = fx3 > ε ? fx3 : (116.0 * fx - 16.0) / κ;
    const double yr = L > (κ * ε) ? pow((L + 16.0) / 116.0, 3) : L / κ;
    const double zr = fz3 > ε ? fz3 : (116.0 * fz - 16.0) / κ;

    // Reference white D50
    const double Xn = 0.96422;  // Xn, Yn, Zn for D50
    const double Yn = 1.0;
    const double Zn = 0.82521;

    X = xr * Xn;
    Y = yr * Yn;
    Z = zr * Zn;
  }

  void adaptD50toD65(double X_D50, double Y_D50, double Z_D50, double& X_D65, double& Y_D65,
                     double& Z_D65) {
    // Bradford transformation matrix
    const double M[3][3] = {{0.9554734527042182, -0.0230985368742614, 0.0632593086610217},
                            {-0.0283697069632081, 1.0099954580058226, 0.0210413989669430},
                            {0.0123140016883199, -0.0205076964334771, 1.3303659908427779}};

    X_D65 = M[0][0] * X_D50 + M[0][1] * Y_D50 + M[0][2] * Z_D50;
    Y_D65 = M[1][0] * X_D50 + M[1][1] * Y_D50 + M[1][2] * Z_D50;
    Z_D65 = M[2][0] * X_D50 + M[2][1] * Y_D50 + M[2][2] * Z_D50;
  }

  // Convert XYZ to linear RGB
  void xyzToLinearRGB(double X, double Y, double Z, double& r, double& g, double& b) {
    // sRGB conversion matrix (D65)
    r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
  }

  // Convert linear RGB to sRGB
  uint8_t linearToSRGB(double value) {
    double v = value <= 0.0031308 ? 12.92 * value : 1.055 * pow(value, 1.0 / 2.4) - 0.055;

    // Ensure the value is within [0,1]
    v = Clamp(v, 0.0, 1.0);
    return static_cast<uint8_t>(Round(v * 255.0));
  }

  std::span<const css::ComponentValue> components_;
};

}  // namespace

ParseResult<Color> ColorParser::Parse(std::span<const css::ComponentValue> components) {
  ColorParserImpl parser(components);
  return parser.parseColor();
}

ParseResult<Color> ColorParser::ParseString(std::string_view str) {
  details::Tokenizer tokenizer(str);
  const std::vector<ComponentValue> componentValues =
      details::parseListOfComponentValues(tokenizer);
  ColorParserImpl parser(componentValues);
  return parser.parseColor();
}

}  // namespace donner::css::parser
