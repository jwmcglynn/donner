#include "donner/svg/renderer/tests/SpriteSceneFixture.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <variant>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::tests {
namespace {

constexpr int kSheets = 3;
constexpr int kSpritesPerSheet = 3;

/// Serves the scene's sprite sheets by URL.
class SheetResourceLoader final : public ResourceLoaderInterface {
public:
  explicit SheetResourceLoader(const std::map<std::string, std::string, std::less<>>& sheets)
      : sheets_(sheets) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    const auto it = sheets_.find(url);
    if (it == sheets_.end()) {
      return ResourceLoaderError::NotFound;
    }
    return std::vector<uint8_t>(it->second.begin(), it->second.end());
  }

private:
  std::map<std::string, std::string, std::less<>> sheets_;
};

std::string SheetUrl(int sheet) {
  return "sheet-" + std::to_string(sheet) + ".svg";
}

/// One character sprite: an outlined curved body with a gradient, eyes, and a highlight.
std::string CharacterSprite(const std::string& id, int sheet, int kind, bool glowing,
                            bool labeled) {
  static constexpr const char* kBodyColors[] = {"#e05090", "#40c0f0", "#f0c040",
                                                "#60d070", "#a070e0", "#f08040"};
  const char* color = kBodyColors[(sheet * kSpritesPerSheet + kind) % 6];
  const double wobble = 6.0 + kind * 3.0;
  std::string body = "<path d=\"M0,-40 C" + std::to_string(30 + wobble) + ",-44 48,-18 46,4 C44," +
                     std::to_string(30 + wobble) + " 22,46 0,44 C-22,46 -44," +
                     std::to_string(30 - wobble) +
                     " -46,4 C-48,-18 -30,-44 0,-40 Z\" fill=\"url(#" + id +
                     "-grad)\" stroke=\"#202020\" stroke-width=\"3\"/>";
  std::string sprite = "<radialGradient id=\"" + id +
                       "-grad\" cx=\"0.4\" cy=\"0.35\" r=\"0.7\"><stop offset=\"0\" "
                       "stop-color=\"#ffffff\"/><stop offset=\"0.35\" stop-color=\"" +
                       color + "\"/><stop offset=\"1\" stop-color=\"#302030\"/></radialGradient>";
  sprite += "<g id=\"" + id + "\"" + (glowing ? " filter=\"url(#glow)\"" : "") + ">" + body +
            "<ellipse cx=\"-15\" cy=\"-8\" rx=\"9\" ry=\"12\" fill=\"#ffffff\"/>"
            "<ellipse cx=\"15\" cy=\"-8\" rx=\"9\" ry=\"12\" fill=\"#ffffff\"/>"
            "<circle cx=\"-13\" cy=\"-5\" r=\"4.5\" fill=\"#101010\"/>"
            "<circle cx=\"17\" cy=\"-5\" r=\"4.5\" fill=\"#101010\"/>"
            "<path d=\"M-14,18 Q0,30 14,18\" fill=\"none\" stroke=\"#101010\" stroke-width=\"3\" "
            "stroke-linecap=\"round\"/>"
            "<ellipse cx=\"-20\" cy=\"-26\" rx=\"10\" ry=\"5\" fill=\"#ffffff\" opacity=\"0.5\"/>";
  if (labeled) {
    sprite +=
        "<text x=\"0\" y=\"62\" font-size=\"16\" text-anchor=\"middle\" "
        "fill=\"#ffffff\">Go</text>";
  }
  return sprite + "</g>";
}

std::string SheetSource(int sheet, bool includeText, bool unreferencedText) {
  std::string source = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"400\">";
  source += R"svg(<filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="6" result="halo"/>
      <feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter><defs>)svg";
  for (int kind = 0; kind < kSpritesPerSheet; ++kind) {
    const std::string id = "s" + std::to_string(kind);
    source += CharacterSprite(id, sheet, kind, /*glowing=*/false, /*labeled=*/false);
    source += CharacterSprite(id + "-glow", sheet, kind, /*glowing=*/true, /*labeled=*/false);
    if (includeText && sheet == 0) {
      source += CharacterSprite(id + "-label", sheet, kind, /*glowing=*/false, /*labeled=*/true);
    }
  }
  if (unreferencedText) {
    source += "<text id=\"note\" x=\"4\" y=\"20\" font-size=\"12\">sheet</text>";
  }
  return source + "</defs></svg>";
}

}  // namespace

SpriteScene::SpriteScene(const SpriteSceneOptions& options) : options_(options) {
  for (int sheet = 0; sheet < kSheets; ++sheet) {
    sheets_.emplace(SheetUrl(sheet),
                    SheetSource(sheet, options.includeText, options.unreferencedText));
  }

  hostSource_ = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
                std::to_string(options.width) + "\" height=\"" + std::to_string(options.height) +
                "\"><rect width=\"100%\" height=\"100%\" fill=\"#203040\"/>";
  for (int index = 0; index < options.sprites; ++index) {
    const int sheet = index % kSheets;
    const int kind = (index / kSheets) % kSpritesPerSheet;
    std::string fragment = "s" + std::to_string(kind);
    if (options.glowingEvery > 0 && index % options.glowingEvery == 0) {
      fragment += "-glow";
    } else if (options.includeText && sheet == 0 && index % 5 == 0) {
      fragment += "-label";
    }
    hostSource_ += "<use id=\"u" + std::to_string(index) + "\" href=\"" + SheetUrl(sheet) + "#" +
                   fragment + "\" transform=\"" + transformFor(index, 0) + "\"/>";
  }
  hostSource_ += "</svg>";

  SVGDocument::Settings settings;
  settings.resourceLoader = std::make_unique<SheetResourceLoader>(sheets_);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(hostSource_, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  UTILS_RELEASE_ASSERT_MSG(!parsed.hasError(), "sprite scene host failed to parse");
  document_ = std::move(parsed.result());
  for (int index = 0; index < options.sprites; ++index) {
    auto element = document_.querySelector("#u" + std::to_string(index));
    UTILS_RELEASE_ASSERT_MSG(element.has_value(), "sprite scene occurrence missing");
    uses_.push_back(*element);
  }
}

std::string SpriteScene::transformFor(int index, int frame) const {
  const int columns = 10;
  const double cellWidth = static_cast<double>(options_.width) / columns;
  const double rows = std::ceil(static_cast<double>(options_.sprites) / columns);
  const double cellHeight = static_cast<double>(options_.height) / std::max(rows, 1.0);
  const double x = cellWidth * (index % columns + 0.5) + std::fmod(frame * 1.7 + index * 3.0, 20.0);
  const double y = cellHeight * (index / columns + 0.5) + std::fmod(frame * 2.3, 24.0);
  return "translate(" + std::to_string(x) + " " + std::to_string(y) + ")";
}

void SpriteScene::advance(int frame) {
  for (int index = 0; index < static_cast<int>(uses_.size()); ++index) {
    uses_[static_cast<size_t>(index)].setAttribute("transform", transformFor(index, frame));
  }
}

}  // namespace donner::svg::tests
