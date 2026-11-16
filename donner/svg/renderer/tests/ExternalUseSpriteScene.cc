#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/encoding/Base64.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::tests {
namespace {

constexpr int kBitmapWidth = 228;
constexpr int kBitmapHeight = 229;
constexpr int kPathsPerVectorSprite = 45;

/// Serves only \ref kExternalResourceUrl; any other URL is not found.
class SingleResourceLoader final : public ResourceLoaderInterface {
public:
  explicit SingleResourceLoader(std::string source) : bytes_(source.begin(), source.end()) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    if (url != kExternalResourceUrl) {
      return ResourceLoaderError::NotFound;
    }
    return bytes_;
  }

private:
  std::vector<uint8_t> bytes_;
};

std::string MakeBitmapDataUri(int sprite) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kBitmapWidth) * kBitmapHeight * 4);
  for (int y = 0; y < kBitmapHeight; ++y) {
    for (int x = 0; x < kBitmapWidth; ++x) {
      uint8_t* pixel = &pixels[(static_cast<size_t>(y) * kBitmapWidth + x) * 4];
      pixel[0] = static_cast<uint8_t>((x + sprite * 20) & 0xFF);
      pixel[1] = static_cast<uint8_t>(y & 0xFF);
      pixel[2] = static_cast<uint8_t>((x * y) & 0xFF);
      pixel[3] = 255;
    }
  }
  const std::vector<uint8_t> png =
      RendererImageIO::writeRgbaPixelsToPngMemory(pixels, kBitmapWidth, kBitmapHeight);
  return "data:image/png;base64," + EncodeBase64Data(png);
}

void AppendVectorSprite(std::ostringstream& out, int sprite) {
  for (int i = 0; i < kPathsPerVectorSprite; ++i) {
    const int x = 10 + (i * 37 + sprite * 11) % 180;
    const int y = 10 + (i * 53 + sprite * 7) % 180;
    const int r = 8 + (i * 13) % 30;
    out << "<path fill=\"#" << std::hex
        << ((0x305070 + i * 0x0a0b0c + sprite * 0x111111) & 0xFFFFFF) << std::dec << "\" d=\"M" << x
        << ',' << y << " C" << x + r << ',' << y - r << ' ' << x + 2 * r << ',' << y + r << ' '
        << x + r << ',' << y + 2 * r << " S" << x - r << ',' << y + r << ' ' << x << ',' << y
        << "Z\"/>";
  }
}

}  // namespace

std::string MakeSpriteSheetSource(SpriteKind kind) {
  std::ostringstream out;
  out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="228" height="229"><g opacity="0">)";
  for (int sprite = 0; sprite < kSpriteSheetSpriteCount; ++sprite) {
    out << "<g id=\"sprite-" << sprite << "\">";
    if (kind == SpriteKind::Bitmap) {
      out << "<image width=\"228\" height=\"229\" href=\"" << MakeBitmapDataUri(sprite) << "\"/>";
    } else {
      AppendVectorSprite(out, sprite);
    }
    out << "</g>";
  }
  out << "</g></svg>";
  return out.str();
}

std::optional<ExternalUseDocument> MakeExternalUseDocument(std::string_view hostSource,
                                                           std::string resourceSource) {
  auto resource = std::make_shared<SVGDocumentHandle>();
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>();
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  settings.resourceLoader = std::make_unique<SingleResourceLoader>(std::move(resourceSource));
  // Mirrors the parser's default external-document settings, retaining the handle.
  settings.svgParseCallback = [resource, family](
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
    *resource = parsed.result().handle();
    return *resource;
  };
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(hostSource, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  if (parsed.hasError()) {
    ADD_FAILURE() << "External-use host parse failed: " << parsed.error();
    return std::nullopt;
  }
  return ExternalUseDocument{std::move(parsed.result()), std::move(resource)};
}

std::optional<ExternalUseSpriteScene> MakeExternalUseSpriteScene(SpriteKind kind) {
  constexpr std::string_view kHost = R"SVG(
    <svg xmlns="http://www.w3.org/2000/svg" width="800" height="1750" viewBox="0 0 400 875">
      <defs>
        <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stop-color="#4a7bd0"/>
          <stop offset="1" stop-color="#f4d9a0"/>
        </linearGradient>
      </defs>
      <rect width="400" height="875" fill="url(#sky)"/>
      <circle cx="80" cy="120" r="30" fill="#ffffff" opacity="0.8"/>
      <circle cx="300" cy="200" r="45" fill="#ffffff" opacity="0.6"/>
      <circle cx="200" cy="700" r="60" fill="#2c6e2c"/>
      <use href="sprites.svg#sprite-3" transform="translate(280 600) scale(0.4)"/>
      <use id="character" href="sprites.svg#sprite-0" transform="translate(100 400)"/>
    </svg>)SVG";

  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kHost, MakeSpriteSheetSource(kind));
  if (!host) {
    return std::nullopt;
  }
  SVGUseElement character = host->document.querySelector("#character")->cast<SVGUseElement>();
  return ExternalUseSpriteScene{std::move(*host), std::move(character)};
}

void MoveExternalUseSprite(ExternalUseSpriteScene& scene, int frame) {
  scene.character.setTransform(
      Transform2d::Translate(Vector2d(100.0 + (frame % 60) * 2.0, 400.0 - (frame % 20))));
}

void SetExternalUseSprite(ExternalUseSpriteScene& scene, int sprite) {
  scene.character.setHref(
      RcString(std::string(kExternalResourceUrl) + "#sprite-" + std::to_string(sprite)));
}

void AdvanceExternalUseSpriteFrame(ExternalUseSpriteScene& scene, int frame) {
  MoveExternalUseSprite(scene, frame);
  if (frame > 0 && frame % kFramesPerSpriteChange == 0) {
    SetExternalUseSprite(scene, (frame / kFramesPerSpriteChange) % kSpriteSheetSpriteCount);
  }
}

}  // namespace donner::svg::tests
