#include "donner/css/Color.h"

#include <array>
#include <cmath>

#include "donner/base/CompileTimeMap.h"
#include "donner/base/MathUtils.h"

namespace donner::css {

namespace {

using namespace string_literals;
using namespace std::string_view_literals;

static constexpr auto kColors = makeCompileTimeMap(std::to_array<std::pair<std::string_view, Color>>({
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

uint8_t numberToChannel(double number) {
  return static_cast<uint8_t>(Clamp(Round(number), 0.0, 255.0));
}

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
