#include "donner/css/Color.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

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

double decodeGamma(double value, double gamma) {
  const double clamped = Clamp(value, 0.0, 1.0);
  return pow(clamped, gamma);
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

void lchToLab(double L, double C, double HDeg, double& a, double& b) {
  const double HRad = HDeg * MathConstants<double>::kDegToRad;
  a = C * cos(HRad);
  b = C * sin(HRad);
}

void labToXYZ(double L, double a, double b, double& X, double& Y, double& Z) {
  const double kappa = 24389.0 / 27.0;
  const double epsilon = 216.0 / 24389.0;

  const double fy = (L + 16.0) / 116.0;
  const double fx = fy + (a / 500.0);
  const double fz = fy - (b / 200.0);

  const double fx3 = fx * fx * fx;
  const double fz3 = fz * fz * fz;

  const double xr = fx3 > epsilon ? fx3 : (116.0 * fx - 16.0) / kappa;
  const double yr = L > (kappa * epsilon) ? pow((L + 16.0) / 116.0, 3) : L / kappa;
  const double zr = fz3 > epsilon ? fz3 : (116.0 * fz - 16.0) / kappa;

  const double Xn = 0.96422;
  const double Yn = 1.0;
  const double Zn = 0.82521;

  X = xr * Xn;
  Y = yr * Yn;
  Z = zr * Zn;
}

void adaptD50toD65(double X_D50, double Y_D50, double Z_D50, double& X_D65, double& Y_D65,
                   double& Z_D65) {
  const double M[3][3] = {{0.9554734527042182, -0.0230985368742614, 0.0632593086610217},
                          {-0.0283697069632081, 1.0099954580058226, 0.0210413989669430},
                          {0.0123140016883199, -0.0205076964334771, 1.3303659908427779}};

  X_D65 = M[0][0] * X_D50 + M[0][1] * Y_D50 + M[0][2] * Z_D50;
  Y_D65 = M[1][0] * X_D50 + M[1][1] * Y_D50 + M[1][2] * Z_D50;
  Z_D65 = M[2][0] * X_D50 + M[2][1] * Y_D50 + M[2][2] * Z_D50;
}

Vec3 adaptD50toD65Vec(const Vec3& xyzD50) {
  Vec3 xyzD65;
  adaptD50toD65(xyzD50.x, xyzD50.y, xyzD50.z, xyzD65.x, xyzD65.y, xyzD65.z);
  return xyzD65;
}

void xyzToLinearRGB(double X, double Y, double Z, double& r, double& g, double& b) {
  r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
  g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
  b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
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

RGBA labToSRGB(double L, double a, double b, uint8_t alpha) {
  double X_D50, Y_D50, Z_D50;
  labToXYZ(L, a, b, X_D50, Y_D50, Z_D50);

  double X_D65, Y_D65, Z_D65;
  adaptD50toD65(X_D50, Y_D50, Z_D50, X_D65, Y_D65, Z_D65);

  double r_lin, g_lin, b_lin;
  xyzToLinearRGB(X_D65, Y_D65, Z_D65, r_lin, g_lin, b_lin);

  const uint8_t R = linearToSRGB(r_lin);
  const uint8_t G = linearToSRGB(g_lin);
  const uint8_t B = linearToSRGB(b_lin);

  return RGBA(R, G, B, alpha);
}

void oklchToOklab(double L, double C, double HDeg, double& a, double& b) {
  const double HRad = HDeg * MathConstants<double>::kDegToRad;
  a = C * cos(HRad);
  b = C * sin(HRad);
}

RGBA oklabToSRGB(double L, double a, double b, uint8_t alpha) {
  double l = L + 0.3963377774 * a + 0.2158037573 * b;
  double m = L - 0.1055613458 * a - 0.0638541728 * b;
  double s = L - 0.0894841775 * a - 1.2914855480 * b;

  l = l * l * l;
  m = m * m * m;
  s = s * s * s;

  double r_lin = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
  double g_lin = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
  double b_lin = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

  const uint8_t R = linearToSRGB(r_lin);
  const uint8_t G = linearToSRGB(g_lin);
  const uint8_t B = linearToSRGB(b_lin);
  return RGBA(R, G, B, alpha);
}

RGBA colorSpaceToRGBA(const ColorSpaceValue& value) {
  switch (value.id) {
    case ColorSpaceId::kSRGB:
      return RGBA(numberToChannel(value.c1 * 255.0), numberToChannel(value.c2 * 255.0),
                  numberToChannel(value.c3 * 255.0), value.alpha);
    case ColorSpaceId::kSRGBLinear:
      return RGBA(linearToSRGB(value.c1), linearToSRGB(value.c2), linearToSRGB(value.c3),
                  value.alpha);
    case ColorSpaceId::kDisplayP3:
      return rgbProfileToSRGB(value, kDisplayP3ToXyzD65, decodeSRGB, false);
    case ColorSpaceId::kA98Rgb: return rgbProfileToSRGB(value, kA98RgbToXyzD65, decodeA98, false);
    case ColorSpaceId::kProPhotoRgb:
      return rgbProfileToSRGB(value, kProPhotoToXyzD50, decodeProPhoto, true);
    case ColorSpaceId::kRec2020:
      return rgbProfileToSRGB(value, kRec2020ToXyzD65, decodeRec2020, false);
    case ColorSpaceId::kXyzD65: return xyzD65ToRGBA({value.c1, value.c2, value.c3}, value.alpha);
    case ColorSpaceId::kXyzD50:
      return xyzD65ToRGBA(adaptD50toD65Vec({value.c1, value.c2, value.c3}), value.alpha);
    case ColorSpaceId::kHwb:
      return hwbToRgb(normalizeAngleDegrees(value.c1), Clamp(value.c2, 0.0, 1.0),
                      Clamp(value.c3, 0.0, 1.0), value.alpha);
    case ColorSpaceId::kLab: return labToSRGB(value.c1, value.c2, value.c3, value.alpha);
    case ColorSpaceId::kLch: {
      double a = 0.0;
      double b = 0.0;
      lchToLab(value.c1, value.c2, value.c3, a, b);
      return labToSRGB(value.c1, a, b, value.alpha);
    }
    case ColorSpaceId::kOklab: return oklabToSRGB(value.c1, value.c2, value.c3, value.alpha);
    case ColorSpaceId::kOklch: {
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
    case ColorSpaceId::kSRGB: return "srgb";
    case ColorSpaceId::kSRGBLinear: return "srgb-linear";
    case ColorSpaceId::kDisplayP3: return "display-p3";
    case ColorSpaceId::kA98Rgb: return "a98-rgb";
    case ColorSpaceId::kProPhotoRgb: return "prophoto-rgb";
    case ColorSpaceId::kRec2020: return "rec2020";
    case ColorSpaceId::kXyzD65: return "xyz-d65";
    case ColorSpaceId::kXyzD50: return "xyz-d50";
    case ColorSpaceId::kHwb: return "hwb";
    case ColorSpaceId::kLab: return "lab";
    case ColorSpaceId::kLch: return "lch";
    case ColorSpaceId::kOklab: return "oklab";
    case ColorSpaceId::kOklch: return "oklch";
  }

  return "unknown";
}

using namespace string_literals;
using namespace frozen::string_literals;

static constexpr auto kColors = frozen::make_unordered_map<frozen::string, Color>({
    {"aliceblue"_s, 0xF0F8FF_rgb},
    {"antiquewhite"_s, 0xFAEBD7_rgb},
    {"aqua"_s, 0x00FFFF_rgb},
    {"aquamarine"_s, 0x7FFFD4_rgb},
    {"azure"_s, 0xF0FFFF_rgb},
    {"beige"_s, 0xF5F5DC_rgb},
    {"bisque"_s, 0xFFE4C4_rgb},
    {"black"_s, 0x000000_rgb},
    {"blanchedalmond"_s, 0xFFEBCD_rgb},
    {"blue"_s, 0x0000FF_rgb},
    {"blueviolet"_s, 0x8A2BE2_rgb},
    {"brown"_s, 0xA52A2A_rgb},
    {"burlywood"_s, 0xDEB887_rgb},
    {"cadetblue"_s, 0x5F9EA0_rgb},
    {"chartreuse"_s, 0x7FFF00_rgb},
    {"chocolate"_s, 0xD2691E_rgb},
    {"coral"_s, 0xFF7F50_rgb},
    {"cornflowerblue"_s, 0x6495ED_rgb},
    {"cornsilk"_s, 0xFFF8DC_rgb},
    {"crimson"_s, 0xDC143C_rgb},
    {"cyan"_s, 0x00FFFF_rgb},
    {"darkblue"_s, 0x00008B_rgb},
    {"darkcyan"_s, 0x008B8B_rgb},
    {"darkgoldenrod"_s, 0xB8860B_rgb},
    {"darkgray"_s, 0xA9A9A9_rgb},
    {"darkgreen"_s, 0x006400_rgb},
    {"darkgrey"_s, 0xA9A9A9_rgb},
    {"darkkhaki"_s, 0xBDB76B_rgb},
    {"darkmagenta"_s, 0x8B008B_rgb},
    {"darkolivegreen"_s, 0x556B2F_rgb},
    {"darkorange"_s, 0xFF8C00_rgb},
    {"darkorchid"_s, 0x9932CC_rgb},
    {"darkred"_s, 0x8B0000_rgb},
    {"darksalmon"_s, 0xE9967A_rgb},
    {"darkseagreen"_s, 0x8FBC8F_rgb},
    {"darkslateblue"_s, 0x483D8B_rgb},
    {"darkslategray"_s, 0x2F4F4F_rgb},
    {"darkslategrey"_s, 0x2F4F4F_rgb},
    {"darkturquoise"_s, 0x00CED1_rgb},
    {"darkviolet"_s, 0x9400D3_rgb},
    {"deeppink"_s, 0xFF1493_rgb},
    {"deepskyblue"_s, 0x00BFFF_rgb},
    {"dimgray"_s, 0x696969_rgb},
    {"dimgrey"_s, 0x696969_rgb},
    {"dodgerblue"_s, 0x1E90FF_rgb},
    {"firebrick"_s, 0xB22222_rgb},
    {"floralwhite"_s, 0xFFFAF0_rgb},
    {"forestgreen"_s, 0x228B22_rgb},
    {"fuchsia"_s, 0xFF00FF_rgb},
    {"gainsboro"_s, 0xDCDCDC_rgb},
    {"ghostwhite"_s, 0xF8F8FF_rgb},
    {"gold"_s, 0xFFD700_rgb},
    {"goldenrod"_s, 0xDAA520_rgb},
    {"gray"_s, 0x808080_rgb},
    {"green"_s, 0x008000_rgb},
    {"greenyellow"_s, 0xADFF2F_rgb},
    {"grey"_s, 0x808080_rgb},
    {"honeydew"_s, 0xF0FFF0_rgb},
    {"hotpink"_s, 0xFF69B4_rgb},
    {"indianred"_s, 0xCD5C5C_rgb},
    {"indigo"_s, 0x4B0082_rgb},
    {"ivory"_s, 0xFFFFF0_rgb},
    {"khaki"_s, 0xF0E68C_rgb},
    {"lavender"_s, 0xE6E6FA_rgb},
    {"lavenderblush"_s, 0xFFF0F5_rgb},
    {"lawngreen"_s, 0x7CFC00_rgb},
    {"lemonchiffon"_s, 0xFFFACD_rgb},
    {"lightblue"_s, 0xADD8E6_rgb},
    {"lightcoral"_s, 0xF08080_rgb},
    {"lightcyan"_s, 0xE0FFFF_rgb},
    {"lightgoldenrodyellow"_s, 0xFAFAD2_rgb},
    {"lightgray"_s, 0xD3D3D3_rgb},
    {"lightgreen"_s, 0x90EE90_rgb},
    {"lightgrey"_s, 0xD3D3D3_rgb},
    {"lightpink"_s, 0xFFB6C1_rgb},
    {"lightsalmon"_s, 0xFFA07A_rgb},
    {"lightseagreen"_s, 0x20B2AA_rgb},
    {"lightskyblue"_s, 0x87CEFA_rgb},
    {"lightslategray"_s, 0x778899_rgb},
    {"lightslategrey"_s, 0x778899_rgb},
    {"lightsteelblue"_s, 0xB0C4DE_rgb},
    {"lightyellow"_s, 0xFFFFE0_rgb},
    {"lime"_s, 0x00FF00_rgb},
    {"limegreen"_s, 0x32CD32_rgb},
    {"linen"_s, 0xFAF0E6_rgb},
    {"magenta"_s, 0xFF00FF_rgb},
    {"maroon"_s, 0x800000_rgb},
    {"mediumaquamarine"_s, 0x66CDAA_rgb},
    {"mediumblue"_s, 0x0000CD_rgb},
    {"mediumorchid"_s, 0xBA55D3_rgb},
    {"mediumpurple"_s, 0x9370DB_rgb},
    {"mediumseagreen"_s, 0x3CB371_rgb},
    {"mediumslateblue"_s, 0x7B68EE_rgb},
    {"mediumspringgreen"_s, 0x00FA9A_rgb},
    {"mediumturquoise"_s, 0x48D1CC_rgb},
    {"mediumvioletred"_s, 0xC71585_rgb},
    {"midnightblue"_s, 0x191970_rgb},
    {"mintcream"_s, 0xF5FFFA_rgb},
    {"mistyrose"_s, 0xFFE4E1_rgb},
    {"moccasin"_s, 0xFFE4B5_rgb},
    {"navajowhite"_s, 0xFFDEAD_rgb},
    {"navy"_s, 0x000080_rgb},
    {"oldlace"_s, 0xFDF5E6_rgb},
    {"olive"_s, 0x808000_rgb},
    {"olivedrab"_s, 0x6B8E23_rgb},
    {"orange"_s, 0xFFA500_rgb},
    {"orangered"_s, 0xFF4500_rgb},
    {"orchid"_s, 0xDA70D6_rgb},
    {"palegoldenrod"_s, 0xEEE8AA_rgb},
    {"palegreen"_s, 0x98FB98_rgb},
    {"paleturquoise"_s, 0xAFEEEE_rgb},
    {"palevioletred"_s, 0xDB7093_rgb},
    {"papayawhip"_s, 0xFFEFD5_rgb},
    {"peachpuff"_s, 0xFFDAB9_rgb},
    {"peru"_s, 0xCD853F_rgb},
    {"pink"_s, 0xFFC0CB_rgb},
    {"plum"_s, 0xDDA0DD_rgb},
    {"powderblue"_s, 0xB0E0E6_rgb},
    {"purple"_s, 0x800080_rgb},
    {"red"_s, 0xFF0000_rgb},
    {"rosybrown"_s, 0xBC8F8F_rgb},
    {"royalblue"_s, 0x4169E1_rgb},
    {"saddlebrown"_s, 0x8B4513_rgb},
    {"salmon"_s, 0xFA8072_rgb},
    {"sandybrown"_s, 0xF4A460_rgb},
    {"seagreen"_s, 0x2E8B57_rgb},
    {"seashell"_s, 0xFFF5EE_rgb},
    {"sienna"_s, 0xA0522D_rgb},
    {"silver"_s, 0xC0C0C0_rgb},
    {"skyblue"_s, 0x87CEEB_rgb},
    {"slateblue"_s, 0x6A5ACD_rgb},
    {"slategray"_s, 0x708090_rgb},
    {"slategrey"_s, 0x708090_rgb},
    {"snow"_s, 0xFFFAFA_rgb},
    {"springgreen"_s, 0x00FF7F_rgb},
    {"steelblue"_s, 0x4682B4_rgb},
    {"tan"_s, 0xD2B48C_rgb},
    {"teal"_s, 0x008080_rgb},
    {"thistle"_s, 0xD8BFD8_rgb},
    {"tomato"_s, 0xFF6347_rgb},
    {"turquoise"_s, 0x40E0D0_rgb},
    {"violet"_s, 0xEE82EE_rgb},
    {"wheat"_s, 0xF5DEB3_rgb},
    {"white"_s, 0xFFFFFF_rgb},
    {"whitesmoke"_s, 0xF5F5F5_rgb},
    {"yellow"_s, 0xFFFF00_rgb},
    {"yellowgreen"_s, 0x9ACD32_rgb},

    // Color keywords.
    {"transparent"_s, Color(RGBA(0, 0, 0, 0))},
    {"currentcolor"_s, Color(Color::CurrentColor())},
});

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
    return ColorSpaceId::kSRGB;
  }
  if (lowered == "srgb-linear") {
    return ColorSpaceId::kSRGBLinear;
  }
  if (lowered == "display-p3") {
    return ColorSpaceId::kDisplayP3;
  }
  if (lowered == "a98-rgb") {
    return ColorSpaceId::kA98Rgb;
  }
  if (lowered == "prophoto-rgb") {
    return ColorSpaceId::kProPhotoRgb;
  }
  if (lowered == "rec2020") {
    return ColorSpaceId::kRec2020;
  }
  if (lowered == "xyz-d65") {
    return ColorSpaceId::kXyzD65;
  }
  if (lowered == "xyz-d50") {
    return ColorSpaceId::kXyzD50;
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
  const auto it = kColors.find(name);
  if (it == kColors.end()) {
    return std::nullopt;
  }

  return it->second;
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
