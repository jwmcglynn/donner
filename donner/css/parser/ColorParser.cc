#include "donner/css/parser/ColorParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <optional>
#include <string_view>

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

std::span<const css::ComponentValue> trimLeadingWhitespace(
    std::span<const css::ComponentValue> components);

bool isRelativeColorInvocation(std::span<const css::ComponentValue> components) {
  const auto trimmed = trimWhitespace(components);
  if (trimmed.empty()) {
    return false;
  }

  if (!trimmed.front().is<Token>()) {
    return false;
  }

  const auto& token = trimmed.front().get<Token>();
  return token.is<Token::Ident>() && token.get<Token::Ident>().value.equalsLowercase("from");
}

double decodeSRGB(double value) {
  return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

struct RelativeBaseColor {
  RGBA rgba;
  std::span<const css::ComponentValue> remainder;
  FileOffset baseOffset = FileOffset::Offset(0);
  std::optional<HSLA> baseHsl;
  std::optional<ColorSpaceValue> baseSpace;
};

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Matrix3x3 {
  double m[3][3] = {};
};

Vec3 multiply(const Matrix3x3& matrix, const Vec3& v) {
  return {.x = matrix.m[0][0] * v.x + matrix.m[0][1] * v.y + matrix.m[0][2] * v.z,
          .y = matrix.m[1][0] * v.x + matrix.m[1][1] * v.y + matrix.m[1][2] * v.z,
          .z = matrix.m[2][0] * v.x + matrix.m[2][1] * v.y + matrix.m[2][2] * v.z};
}

Matrix3x3 invert(const Matrix3x3& matrix) {
  const double det =
      matrix.m[0][0] * (matrix.m[1][1] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][1]) -
      matrix.m[0][1] * (matrix.m[1][0] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][0]) +
      matrix.m[0][2] * (matrix.m[1][0] * matrix.m[2][1] - matrix.m[1][1] * matrix.m[2][0]);

  const double invDet = 1.0 / det;

  Matrix3x3 result;
  result.m[0][0] = (matrix.m[1][1] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][1]) * invDet;
  result.m[0][1] = (matrix.m[0][2] * matrix.m[2][1] - matrix.m[0][1] * matrix.m[2][2]) * invDet;
  result.m[0][2] = (matrix.m[0][1] * matrix.m[1][2] - matrix.m[0][2] * matrix.m[1][1]) * invDet;

  result.m[1][0] = (matrix.m[1][2] * matrix.m[2][0] - matrix.m[1][0] * matrix.m[2][2]) * invDet;
  result.m[1][1] = (matrix.m[0][0] * matrix.m[2][2] - matrix.m[0][2] * matrix.m[2][0]) * invDet;
  result.m[1][2] = (matrix.m[0][2] * matrix.m[1][0] - matrix.m[0][0] * matrix.m[1][2]) * invDet;

  result.m[2][0] = (matrix.m[1][0] * matrix.m[2][1] - matrix.m[1][1] * matrix.m[2][0]) * invDet;
  result.m[2][1] = (matrix.m[0][1] * matrix.m[2][0] - matrix.m[0][0] * matrix.m[2][1]) * invDet;
  result.m[2][2] = (matrix.m[0][0] * matrix.m[1][1] - matrix.m[0][1] * matrix.m[1][0]) * invDet;

  return result;
}

constexpr Matrix3x3 kDisplayP3ToXyzD65 = {
    .m = {{0.4865709486482162, 0.26566769316909294, 0.1982172852343625},
          {0.2289745640697488, 0.6917385218365064, 0.079286914093745},
          {0.0, 0.04511338185890264, 1.043944368900976}}};

constexpr Matrix3x3 kA98RgbToXyzD65 = {
    .m = {{0.5766690429101305, 0.1855582379065463, 0.1882286462349947},
          {0.2973449752505361, 0.6273635662554661, 0.0752914584939979},
          {0.02703136138641234, 0.07068885253582723, 0.9913375368376388}}};

constexpr Matrix3x3 kProPhotoToXyzD50 = {
    .m = {{0.7977604896723027, 0.13518583717574031, 0.0313493495815248},
          {0.2880711282292934, 0.7118432178101014, 0.00008565396060525902},
          {0.0, 0.0, 0.8251046025104601}}};

constexpr Matrix3x3 kRec2020ToXyzD65 = {
    .m = {{0.6369580483012914, 0.14461690358620832, 0.1688809751641721},
          {0.2627002120112671, 0.6779980715188708, 0.05930171646986196},
          {0.0, 0.028072693049087428, 1.060985057710791}}};

class RelativeComponentStream {
public:
  RelativeComponentStream(const RcString& functionName,
                          std::span<const css::ComponentValue> components)
      : functionName_(functionName), components_(trimLeadingWhitespace(components)) {}

  ParseResult<Token> next(bool eofIsError) {
    while (!components_.empty()) {
      const auto& component = components_.front();
      if (!component.is<Token>()) {
        ParseError err;
        err.reason = "Unexpected token when parsing function '" + functionName_ + "'";
        err.location = component.sourceOffset();
        return err;
      }

      const auto& token = component.get<Token>();
      components_ = components_.subspan(1);
      if (token.is<Token::Whitespace>()) {
        continue;
      }
      lastOffset_ = token.offset();
      return token;
    }

    if (eofIsError) {
      ParseError err;
      err.reason = "Unexpected EOF when parsing function '" + functionName_ + "'";
      err.location = lastOffset_;
      return err;
    }

    ParseError err;
    err.reason = "Additional tokens when parsing function '" + functionName_ + "'";
    err.location = lastOffset_;
    return err;
  }

  std::optional<ParseError> requireEOF() const {
    for (const auto& component : components_) {
      if (!component.is<Token>()) {
        ParseError err;
        err.reason = "Unexpected token when parsing function '" + functionName_ + "'";
        err.location = component.sourceOffset();
        return err;
      }

      const auto& token = component.get<Token>();
      if (token.is<Token::Whitespace>()) {
        continue;
      }

      ParseError err;
      err.reason = "Additional tokens when parsing function '" + functionName_ + "'";
      err.location = token.offset();
      return err;
    }
    return std::nullopt;
  }

  ParseResult<bool> trySkipSlash() {
    auto trimmed = trimLeadingWhitespace(components_);
    if (trimmed.empty()) {
      return false;
    }

    if (!trimmed.front().is<Token>()) {
      ParseError err;
      err.reason = "Unexpected token when parsing function '" + functionName_ + "'";
      err.location = trimmed.front().sourceOffset();
      return err;
    }

    const auto& token = trimmed.front().get<Token>();
    if (token.is<Token::Delim>() && token.get<Token::Delim>().value == '/') {
      components_ = trimmed.subspan(1);
      lastOffset_ = token.offset();
      return true;
    }

    return false;
  }

private:
  RcString functionName_;
  std::span<const css::ComponentValue> components_;
  FileOffset lastOffset_ = FileOffset::Offset(0);
};

struct HslComponents {
  double h = 0.0;
  double s = 0.0;
  double l = 0.0;
  double alpha = 1.0;
};

struct HwbComponents {
  double h = 0.0;
  double w = 0.0;
  double b = 0.0;
  double alpha = 1.0;
};

struct LabComponents {
  double l = 0.0;
  double a = 0.0;
  double b = 0.0;
  double alpha = 1.0;
};

struct LchComponents {
  double l = 0.0;
  double c = 0.0;
  double h = 0.0;
  double alpha = 1.0;
};

struct OklabComponents {
  double l = 0.0;
  double a = 0.0;
  double b = 0.0;
  double alpha = 1.0;
};

struct OklchComponents {
  double l = 0.0;
  double c = 0.0;
  double h = 0.0;
  double alpha = 1.0;
};

double srgbChannelToLinear(uint8_t channel) {
  return decodeSRGB(static_cast<double>(channel) / 255.0);
}

Vec3 srgbToXyzD65(const RGBA& rgba) {
  const double r = srgbChannelToLinear(rgba.r);
  const double g = srgbChannelToLinear(rgba.g);
  const double b = srgbChannelToLinear(rgba.b);

  return {.x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b,
          .y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
          .z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b};
}

