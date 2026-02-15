#include "donner/css/Color.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

#include "donner/base/CompileTimeMap.h"
#include "donner/base/MathUtils.h"

namespace donner::css {

namespace {

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Matrix3x3 {
  double m[3][3] = {};
};

uint8_t numberToChannel(double number) {
  return static_cast<uint8_t>(Clamp(Round(number), 0.0, 255.0));
}

uint8_t linearToSRGB(double value) {
  double v = value <= 0.0031308 ? 12.92 * value : 1.055 * pow(value, 1.0 / 2.4) - 0.055;

  v = Clamp(v, 0.0, 1.0);
  return numberToChannel(v * 255.0);
}

double normalizeAngleDegrees(double angleDegrees) {
  return angleDegrees - std::floor(angleDegrees / 360.0) * 360.0;
}

Vec3 multiply(const Matrix3x3& matrix, const Vec3& v) {
  return {.x = matrix.m[0][0] * v.x + matrix.m[0][1] * v.y + matrix.m[0][2] * v.z,
          .y = matrix.m[1][0] * v.x + matrix.m[1][1] * v.y + matrix.m[1][2] * v.z,
          .z = matrix.m[2][0] * v.x + matrix.m[2][1] * v.y + matrix.m[2][2] * v.z};
}

double decodeSRGB(double value) {
  return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

double decodeGammaSigned(double value, double gamma) {
  const double clamped = Clamp(value, -1.0, 1.0);
  const double magnitude = pow(std::abs(clamped), gamma);
  return std::copysign(magnitude, clamped);
}

double decodeA98(double value) {
  return decodeGammaSigned(value, 563.0 / 256.0);
}

double decodeProPhoto(double value) {
  if (value < 16.0 / 512.0) {
    return value / 16.0;
  }
  return pow(value, 1.8);
}

double decodeRec2020(double value) {
  if (value < 0.08145) {
    return value / 4.5;
  }
  return pow((value + 0.099) / 1.099, 1.0 / 0.45);
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

RGBA hwbToRgb(double hue, double white, double black, uint8_t alpha) {
  if (white + black >= 1) {
    const uint8_t gray = numberToChannel(white / (white + black));
    return RGBA(gray, gray, gray, alpha);
  }

  HSLA hsl = HSLA::HSL(static_cast<float>(hue), 1.0f, 0.5f);
  float* hslComponents[3] = {&hsl.hDeg, &hsl.s, &hsl.l};

  for (int i = 0; i < 3; i++) {
    *hslComponents[i] *= static_cast<float>(1 - white - black);
    *hslComponents[i] += static_cast<float>(white);
  }

  RGBA rgba = hsl.toRGBA();
  rgba.a = alpha;
  return rgba;
}

void lchToLab(double l, double c, double hDeg, double& a, double& b) {
  const double hRad = hDeg * MathConstants<double>::kDegToRad;
  a = c * cos(hRad);
  b = c * sin(hRad);
}

void labToXYZ(double l, double a, double b, double& x, double& y, double& z) {
  const double kappa = 24389.0 / 27.0;
  const double epsilon = 216.0 / 24389.0;

  const double fy = (l + 16.0) / 116.0;
  const double fx = fy + (a / 500.0);
  const double fz = fy - (b / 200.0);

  const double fx3 = fx * fx * fx;
  const double fz3 = fz * fz * fz;

  const double xr = fx3 > epsilon ? fx3 : (116.0 * fx - 16.0) / kappa;
  const double yr = l > (kappa * epsilon) ? pow((l + 16.0) / 116.0, 3) : l / kappa;
  const double zr = fz3 > epsilon ? fz3 : (116.0 * fz - 16.0) / kappa;

  const double xn = 0.96422;
  const double yn = 1.0;
  const double zn = 0.82521;

  x = xr * xn;
  y = yr * yn;
  z = zr * zn;
}

void adaptD50toD65(double xD50, double yD50, double zD50, double& xD65, double& yD65,
                   double& zD65) {
  const double m[3][3] = {{0.9554734527042182, -0.0230985368742614, 0.0632593086610217},
                          {-0.0283697069632081, 1.0099954580058226, 0.0210413989669430},
                          {0.0123140016883199, -0.0205076964334771, 1.3303659908427779}};

  xD65 = m[0][0] * xD50 + m[0][1] * yD50 + m[0][2] * zD50;
  yD65 = m[1][0] * xD50 + m[1][1] * yD50 + m[1][2] * zD50;
  zD65 = m[2][0] * xD50 + m[2][1] * yD50 + m[2][2] * zD50;
}

Vec3 adaptD50toD65Vec(const Vec3& xyzD50) {
  Vec3 xyzD65;
  adaptD50toD65(xyzD50.x, xyzD50.y, xyzD50.z, xyzD65.x, xyzD65.y, xyzD65.z);
  return xyzD65;
}

void xyzToLinearRGB(double x, double y, double z, double& r, double& g, double& b) {
  r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
  g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
  b = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
}

RGBA xyzD65ToRGBA(const Vec3& xyz, uint8_t alpha) {
  double r_lin;
  double g_lin;
  double b_lin;
  xyzToLinearRGB(xyz.x, xyz.y, xyz.z, r_lin, g_lin, b_lin);

  const uint8_t R = linearToSRGB(r_lin);
  const uint8_t G = linearToSRGB(g_lin);
  const uint8_t B = linearToSRGB(b_lin);

  return RGBA(R, G, B, alpha);
}

RGBA rgbProfileToSRGB(const ColorSpaceValue& value, const Matrix3x3& toXyz,
                      double (*decode)(double), bool sourceWhitePointIsD50) {
  const Vec3 encoded{value.c1, value.c2, value.c3};
  const Vec3 linear{decode(encoded.x), decode(encoded.y), decode(encoded.z)};
  const Vec3 xyz = multiply(toXyz, linear);
  const Vec3 xyzD65 = sourceWhitePointIsD50 ? adaptD50toD65Vec(xyz) : xyz;

  return xyzD65ToRGBA(xyzD65, value.alpha);
}

RGBA labToSRGB(double l, double a, double b, uint8_t alpha) {
  double xD50;
  double yD50;
  double zD50;
  labToXYZ(l, a, b, xD50, yD50, zD50);

  double xD65;
  double yD65;
  double zD65;
  adaptD50toD65(xD50, yD50, zD50, xD65, yD65, zD65);

  double rLin;
  double gLin;
  double bLin;
  xyzToLinearRGB(xD65, yD65, zD65, rLin, gLin, bLin);

  const uint8_t red = linearToSRGB(rLin);
  const uint8_t green = linearToSRGB(gLin);
  const uint8_t blue = linearToSRGB(bLin);

  return RGBA(red, green, blue, alpha);
}

void oklchToOklab(double l, double c, double hDeg, double& a, double& b) {
  const double hRad = hDeg * MathConstants<double>::kDegToRad;
  a = c * cos(hRad);
  b = c * sin(hRad);
}

RGBA oklabToSRGB(double l, double a, double b, uint8_t alpha) {
  double l_ = l + 0.3963377774 * a + 0.2158037573 * b;
  double m = l - 0.1055613458 * a - 0.0638541728 * b;
  double s = l - 0.0894841775 * a - 1.2914855480 * b;

  l_ = l_ * l_ * l_;
  m = m * m * m;
  s = s * s * s;

  double rLin = +4.0767416621 * l_ - 3.3077115913 * m + 0.2309699292 * s;
  double gLin = -1.2684380046 * l_ + 2.6097574011 * m - 0.3413193965 * s;
  double bLin = -0.0041960863 * l_ - 0.7034186147 * m + 1.7076147010 * s;

  const uint8_t red = linearToSRGB(rLin);
  const uint8_t green = linearToSRGB(gLin);
  const uint8_t blue = linearToSRGB(bLin);
  return RGBA(red, green, blue, alpha);
}

RGBA colorSpaceToRGBA(const ColorSpaceValue& value) {
  switch (value.id) {
    case ColorSpaceId::SRGB:
      return RGBA(numberToChannel(value.c1 * 255.0), numberToChannel(value.c2 * 255.0),
                  numberToChannel(value.c3 * 255.0), value.alpha);
    case ColorSpaceId::SRGBLinear:
      return RGBA(linearToSRGB(value.c1), linearToSRGB(value.c2), linearToSRGB(value.c3),
                  value.alpha);
    case ColorSpaceId::DisplayP3:
      return rgbProfileToSRGB(value, kDisplayP3ToXyzD65, decodeSRGB, false);
    case ColorSpaceId::A98Rgb: return rgbProfileToSRGB(value, kA98RgbToXyzD65, decodeA98, false);
    case ColorSpaceId::ProPhotoRgb:
      return rgbProfileToSRGB(value, kProPhotoToXyzD50, decodeProPhoto, true);
    case ColorSpaceId::Rec2020:
      return rgbProfileToSRGB(value, kRec2020ToXyzD65, decodeRec2020, false);
    case ColorSpaceId::XyzD65: return xyzD65ToRGBA({value.c1, value.c2, value.c3}, value.alpha);
    case ColorSpaceId::XyzD50:
      return xyzD65ToRGBA(adaptD50toD65Vec({value.c1, value.c2, value.c3}), value.alpha);
    case ColorSpaceId::Hwb:
      return hwbToRgb(normalizeAngleDegrees(value.c1), Clamp(value.c2, 0.0, 1.0),
                      Clamp(value.c3, 0.0, 1.0), value.alpha);
    case ColorSpaceId::Lab: return labToSRGB(value.c1, value.c2, value.c3, value.alpha);
    case ColorSpaceId::Lch: {
      double a = 0.0;
      double b = 0.0;
      lchToLab(value.c1, value.c2, value.c3, a, b);
      return labToSRGB(value.c1, a, b, value.alpha);
    }
    case ColorSpaceId::Oklab: return oklabToSRGB(value.c1, value.c2, value.c3, value.alpha);
    case ColorSpaceId::Oklch: {
      double a = 0.0;
      double b = 0.0;
      oklchToOklab(value.c1, value.c2, value.c3, a, b);
      return oklabToSRGB(value.c1, a, b, value.alpha);
    }
  }

  UTILS_RELEASE_ASSERT_MSG(false, "Unhandled color space conversion");
  return RGBA();
}

const char* colorSpaceIdToString(ColorSpaceId id) {
  switch (id) {
    case ColorSpaceId::SRGB: return "srgb";
    case ColorSpaceId::SRGBLinear: return "srgb-linear";
    case ColorSpaceId::DisplayP3: return "display-p3";
    case ColorSpaceId::A98Rgb: return "a98-rgb";
    case ColorSpaceId::ProPhotoRgb: return "prophoto-rgb";
    case ColorSpaceId::Rec2020: return "rec2020";
    case ColorSpaceId::XyzD65: return "xyz-d65";
    case ColorSpaceId::XyzD50: return "xyz-d50";
    case ColorSpaceId::Hwb: return "hwb";
    case ColorSpaceId::Lab: return "lab";
    case ColorSpaceId::Lch: return "lch";
    case ColorSpaceId::Oklab: return "oklab";
    case ColorSpaceId::Oklch: return "oklch";
  }

  return "unknown";
}

using namespace string_literals;
using namespace std::string_view_literals;

static constexpr auto kColors =
    makeCompileTimeMap(std::to_array<std::pair<std::string_view, Color>>({
        {"aliceblue"sv, 0xF0F8FF_rgb},
        {"antiquewhite"sv, 0xFAEBD7_rgb},
        {"aqua"sv, 0x00FFFF_rgb},
        {"aquamarine"sv, 0x7FFFD4_rgb},
        {"azure"sv, 0xF0FFFF_rgb},
        {"beige"sv, 0xF5F5DC_rgb},
        {"bisque"sv, 0xFFE4C4_rgb},
        {"black"sv, 0x000000_rgb},
        {"blanchedalmond"sv, 0xFFEBCD_rgb},
        {"blue"sv, 0x0000FF_rgb},
        {"blueviolet"sv, 0x8A2BE2_rgb},
        {"brown"sv, 0xA52A2A_rgb},
        {"burlywood"sv, 0xDEB887_rgb},
        {"cadetblue"sv, 0x5F9EA0_rgb},
        {"chartreuse"sv, 0x7FFF00_rgb},
        {"chocolate"sv, 0xD2691E_rgb},
        {"coral"sv, 0xFF7F50_rgb},
        {"cornflowerblue"sv, 0x6495ED_rgb},
        {"cornsilk"sv, 0xFFF8DC_rgb},
        {"crimson"sv, 0xDC143C_rgb},
        {"cyan"sv, 0x00FFFF_rgb},
        {"darkblue"sv, 0x00008B_rgb},
        {"darkcyan"sv, 0x008B8B_rgb},
        {"darkgoldenrod"sv, 0xB8860B_rgb},
        {"darkgray"sv, 0xA9A9A9_rgb},
        {"darkgreen"sv, 0x006400_rgb},
        {"darkgrey"sv, 0xA9A9A9_rgb},
        {"darkkhaki"sv, 0xBDB76B_rgb},
        {"darkmagenta"sv, 0x8B008B_rgb},
        {"darkolivegreen"sv, 0x556B2F_rgb},
        {"darkorange"sv, 0xFF8C00_rgb},
        {"darkorchid"sv, 0x9932CC_rgb},
        {"darkred"sv, 0x8B0000_rgb},
        {"darksalmon"sv, 0xE9967A_rgb},
        {"darkseagreen"sv, 0x8FBC8F_rgb},
        {"darkslateblue"sv, 0x483D8B_rgb},
        {"darkslategray"sv, 0x2F4F4F_rgb},
        {"darkslategrey"sv, 0x2F4F4F_rgb},
        {"darkturquoise"sv, 0x00CED1_rgb},
        {"darkviolet"sv, 0x9400D3_rgb},
        {"deeppink"sv, 0xFF1493_rgb},
        {"deepskyblue"sv, 0x00BFFF_rgb},
        {"dimgray"sv, 0x696969_rgb},
        {"dimgrey"sv, 0x696969_rgb},
        {"dodgerblue"sv, 0x1E90FF_rgb},
        {"firebrick"sv, 0xB22222_rgb},
        {"floralwhite"sv, 0xFFFAF0_rgb},
        {"forestgreen"sv, 0x228B22_rgb},
        {"fuchsia"sv, 0xFF00FF_rgb},
        {"gainsboro"sv, 0xDCDCDC_rgb},
        {"ghostwhite"sv, 0xF8F8FF_rgb},
        {"gold"sv, 0xFFD700_rgb},
        {"goldenrod"sv, 0xDAA520_rgb},
        {"gray"sv, 0x808080_rgb},
        {"green"sv, 0x008000_rgb},
        {"greenyellow"sv, 0xADFF2F_rgb},
        {"grey"sv, 0x808080_rgb},
        {"honeydew"sv, 0xF0FFF0_rgb},
        {"hotpink"sv, 0xFF69B4_rgb},
        {"indianred"sv, 0xCD5C5C_rgb},
        {"indigo"sv, 0x4B0082_rgb},
        {"ivory"sv, 0xFFFFF0_rgb},
        {"khaki"sv, 0xF0E68C_rgb},
        {"lavender"sv, 0xE6E6FA_rgb},
        {"lavenderblush"sv, 0xFFF0F5_rgb},
        {"lawngreen"sv, 0x7CFC00_rgb},
        {"lemonchiffon"sv, 0xFFFACD_rgb},
        {"lightblue"sv, 0xADD8E6_rgb},
        {"lightcoral"sv, 0xF08080_rgb},
        {"lightcyan"sv, 0xE0FFFF_rgb},
        {"lightgoldenrodyellow"sv, 0xFAFAD2_rgb},
        {"lightgray"sv, 0xD3D3D3_rgb},
        {"lightgreen"sv, 0x90EE90_rgb},
        {"lightgrey"sv, 0xD3D3D3_rgb},
        {"lightpink"sv, 0xFFB6C1_rgb},
        {"lightsalmon"sv, 0xFFA07A_rgb},
        {"lightseagreen"sv, 0x20B2AA_rgb},
        {"lightskyblue"sv, 0x87CEFA_rgb},
        {"lightslategray"sv, 0x778899_rgb},
        {"lightslategrey"sv, 0x778899_rgb},
        {"lightsteelblue"sv, 0xB0C4DE_rgb},
        {"lightyellow"sv, 0xFFFFE0_rgb},
        {"lime"sv, 0x00FF00_rgb},
        {"limegreen"sv, 0x32CD32_rgb},
        {"linen"sv, 0xFAF0E6_rgb},
        {"magenta"sv, 0xFF00FF_rgb},
        {"maroon"sv, 0x800000_rgb},
        {"mediumaquamarine"sv, 0x66CDAA_rgb},
        {"mediumblue"sv, 0x0000CD_rgb},
        {"mediumorchid"sv, 0xBA55D3_rgb},
        {"mediumpurple"sv, 0x9370DB_rgb},
        {"mediumseagreen"sv, 0x3CB371_rgb},
        {"mediumslateblue"sv, 0x7B68EE_rgb},
        {"mediumspringgreen"sv, 0x00FA9A_rgb},
        {"mediumturquoise"sv, 0x48D1CC_rgb},
        {"mediumvioletred"sv, 0xC71585_rgb},
        {"midnightblue"sv, 0x191970_rgb},
        {"mintcream"sv, 0xF5FFFA_rgb},
        {"mistyrose"sv, 0xFFE4E1_rgb},
        {"moccasin"sv, 0xFFE4B5_rgb},
        {"navajowhite"sv, 0xFFDEAD_rgb},
        {"navy"sv, 0x000080_rgb},
        {"oldlace"sv, 0xFDF5E6_rgb},
        {"olive"sv, 0x808000_rgb},
        {"olivedrab"sv, 0x6B8E23_rgb},
        {"orange"sv, 0xFFA500_rgb},
        {"orangered"sv, 0xFF4500_rgb},
        {"orchid"sv, 0xDA70D6_rgb},
        {"palegoldenrod"sv, 0xEEE8AA_rgb},
        {"palegreen"sv, 0x98FB98_rgb},
        {"paleturquoise"sv, 0xAFEEEE_rgb},
        {"palevioletred"sv, 0xDB7093_rgb},
        {"papayawhip"sv, 0xFFEFD5_rgb},
        {"peachpuff"sv, 0xFFDAB9_rgb},
        {"peru"sv, 0xCD853F_rgb},
        {"pink"sv, 0xFFC0CB_rgb},
        {"plum"sv, 0xDDA0DD_rgb},
        {"powderblue"sv, 0xB0E0E6_rgb},
        {"purple"sv, 0x800080_rgb},
        {"red"sv, 0xFF0000_rgb},
        {"rosybrown"sv, 0xBC8F8F_rgb},
        {"royalblue"sv, 0x4169E1_rgb},
        {"saddlebrown"sv, 0x8B4513_rgb},
        {"salmon"sv, 0xFA8072_rgb},
        {"sandybrown"sv, 0xF4A460_rgb},
        {"seagreen"sv, 0x2E8B57_rgb},
        {"seashell"sv, 0xFFF5EE_rgb},
        {"sienna"sv, 0xA0522D_rgb},
        {"silver"sv, 0xC0C0C0_rgb},
        {"skyblue"sv, 0x87CEEB_rgb},
        {"slateblue"sv, 0x6A5ACD_rgb},
        {"slategray"sv, 0x708090_rgb},
        {"slategrey"sv, 0x708090_rgb},
        {"snow"sv, 0xFFFAFA_rgb},
        {"springgreen"sv, 0x00FF7F_rgb},
        {"steelblue"sv, 0x4682B4_rgb},
        {"tan"sv, 0xD2B48C_rgb},
        {"teal"sv, 0x008080_rgb},
        {"thistle"sv, 0xD8BFD8_rgb},
        {"tomato"sv, 0xFF6347_rgb},
        {"turquoise"sv, 0x40E0D0_rgb},
        {"violet"sv, 0xEE82EE_rgb},
        {"wheat"sv, 0xF5DEB3_rgb},
        {"white"sv, 0xFFFFFF_rgb},
        {"whitesmoke"sv, 0xF5F5F5_rgb},
        {"yellow"sv, 0xFFFF00_rgb},
        {"yellowgreen"sv, 0x9ACD32_rgb},

        // Color keywords.
        {"transparent"sv, Color(RGBA(0, 0, 0, 0))},
        {"currentcolor"sv, Color(Color::CurrentColor())},
    }));

template <typename... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};

// Deduction guide needed for older compilers.
template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

/**
 * Convert HSL to RGBA, per https://www.w3.org/TR/css-color-4/#hsl-to-rgb
 *
 * @param hueDegrees Hue as degrees, will be normalized to [0, 360]
 * @param saturation  Saturation in reference range [0,1]
 * @param lightness  Lightness in reference range [0,1]
 */
RGBA hslToRgb(float hueDegrees, float saturation, float lightness) {
  hueDegrees = std::fmod(hueDegrees, 360.0f);

  if (hueDegrees < 0.0f) {
    hueDegrees += 360.0f;
  }

  auto f = [&](float n) {
    const float k = std::fmod(n + hueDegrees / 30.0f, 12.0f);
    const float a = saturation * Min(lightness, 1.0f - lightness);
    return lightness - a * Max(-1.0f, Min(k - 3.0f, 9.0f - k, 1.0f));
  };

  return RGBA::RGB(numberToChannel(f(0) * 255.0f), numberToChannel(f(8) * 255.0f),
                   numberToChannel(f(4) * 255.0f));
}

}  // namespace

std::optional<ColorSpaceId> ColorSpaceIdFromString(std::string_view name) {
  std::string lowered(name);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (lowered == "srgb") {
    return ColorSpaceId::SRGB;
  }
  if (lowered == "srgb-linear") {
    return ColorSpaceId::SRGBLinear;
  }
  if (lowered == "display-p3") {
    return ColorSpaceId::DisplayP3;
  }
  if (lowered == "a98-rgb") {
    return ColorSpaceId::A98Rgb;
  }
  if (lowered == "prophoto-rgb") {
    return ColorSpaceId::ProPhotoRgb;
  }
  if (lowered == "rec2020") {
    return ColorSpaceId::Rec2020;
  }
  if (lowered == "xyz-d65") {
    return ColorSpaceId::XyzD65;
  }
  if (lowered == "xyz-d50") {
    return ColorSpaceId::XyzD50;
  }

  return std::nullopt;
}

std::ostream& operator<<(std::ostream& os, const RGBA& color) {
  return os << "rgba(" << static_cast<int>(color.r) << ", " << static_cast<int>(color.g) << ", "
            << static_cast<int>(color.b) << ", " << static_cast<int>(color.a) << ")";
}

RGBA HSLA::toRGBA() const {
  RGBA result = hslToRgb(hDeg, s, l);
  result.a = a;
  return result;
}

std::ostream& operator<<(std::ostream& os, const HSLA& color) {
  return os << "hsla(" << color.hDeg << ", " << color.s * 100 << "%, " << color.l * 100 << "%, "
            << static_cast<int>(color.a) << ")";
}

std::ostream& operator<<(std::ostream& os, const ColorSpaceValue& value) {
  os << "color(" << colorSpaceIdToString(value.id) << " " << value.c1 << " " << value.c2 << " "
     << value.c3;
  if (value.alpha != 0xFF) {
    os << " / " << static_cast<int>(value.alpha);
  }
  return os << ")";
}

std::string RGBA::toHexString() const {
  // Convert this color to hex without using stringstream.
  constexpr char kHexDigits[] = "0123456789abcdef";

  std::string result = (a == 255) ? "#000000" : "#00000000";

  result[1] = kHexDigits[r >> 4];
  result[2] = kHexDigits[r & 0xf];
  result[3] = kHexDigits[g >> 4];
  result[4] = kHexDigits[g & 0xf];
  result[5] = kHexDigits[b >> 4];
  result[6] = kHexDigits[b & 0xf];
  if (a != 255) {
    result[7] = kHexDigits[a >> 4];
    result[8] = kHexDigits[a & 0xf];
  }

  return result;
}

std::optional<Color> Color::ByName(std::string_view name) {
  const Color* const color = kColors.find(name);
  if (color == nullptr) {
    return std::nullopt;
  }

  return *color;
}

RGBA Color::asRGBA() const {
  RGBA result;
  std::visit(
      overloaded{[&](RGBA rgba) { result = rgba; }, [&](HSLA hsla) { result = hsla.toRGBA(); },
                 [&](ColorSpaceValue space) { result = colorSpaceToRGBA(space); },
                 [&](auto&&) {
                   UTILS_RELEASE_ASSERT_MSG(
                       false, "Cannot convert currentColor to RGBA, use resolve() instead");
                 }},
      value);
  return result;
}

RGBA Color::resolve(RGBA currentColor, float opacity) const {
  RGBA value = isCurrentColor() ? currentColor : asRGBA();
  if (opacity != 1.0f) {
    value.a = static_cast<uint8_t>(static_cast<float>(value.a) * opacity);
  }

  return value;
}

std::ostream& operator<<(std::ostream& os, const Color& color) {
  std::visit([&os](auto&& element) { os << element; }, color.value);
  return os;
}

}  // namespace donner::css
