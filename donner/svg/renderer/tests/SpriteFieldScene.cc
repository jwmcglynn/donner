#include "donner/svg/renderer/tests/SpriteFieldScene.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::tests {
namespace {

constexpr int kPathsPerSprite = 6;

/// A game frame fades a few sprites once in this many frames.
constexpr int kFramesPerFade = 8;

/// Serves a fixed set of documents from memory; any other URL is not found.
class MapResourceLoader final : public ResourceLoaderInterface {
public:
  explicit MapResourceLoader(std::map<std::string, std::string, std::less<>> documents)
      : documents_(std::move(documents)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    const auto found = documents_.find(url);
    if (found == documents_.end()) {
      return ResourceLoaderError::NotFound;
    }
    return std::vector<uint8_t>(found->second.begin(), found->second.end());
  }

private:
  std::map<std::string, std::string, std::less<>> documents_;
};

std::string SheetUrl(int sheet) {
  return "sheet-" + std::to_string(sheet) + ".svg";
}

std::string SpriteHref(int sheet, int sprite) {
  return SheetUrl(sheet) + "#sprite-" + std::to_string(sprite);
}

/// One sprite: a few filled curves, one sprite in four blurred with an objectBoundingBox filter
/// region and one in four clipped, like glowing and masked game pieces.
void AppendSprite(std::ostringstream& out, int sheet, int sprite) {
  out << "<g id=\"sprite-" << sprite << "\"";
  if (sprite % 4 == 1) {
    out << " filter=\"url(#glow)\"";
  } else if (sprite % 4 == 2) {
    out << " clip-path=\"url(#round)\"";
  }
  out << ">";
  for (int i = 0; i < kPathsPerSprite; ++i) {
    const int x = 4 + (i * 11 + sprite * 5) % 30;
    const int y = 4 + (i * 7 + sheet * 3) % 30;
    const int r = 4 + (i * 3) % 8;
    out << "<path fill=\"#" << std::hex << ((0x406080 + i * 0x0b0a09 + sheet * 0x131111) & 0xFFFFFF)
        << std::dec << "\" d=\"M" << x << ',' << y << " C" << x + r << ',' << y - r << ' '
        << x + 2 * r << ',' << y + r << ' ' << x + r << ',' << y + 2 * r << " S" << x - r << ','
        << y + r << ' ' << x << ',' << y << "Z\"/>";
  }
  out << "</g>";
}

std::string MakeSheetSource(int sheet, int spritesPerSheet) {
  std::ostringstream out;
  out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48"><defs>)"
      << R"(<filter id="glow"><feGaussianBlur stdDeviation="1.5"/></filter>)"
      << R"(<clipPath id="round"><circle cx="20" cy="20" r="16"/></clipPath></defs>)"
      << R"(<g opacity="0">)";
  for (int sprite = 0; sprite < spritesPerSheet; ++sprite) {
    AppendSprite(out, sheet, sprite);
  }
  out << "</g></svg>";
  return out.str();
}

std::string MakeHostSource(const SpriteFieldOptions& options) {
  std::ostringstream out;
  out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400">)"
      << R"(<rect width="400" height="400" fill="#203040"/><g id="field">)";
  for (int i = 0; i < options.sprites; ++i) {
    out << "<use id=\"u" << i << "\" href=\""
        << SpriteHref(i % options.sheets, i % options.spritesPerSheet) << "\"/>";
  }
  out << "</g></svg>";
  return out.str();
}

Transform2d SpriteTransform(int index, int frame) {
  const double x = (index * 37 + frame * 3) % 360;
  const double y = (index * 53 + frame * 2) % 360;
  return Transform2d::Translate(Vector2d(x, y));
}

}  // namespace

std::optional<SpriteFieldScene> MakeSpriteFieldScene(const SpriteFieldOptions& options) {
  std::map<std::string, std::string, std::less<>> sheets;
  for (int sheet = 0; sheet < options.sheets; ++sheet) {
    sheets.emplace(SheetUrl(sheet), MakeSheetSource(sheet, options.spritesPerSheet));
  }

  auto family = std::make_shared<components::DocumentResourceFamilyBudget>();
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  settings.resourceLoader = std::make_unique<MapResourceLoader>(std::move(sheets));
  // Mirrors the parser's default external-document settings.
  settings.svgParseCallback = [family](
                                  const std::vector<uint8_t>& bytes,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    SVGDocument::Settings resourceSettings;
    resourceSettings.processingMode = ProcessingMode::SecureStatic;
    resourceSettings.resourceFamilyBudget = family;
    auto parsed = parser::SVGParser::ParseSVG(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), warnings,
        parser::SVGParser::Options(), std::move(resourceSettings));
    if (parsed.hasError()) {
      return std::nullopt;
    }
    return parsed.result().handle();
  };

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(MakeHostSource(options), warnings,
                                            parser::SVGParser::Options(), std::move(settings));
  if (parsed.hasError()) {
    ADD_FAILURE() << "Sprite field parse failed: " << parsed.error();
    return std::nullopt;
  }

  SpriteFieldScene scene{std::move(parsed.result()), {}, options};
  for (int i = 0; i < options.sprites; ++i) {
    scene.uses.push_back(
        scene.document.querySelector("#u" + std::to_string(i))->cast<SVGUseElement>());
  }
  for (int i = 0; i < options.sprites; ++i) {
    scene.uses[i].setTransform(SpriteTransform(i, 0));
  }
  return scene;
}

void AdvanceSpriteField(SpriteFieldScene& scene, int frame, SpriteFieldChange change) {
  const SpriteFieldOptions& options = scene.options;
  const int count = static_cast<int>(scene.uses.size());
  if (change == SpriteFieldChange::Move || change == SpriteFieldChange::All) {
    for (int i = 0; i < count; ++i) {
      scene.uses[i].setTransform(SpriteTransform(i, frame));
    }
  }
  // A handful of sprites per frame flip to their next animation frame; every eighth frame of a
  // game, a few fade.
  const bool fades = change == SpriteFieldChange::Opacity ||
                     (change == SpriteFieldChange::All && frame % kFramesPerFade == 0);
  for (int i = frame % 7; i < count; i += 13) {
    if (fades) {
      scene.uses[i].setAttribute(xml::XMLQualifiedNameRef("opacity"),
                                 std::to_string(0.25 + 0.25 * ((frame + i) % 4)));
    }
    if (change == SpriteFieldChange::Retarget || change == SpriteFieldChange::All) {
      scene.uses[i].setHref(
          RcString(SpriteHref(i % options.sheets, (i + frame) % options.spritesPerSheet)));
    }
  }
}

}  // namespace donner::svg::tests