Vec3 adaptD65ToD50(const Vec3& xyzD65) {
  const double m[3][3] = {{1.0479298208405488, 0.022946793341019088, -0.05019222954313557},
                          {0.02962780877005599, 0.9904344267538799, -0.017073799063418826},
                          {-0.00924304064620458, 0.015055191490297563, 0.7518742838215236}};

  return {.x = m[0][0] * xyzD65.x + m[0][1] * xyzD65.y + m[0][2] * xyzD65.z,
          .y = m[1][0] * xyzD65.x + m[1][1] * xyzD65.y + m[1][2] * xyzD65.z,
          .z = m[2][0] * xyzD65.x + m[2][1] * xyzD65.y + m[2][2] * xyzD65.z};
}

Vec3 adaptD50ToD65(const Vec3& xyzD50) {
  const double m[3][3] = {{0.9554734214880751, -0.023098536874261423, 0.0632593086610217},
                          {-0.02836970933386371, 1.0099954580058226, 0.021041398966943008},
                          {0.012314001688319899, -0.020507696433477912, 1.3303659366080753}};

  return {.x = m[0][0] * xyzD50.x + m[0][1] * xyzD50.y + m[0][2] * xyzD50.z,
          .y = m[1][0] * xyzD50.x + m[1][1] * xyzD50.y + m[1][2] * xyzD50.z,
          .z = m[2][0] * xyzD50.x + m[2][1] * xyzD50.y + m[2][2] * xyzD50.z};
}

double encodeSRGB(double value) {
  if (value <= 0.0031308) {
    return Clamp(value * 12.92, 0.0, 1.0);
  }

  return Clamp(1.055 * pow(value, 1.0 / 2.4) - 0.055, 0.0, 1.0);
}

double encodeGammaSigned(double value, double gamma) {
  const double magnitude = pow(std::abs(value), gamma);
  return std::copysign(magnitude, value);
}

double encodeA98(double value) {
  return encodeGammaSigned(value, 256.0 / 563.0);
}

double encodeProPhoto(double value) {
  if (value < 0.001953125) {
    return value * 16.0;
  }
  return pow(value, 1.0 / 1.8);
}

double encodeRec2020(double value) {
  if (value < 0.018053968510807)
    return value * 4.5;

  return 1.099 * pow(value, 0.45) - 0.099;
}

double labComponent(double t) {
  const double epsilon = 216.0 / 24389.0;
  const double kappa = 24389.0 / 27.0;
  if (t > epsilon) {
    return cbrt(t);
  }
  return (kappa * t + 16.0) / 116.0;
}

LabComponents rgbaToLab(const RGBA& rgba) {
  const Vec3 xyzD65 = srgbToXyzD65(rgba);
  const Vec3 xyzD50 = adaptD65ToD50(xyzD65);

  const double xn = 0.96422;
  const double yn = 1.0;
  const double zn = 0.82521;

  const double fx = labComponent(xyzD50.x / xn);
  const double fy = labComponent(xyzD50.y / yn);
  const double fz = labComponent(xyzD50.z / zn);

  LabComponents result;
  result.l = 116.0 * fy - 16.0;
  result.a = 500.0 * (fx - fy);
  result.b = 200.0 * (fy - fz);
  result.alpha = static_cast<double>(rgba.a) / 255.0;
  return result;
}

LchComponents rgbaToLch(const RGBA& rgba) {
  const LabComponents lab = rgbaToLab(rgba);
  LchComponents result;
  result.l = lab.l;
  result.c = std::sqrt(lab.a * lab.a + lab.b * lab.b);
  result.h = std::atan2(lab.b, lab.a) * MathConstants<double>::kRadToDeg;
  if (result.h < 0.0) {
    result.h += 360.0;
  }
  result.alpha = lab.alpha;
  return result;
}

OklabComponents rgbaToOklab(const RGBA& rgba) {
  const double r = srgbChannelToLinear(rgba.r);
  const double g = srgbChannelToLinear(rgba.g);
  const double b = srgbChannelToLinear(rgba.b);

  const double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);

  OklabComponents result;
  result.l = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  result.a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  result.b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  result.alpha = static_cast<double>(rgba.a) / 255.0;
  return result;
}

OklchComponents rgbaToOklch(const RGBA& rgba) {
  const OklabComponents lab = rgbaToOklab(rgba);
  OklchComponents result;
  result.l = lab.l;
  result.c = std::sqrt(lab.a * lab.a + lab.b * lab.b);
  result.h = std::atan2(lab.b, lab.a) * MathConstants<double>::kRadToDeg;
  if (result.h < 0.0) {
    result.h += 360.0;
  }
  result.alpha = lab.alpha;
  return result;
}

HslComponents rgbaToHsl(const RGBA& rgba) {
  const double r = static_cast<double>(rgba.r) / 255.0;
  const double g = static_cast<double>(rgba.g) / 255.0;
  const double b = static_cast<double>(rgba.b) / 255.0;

  const double maxVal = std::max({r, g, b});
  const double minVal = std::min({r, g, b});
  const double delta = maxVal - minVal;

  HslComponents result;
  result.l = (maxVal + minVal) / 2.0;

  if (delta == 0.0) {
    result.h = 0.0;
    result.s = 0.0;
  } else {
    result.s = delta / (1.0 - std::abs(2.0 * result.l - 1.0));

    if (maxVal == r) {
      result.h = 60.0 * std::fmod((g - b) / delta, 6.0);
    } else if (maxVal == g) {
      result.h = 60.0 * ((b - r) / delta + 2.0);
    } else {
      result.h = 60.0 * ((r - g) / delta + 4.0);
    }

    if (result.h < 0.0) {
      result.h += 360.0;
    }
  }

  result.alpha = static_cast<double>(rgba.a) / 255.0;
  return result;
}

HwbComponents rgbaToHwb(const RGBA& rgba) {
  const double r = static_cast<double>(rgba.r) / 255.0;
  const double g = static_cast<double>(rgba.g) / 255.0;
  const double b = static_cast<double>(rgba.b) / 255.0;

  const double maxVal = std::max({r, g, b});
  const double minVal = std::min({r, g, b});

  HwbComponents result;
  result.w = minVal;
  result.b = 1.0 - maxVal;

  const double delta = maxVal - minVal;
  if (delta == 0.0) {
    result.h = 0.0;
  } else if (maxVal == r) {
    result.h = 60.0 * std::fmod((g - b) / delta, 6.0);
  } else if (maxVal == g) {
    result.h = 60.0 * ((b - r) / delta + 2.0);
  } else {
    result.h = 60.0 * ((r - g) / delta + 4.0);
  }

  if (result.h < 0.0) {
    result.h += 360.0;
  }

  result.alpha = static_cast<double>(rgba.a) / 255.0;
  return result;
}

