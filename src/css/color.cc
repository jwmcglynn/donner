#include "src/css/color.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

namespace donner::css {

namespace {

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

}  // namespace

bool Color::operator==(const Color& other) const {
  return value == other.value;
}

std::optional<Color> Color::ByName(std::string_view name) {
  const auto it = kColors.find(name);
  if (it == kColors.end()) {
    return std::nullopt;
  }

  return it->second;
}

std::ostream& operator<<(std::ostream& os, const Color& color) {
  os << "Color(";
  if (color.isCurrentColor()) {
    os << "currentColor";
  } else {
    const RGBA rgba = color.rgba();
    os << static_cast<int>(rgba.r) << ", " << static_cast<int>(rgba.g) << ", "
       << static_cast<int>(rgba.b) << ", " << static_cast<int>(rgba.a);
  }
  os << ")";
  return os;
}

}  // namespace donner::css
