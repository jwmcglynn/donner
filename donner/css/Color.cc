#include "donner/css/Color.h"

#include <array>
#include <cmath>

#include "donner/base/CompileTimeMap.h"
#include "donner/base/MathUtils.h"

namespace donner::css {

namespace {

using namespace std::string_view_literals;

static constexpr auto kColors = makeCompileTimeMap(std::to_array<std::pair<std::string_view, Color>>({
    {"aliceblue"sv, RgbHex(0xF0F8FF)},
    {"antiquewhite"sv, RgbHex(0xFAEBD7)},
    {"aqua"sv, RgbHex(0x00FFFF)},
    {"aquamarine"sv, RgbHex(0x7FFFD4)},
    {"azure"sv, RgbHex(0xF0FFFF)},
    {"beige"sv, RgbHex(0xF5F5DC)},
    {"bisque"sv, RgbHex(0xFFE4C4)},
    {"black"sv, RgbHex(0x000000)},
    {"blanchedalmond"sv, RgbHex(0xFFEBCD)},
    {"blue"sv, RgbHex(0x0000FF)},
    {"blueviolet"sv, RgbHex(0x8A2BE2)},
    {"brown"sv, RgbHex(0xA52A2A)},
    {"burlywood"sv, RgbHex(0xDEB887)},
    {"cadetblue"sv, RgbHex(0x5F9EA0)},
    {"chartreuse"sv, RgbHex(0x7FFF00)},
    {"chocolate"sv, RgbHex(0xD2691E)},
    {"coral"sv, RgbHex(0xFF7F50)},
    {"cornflowerblue"sv, RgbHex(0x6495ED)},
    {"cornsilk"sv, RgbHex(0xFFF8DC)},
    {"crimson"sv, RgbHex(0xDC143C)},
    {"cyan"sv, RgbHex(0x00FFFF)},
    {"darkblue"sv, RgbHex(0x00008B)},
    {"darkcyan"sv, RgbHex(0x008B8B)},
    {"darkgoldenrod"sv, RgbHex(0xB8860B)},
    {"darkgray"sv, RgbHex(0xA9A9A9)},
    {"darkgreen"sv, RgbHex(0x006400)},
    {"darkgrey"sv, RgbHex(0xA9A9A9)},
    {"darkkhaki"sv, RgbHex(0xBDB76B)},
    {"darkmagenta"sv, RgbHex(0x8B008B)},
    {"darkolivegreen"sv, RgbHex(0x556B2F)},
    {"darkorange"sv, RgbHex(0xFF8C00)},
    {"darkorchid"sv, RgbHex(0x9932CC)},
    {"darkred"sv, RgbHex(0x8B0000)},
    {"darksalmon"sv, RgbHex(0xE9967A)},
    {"darkseagreen"sv, RgbHex(0x8FBC8F)},
    {"darkslateblue"sv, RgbHex(0x483D8B)},
    {"darkslategray"sv, RgbHex(0x2F4F4F)},
    {"darkslategrey"sv, RgbHex(0x2F4F4F)},
    {"darkturquoise"sv, RgbHex(0x00CED1)},
    {"darkviolet"sv, RgbHex(0x9400D3)},
    {"deeppink"sv, RgbHex(0xFF1493)},
    {"deepskyblue"sv, RgbHex(0x00BFFF)},
    {"dimgray"sv, RgbHex(0x696969)},
    {"dimgrey"sv, RgbHex(0x696969)},
    {"dodgerblue"sv, RgbHex(0x1E90FF)},
    {"firebrick"sv, RgbHex(0xB22222)},
    {"floralwhite"sv, RgbHex(0xFFFAF0)},
    {"forestgreen"sv, RgbHex(0x228B22)},
    {"fuchsia"sv, RgbHex(0xFF00FF)},
    {"gainsboro"sv, RgbHex(0xDCDCDC)},
    {"ghostwhite"sv, RgbHex(0xF8F8FF)},
    {"gold"sv, RgbHex(0xFFD700)},
    {"goldenrod"sv, RgbHex(0xDAA520)},
    {"gray"sv, RgbHex(0x808080)},
    {"green"sv, RgbHex(0x008000)},
    {"greenyellow"sv, RgbHex(0xADFF2F)},
    {"grey"sv, RgbHex(0x808080)},
    {"honeydew"sv, RgbHex(0xF0FFF0)},
    {"hotpink"sv, RgbHex(0xFF69B4)},
    {"indianred"sv, RgbHex(0xCD5C5C)},
    {"indigo"sv, RgbHex(0x4B0082)},
    {"ivory"sv, RgbHex(0xFFFFF0)},
    {"khaki"sv, RgbHex(0xF0E68C)},
    {"lavender"sv, RgbHex(0xE6E6FA)},
    {"lavenderblush"sv, RgbHex(0xFFF0F5)},
    {"lawngreen"sv, RgbHex(0x7CFC00)},
    {"lemonchiffon"sv, RgbHex(0xFFFACD)},
    {"lightblue"sv, RgbHex(0xADD8E6)},
    {"lightcoral"sv, RgbHex(0xF08080)},
    {"lightcyan"sv, RgbHex(0xE0FFFF)},
    {"lightgoldenrodyellow"sv, RgbHex(0xFAFAD2)},
    {"lightgray"sv, RgbHex(0xD3D3D3)},
    {"lightgreen"sv, RgbHex(0x90EE90)},
    {"lightgrey"sv, RgbHex(0xD3D3D3)},
    {"lightpink"sv, RgbHex(0xFFB6C1)},
    {"lightsalmon"sv, RgbHex(0xFFA07A)},
    {"lightseagreen"sv, RgbHex(0x20B2AA)},
    {"lightskyblue"sv, RgbHex(0x87CEFA)},
    {"lightslategray"sv, RgbHex(0x778899)},
    {"lightslategrey"sv, RgbHex(0x778899)},
    {"lightsteelblue"sv, RgbHex(0xB0C4DE)},
    {"lightyellow"sv, RgbHex(0xFFFFE0)},
    {"lime"sv, RgbHex(0x00FF00)},
    {"limegreen"sv, RgbHex(0x32CD32)},
    {"linen"sv, RgbHex(0xFAF0E6)},
    {"magenta"sv, RgbHex(0xFF00FF)},
    {"maroon"sv, RgbHex(0x800000)},
    {"mediumaquamarine"sv, RgbHex(0x66CDAA)},
    {"mediumblue"sv, RgbHex(0x0000CD)},
    {"mediumorchid"sv, RgbHex(0xBA55D3)},
    {"mediumpurple"sv, RgbHex(0x9370DB)},
    {"mediumseagreen"sv, RgbHex(0x3CB371)},
    {"mediumslateblue"sv, RgbHex(0x7B68EE)},
    {"mediumspringgreen"sv, RgbHex(0x00FA9A)},
    {"mediumturquoise"sv, RgbHex(0x48D1CC)},
    {"mediumvioletred"sv, RgbHex(0xC71585)},
    {"midnightblue"sv, RgbHex(0x191970)},
    {"mintcream"sv, RgbHex(0xF5FFFA)},
    {"mistyrose"sv, RgbHex(0xFFE4E1)},
    {"moccasin"sv, RgbHex(0xFFE4B5)},
    {"navajowhite"sv, RgbHex(0xFFDEAD)},
    {"navy"sv, RgbHex(0x000080)},
    {"oldlace"sv, RgbHex(0xFDF5E6)},
    {"olive"sv, RgbHex(0x808000)},
    {"olivedrab"sv, RgbHex(0x6B8E23)},
    {"orange"sv, RgbHex(0xFFA500)},
    {"orangered"sv, RgbHex(0xFF4500)},
    {"orchid"sv, RgbHex(0xDA70D6)},
    {"palegoldenrod"sv, RgbHex(0xEEE8AA)},
    {"palegreen"sv, RgbHex(0x98FB98)},
    {"paleturquoise"sv, RgbHex(0xAFEEEE)},
    {"palevioletred"sv, RgbHex(0xDB7093)},
    {"papayawhip"sv, RgbHex(0xFFEFD5)},
    {"peachpuff"sv, RgbHex(0xFFDAB9)},
    {"peru"sv, RgbHex(0xCD853F)},
    {"pink"sv, RgbHex(0xFFC0CB)},
    {"plum"sv, RgbHex(0xDDA0DD)},
    {"powderblue"sv, RgbHex(0xB0E0E6)},
    {"purple"sv, RgbHex(0x800080)},
    {"red"sv, RgbHex(0xFF0000)},
    {"rosybrown"sv, RgbHex(0xBC8F8F)},
    {"royalblue"sv, RgbHex(0x4169E1)},
    {"saddlebrown"sv, RgbHex(0x8B4513)},
    {"salmon"sv, RgbHex(0xFA8072)},
    {"sandybrown"sv, RgbHex(0xF4A460)},
    {"seagreen"sv, RgbHex(0x2E8B57)},
    {"seashell"sv, RgbHex(0xFFF5EE)},
    {"sienna"sv, RgbHex(0xA0522D)},
    {"silver"sv, RgbHex(0xC0C0C0)},
    {"skyblue"sv, RgbHex(0x87CEEB)},
    {"slateblue"sv, RgbHex(0x6A5ACD)},
    {"slategray"sv, RgbHex(0x708090)},
    {"slategrey"sv, RgbHex(0x708090)},
    {"snow"sv, RgbHex(0xFFFAFA)},
    {"springgreen"sv, RgbHex(0x00FF7F)},
    {"steelblue"sv, RgbHex(0x4682B4)},
    {"tan"sv, RgbHex(0xD2B48C)},
    {"teal"sv, RgbHex(0x008080)},
    {"thistle"sv, RgbHex(0xD8BFD8)},
    {"tomato"sv, RgbHex(0xFF6347)},
    {"turquoise"sv, RgbHex(0x40E0D0)},
    {"violet"sv, RgbHex(0xEE82EE)},
    {"wheat"sv, RgbHex(0xF5DEB3)},
    {"white"sv, RgbHex(0xFFFFFF)},
    {"whitesmoke"sv, RgbHex(0xF5F5F5)},
    {"yellow"sv, RgbHex(0xFFFF00)},
    {"yellowgreen"sv, RgbHex(0x9ACD32)},

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