std::span<const css::ComponentValue> trimLeadingWhitespace(
    std::span<const css::ComponentValue> components) {
  while (!components.empty()) {
    if (components.front().is<Token>() &&
        components.front().get<Token>().is<Token::Whitespace>()) {
      components = components.subspan(1);
      continue;
    }
    break;
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
  ColorParserImpl(std::span<const css::ComponentValue> components, ColorParser::Options options)
      : components_(trimWhitespace(components)), options_(std::move(options)) {}

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
      } else if (name.equalsLowercase("oklab")) {
        return parseOklab(name, f.values);
      } else if (name.equalsLowercase("oklch")) {
        return parseOklch(name, f.values);
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
    if (isRelativeColorInvocation(components)) {
      return parseRelativeRgb(functionName, components);
    }

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
  // Based on https://www.w3.org/TR/2025/CRD-css-color-4-20250424/#hue-syntax and
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
    if (isRelativeColorInvocation(components)) {
      return parseRelativeHsl(functionName, components);
    }

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
    if (isRelativeColorInvocation(components)) {
      return parseRelativeHwb(functionName, components);
    }

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

    ColorSpaceValue value;
    value.id = ColorSpaceId::Hwb;
    value.c1 = normalizeAngleDegrees(hueResult.result());
    value.c2 = Clamp(whitenessResult.result().value / 100.0, 0.0, 1.0);
    value.c3 = Clamp(blacknessResult.result().value / 100.0, 0.0, 1.0);

    auto alphaResult = tryParseOptionalAlpha(hwbParams, requiresCommas);
    if (alphaResult.hasError()) {
      return std::move(alphaResult.error());
    }

    value.alpha = alphaResult.result();

    return Color(value);
  }

  ParseResult<Color> parseLab(const RcString& functionName,
                              std::span<const css::ComponentValue> components) {
    if (isRelativeColorInvocation(components)) {
      return parseRelativeLab(functionName, components);
    }

    FunctionParameterParser labParams(functionName, components);

    // Parse L component
    auto lResult = labParams.next();
    if (lResult.hasError()) {
      return std::move(lResult.error());
    }

    double l;
    if (lResult.result().is<Token::Percentage>()) {
      l = Clamp(lResult.result().get<Token::Percentage>().value, 0.0, 100.0);
    } else if (lResult.result().is<Token::Number>()) {
      l = Clamp(lResult.result().get<Token::Number>().value, 0.0, 100.0);
    } else {
      return unexpectedTokenError(functionName, lResult.result());
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

    ColorSpaceValue value;
    value.id = ColorSpaceId::Lab;
    value.c1 = l;
    value.c2 = a;
    value.c3 = b;
    value.alpha = static_cast<uint8_t>(alpha * 255.0);

    return Color(value);
  }

  ParseResult<Color> parseLch(const RcString& functionName,
                              std::span<const css::ComponentValue> components) {
    if (isRelativeColorInvocation(components)) {
      return parseRelativeLch(functionName, components);
    }

    FunctionParameterParser lchParams(functionName, components);

    // Parse L component
    auto lResult = lchParams.next();
    if (lResult.hasError()) {
      return std::move(lResult.error());
    }

    double l;
    if (lResult.result().is<Token::Percentage>()) {
      l = Clamp(lResult.result().get<Token::Percentage>().value, 0.0, 100.0);
    } else if (lResult.result().is<Token::Number>()) {
      l = Clamp(lResult.result().get<Token::Number>().value, 0.0, 100.0);
    } else {
      return unexpectedTokenError(functionName, lResult.result());
    }

    // Parse C component
    auto cResult = lchParams.next();
    if (cResult.hasError()) {
      return std::move(cResult.error());
    }

    double c;
    if (cResult.result().is<Token::Percentage>()) {
      c = Clamp(cResult.result().get<Token::Percentage>().value / 100.0 * 150.0, 0.0, 150.0);
    } else if (cResult.result().is<Token::Number>()) {
      c = std::max(0.0, cResult.result().get<Token::Number>().value);
    } else {
      return unexpectedTokenError(functionName, cResult.result());
    }

    // Parse H component
    auto hueResult = parseHue(lchParams);
    if (hueResult.hasError()) {
      return std::move(hueResult.error());
    }
    const double h = normalizeAngleDegrees(hueResult.result());

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

    ColorSpaceValue value;
    value.id = ColorSpaceId::Lch;
    value.c1 = l;
    value.c2 = c;
    value.c3 = h;
    value.alpha = static_cast<uint8_t>(alpha * 255.0);

    return Color(value);
  }

  ParseResult<Color> parseOklab(const RcString& functionName,
                                std::span<const css::ComponentValue> components) {
    if (isRelativeColorInvocation(components)) {
      return parseRelativeOklab(functionName, components);
    }

    FunctionParameterParser params(functionName, components);

    // Parse L component
    auto lResult = params.next();
    if (lResult.hasError()) {
      return std::move(lResult.error());
    }

    double l;
    if (lResult.result().is<Token::Percentage>()) {
      l = Clamp(lResult.result().get<Token::Percentage>().value / 100.0, 0.0, 1.0);
    } else if (lResult.result().is<Token::Number>()) {
      l = Clamp(lResult.result().get<Token::Number>().value, 0.0, 1.0);
    } else {
      return unexpectedTokenError(functionName, lResult.result());
    }

    // Parse a component
    auto aResult = params.next();
    if (aResult.hasError()) {
      return std::move(aResult.error());
    }

    double a;
    if (aResult.result().is<Token::Percentage>()) {
      a = Clamp(aResult.result().get<Token::Percentage>().value / 100.0 * 0.4, -0.4, 0.4);
    } else if (aResult.result().is<Token::Number>()) {
      a = aResult.result().get<Token::Number>().value;
    } else {
      return unexpectedTokenError(functionName, aResult.result());
    }

    // Parse b component
    auto bResult = params.next();
    if (bResult.hasError()) {
      return std::move(bResult.error());
    }

    double b;
    if (bResult.result().is<Token::Percentage>()) {
      b = Clamp(bResult.result().get<Token::Percentage>().value / 100.0 * 0.4, -0.4, 0.4);
    } else if (bResult.result().is<Token::Number>()) {
      b = bResult.result().get<Token::Number>().value;
    } else {
      return unexpectedTokenError(functionName, bResult.result());
    }

    // Parse optional alpha
    double alpha = 1.0;
    if (!params.isEOF()) {
      if (auto error = params.requireSlash()) {
        return std::move(error.value());
      }
      auto alphaResult = parseAlpha(params);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result() / 255.0;
    }

    if (auto error = params.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = ColorSpaceId::Oklab;
    value.c1 = l;
    value.c2 = a;
    value.c3 = b;
    value.alpha = static_cast<uint8_t>(alpha * 255.0);

    return Color(value);
  }

  ParseResult<Color> parseOklch(const RcString& functionName,
                                std::span<const css::ComponentValue> components) {
    if (isRelativeColorInvocation(components)) {
      return parseRelativeOklch(functionName, components);
    }

    FunctionParameterParser params(functionName, components);

    // Parse L component
    auto lResult = params.next();
    if (lResult.hasError()) {
      return std::move(lResult.error());
    }

    double l;
    if (lResult.result().is<Token::Percentage>()) {
      l = Clamp(lResult.result().get<Token::Percentage>().value / 100.0, 0.0, 1.0);
    } else if (lResult.result().is<Token::Number>()) {
      l = Clamp(lResult.result().get<Token::Number>().value, 0.0, 1.0);
    } else {
      return unexpectedTokenError(functionName, lResult.result());
    }

    // Parse C component
    auto cResult = params.next();
    if (cResult.hasError()) {
      return std::move(cResult.error());
    }

    double c;
    if (cResult.result().is<Token::Percentage>()) {
      c = Clamp(cResult.result().get<Token::Percentage>().value / 100.0 * 0.4, 0.0, 0.4);
    } else if (cResult.result().is<Token::Number>()) {
      c = std::max(0.0, cResult.result().get<Token::Number>().value);
    } else {
      return unexpectedTokenError(functionName, cResult.result());
    }

    // Parse H component
    auto hueResult = parseHue(params);
    if (hueResult.hasError()) {
      return std::move(hueResult.error());
    }
    const double h = normalizeAngleDegrees(hueResult.result());

    // Parse optional alpha
    double alpha = 1.0;
    if (!params.isEOF()) {
      if (auto error = params.requireSlash()) {
        return std::move(error.value());
      }
      auto alphaResult = parseAlpha(params);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result() / 255.0;
    }

    if (auto error = params.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = ColorSpaceId::Oklch;
    value.c1 = l;
    value.c2 = c;
    value.c3 = h;
    value.alpha = static_cast<uint8_t>(alpha * 255.0);

    return Color(value);
  }

  void oklchToOklab(double l, double c, double hDeg, double& a, double& b) {
    const double hRad = hDeg * MathConstants<double>::kDegToRad;
    a = c * cos(hRad);
    b = c * sin(hRad);
  }

  ParseResult<Color> parseColorFunction(const RcString& functionName,
                                        std::span<const css::ComponentValue> components) {
    if (isRelativeColorInvocation(components)) {
      return parseRelativeColorFunction(functionName, components);
    }

    FunctionParameterParser params(functionName, components);

    auto identTokenResult = params.next();
    if (identTokenResult.hasError()) {
      return std::move(identTokenResult.error());
    }
    const auto& identToken = identTokenResult.result();
    if (!identToken.is<Token::Ident>()) {
      return unexpectedTokenError(functionName, identToken);
    }
    std::string space = identToken.get<Token::Ident>().value.str();
    std::transform(space.begin(), space.end(), space.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const auto spaceId = resolveColorSpace(space);
    if (!spaceId) {
      ParseError err;
      err.reason = "Unsupported color space '" + space + "'";
      err.location = identToken.offset();
      return err;
    }

    double comps[3];
    for (int i = 0; i < 3; ++i) {
      auto vResult = params.next();
      if (vResult.hasError()) {
        return std::move(vResult.error());
      }
      const auto& tok = vResult.result();
      double v;
      if (tok.is<Token::Number>()) {
        v = tok.get<Token::Number>().value;
      } else if (tok.is<Token::Percentage>()) {
        v = tok.get<Token::Percentage>().value / 100.0;
      } else {
        return unexpectedTokenError(functionName, tok);
      }
      comps[i] = v;
    }

    double alpha = 1.0;
    if (!params.isEOF()) {
      if (auto error = params.requireSlash()) {
        return std::move(error.value());
      }
      auto alphaResult = parseAlpha(params);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result() / 255.0;
    }

    if (auto error = params.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = spaceId.value();
    value.c1 = comps[0];
    value.c2 = comps[1];
    value.c3 = comps[2];
    value.alpha = static_cast<uint8_t>(alpha * 255.0);

    return Color(value);
  }

  ParseResult<Color> parseDeviceCmyk(const RcString& functionName,
                                     std::span<const css::ComponentValue> components) {
    auto trimmed = trimWhitespace(components);
    if (trimmed.empty()) {
      ParseError err;
      err.reason = "Unexpected EOF when parsing function '" + functionName + "'";
      return err;
    }

    size_t index = 0;
    FileOffset lastOffset = trimmed.front().sourceOffset();

    auto nextToken = [&](bool eofIsError) -> ParseResult<css::Token> {
      while (index < trimmed.size()) {
        const auto& component = trimmed[index++];
        if (!component.is<Token>()) {
          ParseError err;
          err.reason =
              "Unexpected token when parsing function '" + functionName + "'";
          err.location = component.sourceOffset();
          return err;
        }

        const auto& token = component.get<Token>();
        lastOffset = token.offset();
        if (token.is<Token::Whitespace>()) {
          continue;
        }
        return token;
      }

      if (!eofIsError) {
        ParseError err;
        err.reason = "Additional tokens when parsing function '" + functionName + "'";
        err.location = lastOffset;
        return err;
      }

      ParseError err;
      err.reason = "Unexpected EOF when parsing function '" + functionName + "'";
      err.location = lastOffset;
      return err;
    };

    auto trySkipComma = [&]() -> ParseResult<bool> {
      size_t probe = index;
      while (probe < trimmed.size()) {
        const auto& component = trimmed[probe];
        if (!component.is<Token>()) {
          ParseError err;
          err.reason =
              "Unexpected token when parsing function '" + functionName + "'";
          err.location = component.sourceOffset();
          return err;
        }

        const auto& token = component.get<Token>();
        if (token.is<Token::Whitespace>()) {
          ++probe;
          continue;
        }

        if (token.is<Token::Comma>()) {
          index = probe + 1;
          lastOffset = token.offset();
          return true;
        }

        return false;
      }

      return false;
    };

    auto requireComma = [&]() -> std::optional<ParseError> {
      auto result = trySkipComma();
      if (result.hasError()) {
        return std::move(result.error());
      }

      if (!result.result()) {
        ParseError err;
        err.reason = "Missing comma when parsing function '" + functionName + "'";
        err.location = lastOffset;
        return err;
      }

      return std::nullopt;
    };

    auto parseComponent = [&](const Token& token) -> ParseResult<double> {
      if (token.is<Token::Number>()) {
        return Clamp(token.get<Token::Number>().value, 0.0, 1.0);
      }

      if (token.is<Token::Percentage>()) {
        return Clamp(token.get<Token::Percentage>().value / 100.0, 0.0, 1.0);
      }

      return unexpectedTokenError(functionName, token);
    };

    auto trySkipSlash = [&]() -> ParseResult<bool> {
      size_t probe = index;
      while (probe < trimmed.size()) {
        const auto& component = trimmed[probe];
        if (!component.is<Token>()) {
          ParseError err;
          err.reason =
              "Unexpected token when parsing function '" + functionName + "'";
          err.location = component.sourceOffset();
          return err;
        }

        const auto& token = component.get<Token>();
        if (token.is<Token::Whitespace>()) {
          ++probe;
          continue;
        }

        if (token.is<Token::Delim>() && token.get<Token::Delim>().value == '/') {
          index = probe + 1;
          lastOffset = token.offset();
          return true;
        }

        return false;
      }

      return false;
    };

    std::array<double, 4> componentsResult{};
    auto firstTokenResult = nextToken(true);
    if (firstTokenResult.hasError()) {
      return std::move(firstTokenResult.error());
    }

    bool requiresCommas = false;
    const auto& firstToken = firstTokenResult.result();
    auto firstComponent = parseComponent(firstToken);
    if (firstComponent.hasError()) {
      return std::move(firstComponent.error());
    }

    componentsResult[0] = firstComponent.result();

    auto commaResult = trySkipComma();
    if (commaResult.hasError()) {
      return std::move(commaResult.error());
    }
    requiresCommas = commaResult.result();

    for (size_t i = 1; i < componentsResult.size(); ++i) {
      if (requiresCommas) {
        if (auto error = requireComma()) {
          return std::move(error.value());
        }
      }

      auto tokenResult = nextToken(true);
      if (tokenResult.hasError()) {
        return std::move(tokenResult.error());
      }

      auto component = parseComponent(tokenResult.result());
      if (component.hasError()) {
        return std::move(component.error());
      }

      componentsResult[i] = component.result();
    }

    auto slashResult = trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    double alpha = 1.0;
    if (slashResult.result()) {
      auto alphaTokenResult = nextToken(true);
      if (alphaTokenResult.hasError()) {
        return std::move(alphaTokenResult.error());
      }

      const auto& alphaToken = alphaTokenResult.result();
      if (alphaToken.is<Token::Number>()) {
        alpha = Clamp(alphaToken.get<Token::Number>().value, 0.0, 1.0);
      } else if (alphaToken.is<Token::Percentage>()) {
        alpha = Clamp(alphaToken.get<Token::Percentage>().value / 100.0, 0.0, 1.0);
      } else {
        ParseError err;
        err.reason = "Unexpected alpha value";
        err.location = alphaToken.offset();
        return err;
      }
    }

    auto trailingComma = trySkipComma();
    if (trailingComma.hasError()) {
      return std::move(trailingComma.error());
    }

    if (trailingComma.result()) {
      auto fallbackSpan = trimmed.subspan(index);
      auto fallback = ColorParser::Parse(fallbackSpan, options_);
      if (fallback.hasError()) {
        return std::move(fallback.error());
      }

      return fallback.result();
    }

    while (index < trimmed.size()) {
      const auto& component = trimmed[index++];
      if (!component.is<Token>()) {
        ParseError err;
        err.reason = "Unexpected token when parsing function '" + functionName + "'";
        err.location = component.sourceOffset();
        return err;
      }

      const auto& token = component.get<Token>();
      if (token.is<Token::Whitespace>()) {
        continue;
      }

      ParseError err;
      err.reason = "Additional tokens when parsing function '" + functionName + "'";
      err.location = token.offset();
      return err;
    }

    const double cyan = componentsResult[0];
    const double magenta = componentsResult[1];
    const double yellow = componentsResult[2];
    const double key = componentsResult[3];

    const uint8_t r = numberToChannel((1.0 - std::min(1.0, cyan + key)) * 255.0);
    const uint8_t g = numberToChannel((1.0 - std::min(1.0, magenta + key)) * 255.0);
    const uint8_t b = numberToChannel((1.0 - std::min(1.0, yellow + key)) * 255.0);

    return Color(RGBA(r, g, b, numberToAlpha(alpha)));
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

  ParseResult<RelativeBaseColor> parseRelativePrefix(
      const RcString& functionName, std::span<const css::ComponentValue> components) {
    auto trimmed = trimWhitespace(components);
    if (trimmed.empty()) {
      ParseError err;
      err.reason = "Unexpected EOF when parsing function '" + functionName + "'";
      return err;
    }

    const FileOffset fromOffset = trimmed.front().sourceOffset();
    trimmed = trimLeadingWhitespace(trimmed.subspan(1));
    if (trimmed.empty()) {
      ParseError err;
      err.reason = "Missing base color for relative color function";
      err.location = fromOffset;
      return err;
    }

    const auto& baseComponent = trimmed.front();
    auto baseColor = ColorParser::Parse(std::span<const ComponentValue>(&baseComponent, 1), options_);
    if (baseColor.hasError()) {
      return std::move(baseColor.error());
    }

    if (std::holds_alternative<Color::CurrentColor>(baseColor.result().value)) {
      ParseError err;
      err.reason = "Relative colors require a concrete base color";
      err.location = baseComponent.sourceOffset();
      return err;
    }

    RelativeBaseColor result;
    result.baseOffset = baseComponent.sourceOffset();
    if (std::holds_alternative<HSLA>(baseColor.result().value)) {
      result.baseHsl = std::get<HSLA>(baseColor.result().value);
    }
    if (std::holds_alternative<ColorSpaceValue>(baseColor.result().value)) {
      result.baseSpace = std::get<ColorSpaceValue>(baseColor.result().value);
    }

    result.rgba = baseColor.result().asRGBA();
    result.remainder = trimLeadingWhitespace(trimmed.subspan(1));
    return result;
  }

  std::optional<ColorSpaceValue> rgbaToColorSpace(const RGBA& rgba, ColorSpaceId id) {
    ColorSpaceValue result;
    result.id = id;
    result.alpha = rgba.a;

    switch (id) {
      case ColorSpaceId::SRGB: {
        result.c1 = static_cast<double>(rgba.r) / 255.0;
        result.c2 = static_cast<double>(rgba.g) / 255.0;
        result.c3 = static_cast<double>(rgba.b) / 255.0;
        return result;
      }
      case ColorSpaceId::SRGBLinear: {
        result.c1 = srgbChannelToLinear(rgba.r);
        result.c2 = srgbChannelToLinear(rgba.g);
        result.c3 = srgbChannelToLinear(rgba.b);
        return result;
      }
      case ColorSpaceId::DisplayP3:
      case ColorSpaceId::A98Rgb:
      case ColorSpaceId::ProPhotoRgb:
      case ColorSpaceId::Rec2020: {
        const Vec3 xyzD65 = srgbToXyzD65(rgba);
        const Matrix3x3* profileMatrix = nullptr;
        double (*encode)(double) = nullptr;
        bool adaptToD50 = false;

        switch (id) {
          case ColorSpaceId::DisplayP3:
            profileMatrix = &kDisplayP3ToXyzD65;
            encode = encodeSRGB;
            break;
          case ColorSpaceId::A98Rgb:
            profileMatrix = &kA98RgbToXyzD65;
            encode = encodeA98;
            break;
          case ColorSpaceId::ProPhotoRgb:
            profileMatrix = &kProPhotoToXyzD50;
            encode = encodeProPhoto;
            adaptToD50 = true;
            break;
          case ColorSpaceId::Rec2020:
            profileMatrix = &kRec2020ToXyzD65;
            encode = encodeRec2020;
            break;
          default: break;
        }

        if (profileMatrix == nullptr || encode == nullptr) {
          return std::nullopt;
        }

        Vec3 xyz = xyzD65;
        if (adaptToD50) {
          xyz = adaptD65ToD50(xyzD65);
        }

        const Matrix3x3 inverse = invert(*profileMatrix);
        const Vec3 linear = multiply(inverse, xyz);
        result.c1 = encode(linear.x);
        result.c2 = encode(linear.y);
        result.c3 = encode(linear.z);
        return result;
      }
      case ColorSpaceId::XyzD65: {
        const Vec3 xyz = srgbToXyzD65(rgba);
        result.c1 = xyz.x;
        result.c2 = xyz.y;
        result.c3 = xyz.z;
        return result;
      }
      case ColorSpaceId::XyzD50: {
        const Vec3 xyz = adaptD65ToD50(srgbToXyzD65(rgba));
        result.c1 = xyz.x;
        result.c2 = xyz.y;
        result.c3 = xyz.z;
        return result;
      }
      case ColorSpaceId::Hwb: {
        const HwbComponents hwb = rgbaToHwb(rgba);
        result.c1 = normalizeAngleDegrees(hwb.h);
        result.c2 = hwb.w;
        result.c3 = hwb.b;
        return result;
      }
      case ColorSpaceId::Lab: {
        const LabComponents lab = rgbaToLab(rgba);
        result.c1 = lab.l;
        result.c2 = lab.a;
        result.c3 = lab.b;
        return result;
      }
      case ColorSpaceId::Lch: {
        const LchComponents lch = rgbaToLch(rgba);
        result.c1 = lch.l;
        result.c2 = lch.c;
        result.c3 = normalizeAngleDegrees(lch.h);
        return result;
      }
      case ColorSpaceId::Oklab: {
        const OklabComponents lab = rgbaToOklab(rgba);
        result.c1 = lab.l;
        result.c2 = lab.a;
        result.c3 = lab.b;
        return result;
      }
      case ColorSpaceId::Oklch: {
        const OklchComponents lch = rgbaToOklch(rgba);
        result.c1 = lch.l;
        result.c2 = lch.c;
        result.c3 = normalizeAngleDegrees(lch.h);
        return result;
      }
    }

    return std::nullopt;
  }

  ParseResult<uint8_t> parseRelativeAlpha(const RcString& functionName,
                                          RelativeComponentStream& stream, double baseAlpha) {
    auto alphaTokenResult = stream.next(true);
    if (alphaTokenResult.hasError()) {
      return std::move(alphaTokenResult.error());
    }

    const auto& alphaToken = alphaTokenResult.result();
    if (alphaToken.is<Token::Number>()) {
      return numberToAlpha(alphaToken.get<Token::Number>().value);
    }
    if (alphaToken.is<Token::Percentage>()) {
      return percentageToChannel(alphaToken.get<Token::Percentage>().value);
    }
    if (alphaToken.is<Token::Ident>()) {
      const RcString ident = alphaToken.get<Token::Ident>().value;
      if (ident.equalsLowercase("a") || ident.equalsLowercase("alpha")) {
        return numberToAlpha(baseAlpha);
      }
    }

    return unexpectedTokenError(functionName, alphaToken);
  }

  ParseResult<std::array<double, 3>> baseColorComponents(const RcString& functionName,
                                                         ColorSpaceId targetSpace,
                                                         const RelativeBaseColor& base) {
    if (base.baseSpace && base.baseSpace->id == targetSpace) {
      return std::array<double, 3>{base.baseSpace->c1, base.baseSpace->c2, base.baseSpace->c3};
    }

    auto converted = rgbaToColorSpace(base.rgba, targetSpace);
    if (!converted) {
      ParseError err;
      err.reason = "Unsupported color space for relative color()";
      err.location = base.baseOffset;
      return err;
    }

    return std::array<double, 3>{converted->c1, converted->c2, converted->c3};
  }

  ParseResult<Color> parseRelativeColorFunction(const RcString& functionName,
                                                std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    RelativeComponentStream stream(functionName, base.result().remainder);

    auto spaceToken = stream.next(true);
    if (spaceToken.hasError()) {
      return std::move(spaceToken.error());
    }

    if (!spaceToken.result().is<Token::Ident>()) {
      return unexpectedTokenError(functionName, spaceToken.result());
    }

    std::string space = spaceToken.result().get<Token::Ident>().value.str();
    std::transform(space.begin(), space.end(), space.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto spaceId = resolveColorSpace(space);
    if (!spaceId) {
      ParseError err;
      err.reason = "Unsupported color space '" + space + "'";
      err.location = spaceToken.result().offset();
      return err;
    }

    auto baseComponents = baseColorComponents(functionName, spaceId.value(), base.result());
    if (baseComponents.hasError()) {
      return std::move(baseComponents.error());
    }

    const auto valueFromIdent = [&](const RcString& ident) -> std::optional<double> {
      if (ident.equalsLowercase("r") || ident.equalsLowercase("red")) {
        if (spaceId == ColorSpaceId::SRGB || spaceId == ColorSpaceId::SRGBLinear ||
            spaceId == ColorSpaceId::DisplayP3 || spaceId == ColorSpaceId::A98Rgb ||
            spaceId == ColorSpaceId::ProPhotoRgb || spaceId == ColorSpaceId::Rec2020) {
          return baseComponents.result()[0];
        }
      }

      if (ident.equalsLowercase("g") || ident.equalsLowercase("green")) {
        if (spaceId == ColorSpaceId::SRGB || spaceId == ColorSpaceId::SRGBLinear ||
            spaceId == ColorSpaceId::DisplayP3 || spaceId == ColorSpaceId::A98Rgb ||
            spaceId == ColorSpaceId::ProPhotoRgb || spaceId == ColorSpaceId::Rec2020) {
          return baseComponents.result()[1];
        }
      }

      if (ident.equalsLowercase("b") || ident.equalsLowercase("blue")) {
        if (spaceId == ColorSpaceId::SRGB || spaceId == ColorSpaceId::SRGBLinear ||
            spaceId == ColorSpaceId::DisplayP3 || spaceId == ColorSpaceId::A98Rgb ||
            spaceId == ColorSpaceId::ProPhotoRgb || spaceId == ColorSpaceId::Rec2020) {
          return baseComponents.result()[2];
        }
      }

      if (ident.equalsLowercase("x") &&
          (spaceId == ColorSpaceId::XyzD65 || spaceId == ColorSpaceId::XyzD50)) {
        return baseComponents.result()[0];
      }
      if (ident.equalsLowercase("y") &&
          (spaceId == ColorSpaceId::XyzD65 || spaceId == ColorSpaceId::XyzD50)) {
        return baseComponents.result()[1];
      }
      if (ident.equalsLowercase("z") &&
          (spaceId == ColorSpaceId::XyzD65 || spaceId == ColorSpaceId::XyzD50)) {
        return baseComponents.result()[2];
      }

      if (ident.equalsLowercase("l")) {
        if (spaceId == ColorSpaceId::Lab || spaceId == ColorSpaceId::Lch ||
            spaceId == ColorSpaceId::Oklab || spaceId == ColorSpaceId::Oklch) {
          return baseComponents.result()[0];
        }
      }
      if (ident.equalsLowercase("a") &&
          (spaceId == ColorSpaceId::Lab || spaceId == ColorSpaceId::Oklab)) {
        return baseComponents.result()[1];
      }
      if (ident.equalsLowercase("b") &&
          (spaceId == ColorSpaceId::Lab || spaceId == ColorSpaceId::Oklab)) {
        return baseComponents.result()[2];
      }

      if (ident.equalsLowercase("c") &&
          (spaceId == ColorSpaceId::Lch || spaceId == ColorSpaceId::Oklch)) {
        return baseComponents.result()[1];
      }
      if (ident.equalsLowercase("h") &&
          (spaceId == ColorSpaceId::Lch || spaceId == ColorSpaceId::Oklch)) {
        return baseComponents.result()[2];
      }

      if (ident.equalsLowercase("h") && spaceId == ColorSpaceId::Hwb) {
        return baseComponents.result()[0];
      }
      if (ident.equalsLowercase("w") && spaceId == ColorSpaceId::Hwb) {
        return baseComponents.result()[1];
      }
      if (ident.equalsLowercase("b") && spaceId == ColorSpaceId::Hwb) {
        return baseComponents.result()[2];
      }

      return std::nullopt;
    };

    double componentsResult[3];
    for (double& component : componentsResult) {
      auto tokenResult = stream.next(true);
      if (tokenResult.hasError()) {
        return std::move(tokenResult.error());
      }

      const auto& token = tokenResult.result();
      if (token.is<Token::Number>()) {
        component = token.get<Token::Number>().value;
        continue;
      }
      if (token.is<Token::Percentage>()) {
        component = token.get<Token::Percentage>().value / 100.0;
        continue;
      }
      if (token.is<Token::Ident>()) {
        const RcString ident = token.get<Token::Ident>().value;
        auto maybeValue = valueFromIdent(ident);
        if (maybeValue) {
          component = maybeValue.value();
          continue;
        }
      }

      return unexpectedTokenError(functionName, token);
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    const double baseAlpha = base.result().baseSpace ?
                                 static_cast<double>(base.result().baseSpace->alpha) / 255.0 :
                                 static_cast<double>(base.result().rgba.a) / 255.0;
    uint8_t alpha = numberToAlpha(baseAlpha);
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream, baseAlpha);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = spaceId.value();
    value.c1 = componentsResult[0];
    value.c2 = componentsResult[1];
    value.c3 = componentsResult[2];
    value.alpha = alpha;
    return Color(value);
  }

  ParseResult<double> parseRelativeHueToken(const RcString& functionName, const Token& token) {
    if (token.is<Token::Number>()) {
      return token.get<Token::Number>().value;
    }

    if (token.is<Token::Dimension>()) {
      const auto& dim = token.get<Token::Dimension>();
      if (dim.suffixString.equalsLowercase("deg")) {
        return dim.value;
      }
      if (dim.suffixString.equalsLowercase("grad")) {
        return dim.value / 400.0 * 360.0;
      }
      if (dim.suffixString.equalsLowercase("rad")) {
        return dim.value * MathConstants<double>::kRadToDeg;
      }
      if (dim.suffixString.equalsLowercase("turn")) {
        return dim.value * 360.0;
      }
    }

    return unexpectedTokenError(functionName, token);
  }

  ParseResult<Color> parseRelativeRgb(const RcString& functionName,
                                      std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    RelativeComponentStream stream(functionName, base.result().remainder);
    double channels[3];
    for (int i = 0; i < 3; ++i) {
      auto tokenResult = stream.next(true);
      if (tokenResult.hasError()) {
        return std::move(tokenResult.error());
      }

      const auto& token = tokenResult.result();
      double value;
      if (token.is<Token::Number>()) {
        value = Clamp(token.get<Token::Number>().value, 0.0, 255.0);
      } else if (token.is<Token::Percentage>()) {
        value = Clamp(token.get<Token::Percentage>().value * 2.55, 0.0, 255.0);
      } else if (token.is<Token::Ident>()) {
        const RcString ident = token.get<Token::Ident>().value;
        if (ident.equalsLowercase("r") || ident.equalsLowercase("red")) {
          value = base.result().rgba.r;
        } else if (ident.equalsLowercase("g") || ident.equalsLowercase("green")) {
          value = base.result().rgba.g;
        } else if (ident.equalsLowercase("b") || ident.equalsLowercase("blue")) {
          value = base.result().rgba.b;
        } else {
          return unexpectedTokenError(functionName, token);
        }
      } else {
        return unexpectedTokenError(functionName, token);
      }

      channels[i] = value;
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    uint8_t alpha = base.result().rgba.a;
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream,
                                            static_cast<double>(base.result().rgba.a) / 255.0);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    return Color(RGBA(numberToChannel(channels[0]), numberToChannel(channels[1]),
                      numberToChannel(channels[2]), alpha));
  }

  ParseResult<Color> parseRelativeHsl(const RcString& functionName,
                                      std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    HslComponents baseHsl = rgbaToHsl(base.result().rgba);
    if (base.result().baseHsl) {
      baseHsl.h = base.result().baseHsl->hDeg;
      baseHsl.s = base.result().baseHsl->s;
      baseHsl.l = base.result().baseHsl->l;
      baseHsl.alpha = static_cast<double>(base.result().baseHsl->a) / 255.0;
    }
    RelativeComponentStream stream(functionName, base.result().remainder);

    auto hueToken = stream.next(true);
    if (hueToken.hasError()) {
      return std::move(hueToken.error());
    }

    double h;
    if (hueToken.result().is<Token::Ident>() &&
        hueToken.result().get<Token::Ident>().value.equalsLowercase("h")) {
      h = baseHsl.h;
    } else {
      auto hueValue = parseRelativeHueToken(functionName, hueToken.result());
      if (hueValue.hasError()) {
        return std::move(hueValue.error());
      }
      h = hueValue.result();
    }

    auto parsePercent = [&](const Token& token, double baseValue,
                           std::string_view ident) -> ParseResult<double> {
      if (token.is<Token::Percentage>()) {
        return Clamp(token.get<Token::Percentage>().value / 100.0, 0.0, 1.0);
      }
      if (token.is<Token::Number>()) {
        return Clamp(token.get<Token::Number>().value, 0.0, 1.0);
      }
      if (token.is<Token::Ident>() && token.get<Token::Ident>().value.equalsLowercase(ident)) {
        return Clamp(baseValue, 0.0, 1.0);
      }
      return unexpectedTokenError(functionName, token);
    };

    auto sToken = stream.next(true);
    if (sToken.hasError()) {
      return std::move(sToken.error());
    }
    auto s = parsePercent(sToken.result(), baseHsl.s, "s");
    if (s.hasError()) {
      return std::move(s.error());
    }

    auto lToken = stream.next(true);
    if (lToken.hasError()) {
      return std::move(lToken.error());
    }
    auto l = parsePercent(lToken.result(), baseHsl.l, "l");
    if (l.hasError()) {
      return std::move(l.error());
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    uint8_t alpha = numberToAlpha(baseHsl.alpha);
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream, baseHsl.alpha);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    HSLA hsl = HSLA::HSL(static_cast<float>(normalizeAngleDegrees(h)),
                         static_cast<float>(s.result()), static_cast<float>(l.result()));
    hsl.a = alpha;
    return Color(hsl);
  }

  ParseResult<Color> parseRelativeHwb(const RcString& functionName,
                                      std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    HwbComponents baseHwb = rgbaToHwb(base.result().rgba);
    if (base.result().baseSpace && base.result().baseSpace->id == ColorSpaceId::Hwb) {
      baseHwb.h = base.result().baseSpace->c1;
      baseHwb.w = base.result().baseSpace->c2;
      baseHwb.b = base.result().baseSpace->c3;
      baseHwb.alpha = static_cast<double>(base.result().baseSpace->alpha) / 255.0;
    }
    RelativeComponentStream stream(functionName, base.result().remainder);

    auto hueToken = stream.next(true);
    if (hueToken.hasError()) {
      return std::move(hueToken.error());
    }

    double h;
    if (hueToken.result().is<Token::Ident>() &&
        hueToken.result().get<Token::Ident>().value.equalsLowercase("h")) {
      h = baseHwb.h;
    } else {
      auto hueValue = parseRelativeHueToken(functionName, hueToken.result());
      if (hueValue.hasError()) {
        return std::move(hueValue.error());
      }
      h = hueValue.result();
    }

    auto parseWb = [&](const Token& token, double baseValue,
                      std::string_view ident) -> ParseResult<double> {
      if (token.is<Token::Percentage>()) {
        return Clamp(token.get<Token::Percentage>().value / 100.0, 0.0, 1.0);
      }
      if (token.is<Token::Number>()) {
        return Clamp(token.get<Token::Number>().value, 0.0, 1.0);
      }
      if (token.is<Token::Ident>() && token.get<Token::Ident>().value.equalsLowercase(ident)) {
        return Clamp(baseValue, 0.0, 1.0);
      }
      return unexpectedTokenError(functionName, token);
    };

    auto wToken = stream.next(true);
    if (wToken.hasError()) {
      return std::move(wToken.error());
    }
    auto w = parseWb(wToken.result(), baseHwb.w, "w");
    if (w.hasError()) {
      return std::move(w.error());
    }

    auto bToken = stream.next(true);
    if (bToken.hasError()) {
      return std::move(bToken.error());
    }
    auto b = parseWb(bToken.result(), baseHwb.b, "b");
    if (b.hasError()) {
      return std::move(b.error());
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    uint8_t alpha = numberToAlpha(baseHwb.alpha);
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream, baseHwb.alpha);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = ColorSpaceId::Hwb;
    value.c1 = normalizeAngleDegrees(h);
    value.c2 = w.result();
    value.c3 = b.result();
    value.alpha = alpha;
    return Color(value);
  }

  ParseResult<Color> parseRelativeLab(const RcString& functionName,
                                      std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    LabComponents baseLab = rgbaToLab(base.result().rgba);
    if (base.result().baseSpace && base.result().baseSpace->id == ColorSpaceId::Lab) {
      baseLab.l = base.result().baseSpace->c1;
      baseLab.a = base.result().baseSpace->c2;
      baseLab.b = base.result().baseSpace->c3;
      baseLab.alpha = static_cast<double>(base.result().baseSpace->alpha) / 255.0;
    }
    RelativeComponentStream stream(functionName, base.result().remainder);

    auto parseLabComponent = [&](const Token& token, double baseValue, double percentageScale,
                                std::string_view ident) -> ParseResult<double> {
      if (token.is<Token::Percentage>()) {
        return Clamp(token.get<Token::Percentage>().value / 100.0 * percentageScale,
                     -percentageScale, percentageScale);
      }
      if (token.is<Token::Number>()) {
        return token.get<Token::Number>().value;
      }
      if (token.is<Token::Ident>() && token.get<Token::Ident>().value.equalsLowercase(ident)) {
        return baseValue;
      }
      return unexpectedTokenError(functionName, token);
    };

    auto lToken = stream.next(true);
    if (lToken.hasError()) {
      return std::move(lToken.error());
    }
    double l;
    if (lToken.result().is<Token::Percentage>()) {
      l = Clamp(lToken.result().get<Token::Percentage>().value, 0.0, 100.0);
    } else if (lToken.result().is<Token::Number>()) {
      l = Clamp(lToken.result().get<Token::Number>().value, 0.0, 100.0);
    } else if (lToken.result().is<Token::Ident>() &&
               lToken.result().get<Token::Ident>().value.equalsLowercase("l")) {
      l = Clamp(baseLab.l, 0.0, 100.0);
    } else {
      return unexpectedTokenError(functionName, lToken.result());
    }

    auto aToken = stream.next(true);
    if (aToken.hasError()) {
      return std::move(aToken.error());
    }
    auto a = parseLabComponent(aToken.result(), baseLab.a, 125.0, "a");
    if (a.hasError()) {
      return std::move(a.error());
    }

    auto bToken = stream.next(true);
    if (bToken.hasError()) {
      return std::move(bToken.error());
    }
    auto b = parseLabComponent(bToken.result(), baseLab.b, 125.0, "b");
    if (b.hasError()) {
      return std::move(b.error());
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    uint8_t alpha = numberToAlpha(baseLab.alpha);
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream, baseLab.alpha);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = ColorSpaceId::Lab;
    value.c1 = l;
    value.c2 = Clamp(a.result(), -125.0, 125.0);
    value.c3 = Clamp(b.result(), -125.0, 125.0);
    value.alpha = alpha;
    return Color(value);
  }

  ParseResult<Color> parseRelativeLch(const RcString& functionName,
                                      std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    LchComponents baseLch = rgbaToLch(base.result().rgba);
    if (base.result().baseSpace && base.result().baseSpace->id == ColorSpaceId::Lch) {
      baseLch.l = base.result().baseSpace->c1;
      baseLch.c = base.result().baseSpace->c2;
      baseLch.h = base.result().baseSpace->c3;
      baseLch.alpha = static_cast<double>(base.result().baseSpace->alpha) / 255.0;
    }
    RelativeComponentStream stream(functionName, base.result().remainder);

    auto parseLComponent = [&](const Token& token) -> ParseResult<double> {
      if (token.is<Token::Percentage>()) {
        return Clamp(token.get<Token::Percentage>().value, 0.0, 100.0);
      }
      if (token.is<Token::Number>()) {
        return Clamp(token.get<Token::Number>().value, 0.0, 100.0);
      }
      if (token.is<Token::Ident>() && token.get<Token::Ident>().value.equalsLowercase("l")) {
        return Clamp(baseLch.l, 0.0, 100.0);
      }
      return unexpectedTokenError(functionName, token);
    };

    auto lToken = stream.next(true);
    if (lToken.hasError()) {
      return std::move(lToken.error());
    }
    auto l = parseLComponent(lToken.result());
    if (l.hasError()) {
      return std::move(l.error());
    }

    auto parseCComponent = [&](const Token& token) -> ParseResult<double> {
      if (token.is<Token::Percentage>()) {
        return Clamp(token.get<Token::Percentage>().value / 100.0 * 150.0, 0.0, 150.0);
      }
      if (token.is<Token::Number>()) {
        return std::max(0.0, token.get<Token::Number>().value);
      }
      if (token.is<Token::Ident>() && token.get<Token::Ident>().value.equalsLowercase("c")) {
        return baseLch.c;
      }
      return unexpectedTokenError(functionName, token);
    };

    auto cToken = stream.next(true);
    if (cToken.hasError()) {
      return std::move(cToken.error());
    }
    auto c = parseCComponent(cToken.result());
    if (c.hasError()) {
      return std::move(c.error());
    }

    auto hToken = stream.next(true);
    if (hToken.hasError()) {
      return std::move(hToken.error());
    }

    double h;
    if (hToken.result().is<Token::Ident>() &&
        hToken.result().get<Token::Ident>().value.equalsLowercase("h")) {
      h = baseLch.h;
    } else {
      auto hueValue = parseRelativeHueToken(functionName, hToken.result());
      if (hueValue.hasError()) {
        return std::move(hueValue.error());
      }
      h = hueValue.result();
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    uint8_t alpha = numberToAlpha(baseLch.alpha);
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream, baseLch.alpha);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = ColorSpaceId::Lch;
    value.c1 = l.result();
    value.c2 = c.result();
    value.c3 = normalizeAngleDegrees(h);
    value.alpha = alpha;
    return Color(value);
  }

  ParseResult<Color> parseRelativeOklab(const RcString& functionName,
                                        std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    OklabComponents baseLab = rgbaToOklab(base.result().rgba);
    if (base.result().baseSpace && base.result().baseSpace->id == ColorSpaceId::Oklab) {
      baseLab.l = base.result().baseSpace->c1;
      baseLab.a = base.result().baseSpace->c2;
      baseLab.b = base.result().baseSpace->c3;
      baseLab.alpha = static_cast<double>(base.result().baseSpace->alpha) / 255.0;
    }
    RelativeComponentStream stream(functionName, base.result().remainder);

    auto parseOkComponent = [&](const Token& token, double baseValue, std::string_view ident,
                               bool clampComponent) -> ParseResult<double> {
      if (token.is<Token::Percentage>()) {
        double scaled = token.get<Token::Percentage>().value / 100.0 * 0.4;
        scaled = clampComponent ? Clamp(scaled, -0.4, 0.4) : scaled;
        return scaled;
      }
      if (token.is<Token::Number>()) {
        return clampComponent ? Clamp(token.get<Token::Number>().value, -0.4, 0.4)
                              : token.get<Token::Number>().value;
      }
      if (token.is<Token::Ident>() && token.get<Token::Ident>().value.equalsLowercase(ident)) {
        return baseValue;
      }
      return unexpectedTokenError(functionName, token);
    };

    auto lToken = stream.next(true);
    if (lToken.hasError()) {
      return std::move(lToken.error());
    }

    double l;
    if (lToken.result().is<Token::Percentage>()) {
      l = Clamp(lToken.result().get<Token::Percentage>().value / 100.0, 0.0, 1.0);
    } else if (lToken.result().is<Token::Number>()) {
      l = Clamp(lToken.result().get<Token::Number>().value, 0.0, 1.0);
    } else if (lToken.result().is<Token::Ident>() &&
               lToken.result().get<Token::Ident>().value.equalsLowercase("l")) {
      l = Clamp(baseLab.l, 0.0, 1.0);
    } else {
      return unexpectedTokenError(functionName, lToken.result());
    }

    auto aToken = stream.next(true);
    if (aToken.hasError()) {
      return std::move(aToken.error());
    }
    auto a = parseOkComponent(aToken.result(), baseLab.a, "a", true);
    if (a.hasError()) {
      return std::move(a.error());
    }

    auto bToken = stream.next(true);
    if (bToken.hasError()) {
      return std::move(bToken.error());
    }
    auto b = parseOkComponent(bToken.result(), baseLab.b, "b", true);
    if (b.hasError()) {
      return std::move(b.error());
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    uint8_t alpha = numberToAlpha(baseLab.alpha);
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream, baseLab.alpha);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = ColorSpaceId::Oklab;
    value.c1 = l;
    value.c2 = Clamp(a.result(), -0.4, 0.4);
    value.c3 = Clamp(b.result(), -0.4, 0.4);
    value.alpha = alpha;
    return Color(value);
  }

  ParseResult<Color> parseRelativeOklch(const RcString& functionName,
                                        std::span<const css::ComponentValue> components) {
    auto base = parseRelativePrefix(functionName, components);
    if (base.hasError()) {
      return std::move(base.error());
    }

    OklchComponents baseLch = rgbaToOklch(base.result().rgba);
    if (base.result().baseSpace && base.result().baseSpace->id == ColorSpaceId::Oklch) {
      baseLch.l = base.result().baseSpace->c1;
      baseLch.c = base.result().baseSpace->c2;
      baseLch.h = base.result().baseSpace->c3;
      baseLch.alpha = static_cast<double>(base.result().baseSpace->alpha) / 255.0;
    }
    RelativeComponentStream stream(functionName, base.result().remainder);

    auto lToken = stream.next(true);
    if (lToken.hasError()) {
      return std::move(lToken.error());
    }

    double l;
    if (lToken.result().is<Token::Percentage>()) {
      l = Clamp(lToken.result().get<Token::Percentage>().value / 100.0, 0.0, 1.0);
    } else if (lToken.result().is<Token::Number>()) {
      l = Clamp(lToken.result().get<Token::Number>().value, 0.0, 1.0);
    } else if (lToken.result().is<Token::Ident>() &&
               lToken.result().get<Token::Ident>().value.equalsLowercase("l")) {
      l = Clamp(baseLch.l, 0.0, 1.0);
    } else {
      return unexpectedTokenError(functionName, lToken.result());
    }

    auto cToken = stream.next(true);
    if (cToken.hasError()) {
      return std::move(cToken.error());
    }

    double c;
    if (cToken.result().is<Token::Percentage>()) {
      c = Clamp(cToken.result().get<Token::Percentage>().value / 100.0 * 0.4, 0.0, 0.4);
    } else if (cToken.result().is<Token::Number>()) {
      c = std::max(0.0, cToken.result().get<Token::Number>().value);
    } else if (cToken.result().is<Token::Ident>() &&
               cToken.result().get<Token::Ident>().value.equalsLowercase("c")) {
      c = baseLch.c;
    } else {
      return unexpectedTokenError(functionName, cToken.result());
    }

    auto hToken = stream.next(true);
    if (hToken.hasError()) {
      return std::move(hToken.error());
    }

    double h;
    if (hToken.result().is<Token::Ident>() &&
        hToken.result().get<Token::Ident>().value.equalsLowercase("h")) {
      h = baseLch.h;
    } else {
      auto hueValue = parseRelativeHueToken(functionName, hToken.result());
      if (hueValue.hasError()) {
        return std::move(hueValue.error());
      }
      h = hueValue.result();
    }

    auto slashResult = stream.trySkipSlash();
    if (slashResult.hasError()) {
      return std::move(slashResult.error());
    }

    uint8_t alpha = numberToAlpha(baseLch.alpha);
    if (slashResult.result()) {
      auto alphaResult = parseRelativeAlpha(functionName, stream, baseLch.alpha);
      if (alphaResult.hasError()) {
        return std::move(alphaResult.error());
      }
      alpha = alphaResult.result();
    }

    if (auto error = stream.requireEOF()) {
      return std::move(error.value());
    }

    ColorSpaceValue value;
    value.id = ColorSpaceId::Oklch;
    value.c1 = l;
    value.c2 = c;
    value.c3 = normalizeAngleDegrees(h);
    value.alpha = alpha;
    return Color(value);
  }

private:
  ParseError unexpectedTokenError(const RcString& functionName, const Token& token) {
    ParseError err;
    err.reason = "Unexpected token when parsing function '" + functionName + "'";
    err.location = token.offset();
    return err;
  }

  std::optional<ColorSpaceId> resolveColorSpace(const std::string& name) const {
    if (options_.profileRegistry) {
      if (auto fromProfile = options_.profileRegistry->resolve(name)) {
        return fromProfile;
      }
    }

    return ColorSpaceIdFromString(name);
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

  std::span<const css::ComponentValue> components_;
  ColorParser::Options options_;
};

}  // namespace

ParseResult<Color> ColorParser::Parse(std::span<const css::ComponentValue> components,
                                      const ColorParser::Options& options) {
  ColorParserImpl parser(components, options);
  return parser.parseColor();
}

ParseResult<Color> ColorParser::ParseString(std::string_view str,
                                            const ColorParser::Options& options) {
  details::Tokenizer tokenizer(str);
  const std::vector<ComponentValue> componentValues =
      details::parseListOfComponentValues(tokenizer);
  ColorParserImpl parser(componentValues, options);
  return parser.parseColor();
}

}  // namespace donner::css::parser
