/// @file
/// External `<use>` content drawn from another document must render like the same content
/// referenced in-document. The renderer prepares filter graphs and tracks pending subtree layers
/// by entity, and a referenced document's entity ids overlap its host's, so each case pairs
/// filtered or layered sheet content with host content that the overlap would disturb.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {
namespace {

using test::PixelAt;
using ::testing::Gt;
using ::testing::Lt;

/// Serves in-memory documents by URL; every other URL is not found.
class InMemoryLoader final : public ResourceLoaderInterface {
public:
  explicit InMemoryLoader(std::map<std::string, std::string, std::less<>> documents)
      : documents_(std::move(documents)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    const auto it = documents_.find(url);
    if (it == documents_.end()) {
      return ResourceLoaderError::NotFound;
    }
    return std::vector<uint8_t>(it->second.begin(), it->second.end());
  }

private:
  std::map<std::string, std::string, std::less<>> documents_;
};

SVGDocument Parse(std::string_view source,
                  std::map<std::string, std::string, std::less<>> documents = {}) {
  SVGDocument::Settings settings;
  settings.resourceLoader = std::make_unique<InMemoryLoader>(std::move(documents));
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  EXPECT_FALSE(parsed.hasError());
  return std::move(parsed.result());
}

RendererBitmap Draw(Renderer& renderer, SVGDocument& document) {
  renderer.draw(document);
  RendererBitmap snapshot = renderer.takeSnapshot();
  const size_t tightRowBytes = static_cast<size_t>(snapshot.dimensions.x) * 4;
  if (snapshot.rowBytes == tightRowBytes) {
    return snapshot;
  }
  RendererBitmap tight;
  tight.dimensions = snapshot.dimensions;
  tight.rowBytes = tightRowBytes;
  tight.alphaType = snapshot.alphaType;
  tight.pixels.resize(tightRowBytes * static_cast<size_t>(snapshot.dimensions.y));
  for (int y = 0; y < snapshot.dimensions.y; ++y) {
    std::copy_n(snapshot.pixels.begin() + static_cast<std::ptrdiff_t>(y * snapshot.rowBytes),
                tightRowBytes,
                tight.pixels.begin() + static_cast<std::ptrdiff_t>(y * tightRowBytes));
  }
  return tight;
}

void ExpectSameBitmap(const RendererBitmap& actual, const RendererBitmap& expected,
                      const std::string& label) {
  ASSERT_FALSE(expected.empty());
  editor::tests::CompareBitmapToBitmap(actual, expected, label,
                                       editor::tests::PixelmatchIdentityParams());
}

/// Sprite frames inside a hidden container, the way generated sprite sheets keep their frames out
/// of the sheet's own rendering. `sprite-blur` blurs a group that holds a translucent group, and
/// `sprite-glow` filters a single shape.
constexpr std::string_view kSprites = R"svg(
  <g id="sprites" opacity="0">
    <g id="sprite-blur">
      <defs>
        <filter id="sheet-blur" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
          <feGaussianBlur stdDeviation="1"/>
        </filter>
      </defs>
      <g filter="url(#sheet-blur)">
        <rect x="2" y="2" width="12" height="12" fill="#d02020"/>
        <g opacity="0.5"><rect x="4" y="4" width="8" height="8" fill="#ffffff"/></g>
      </g>
    </g>
    <g id="sprite-glow">
      <defs>
        <filter id="sheet-glow" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
          <feFlood flood-color="#ffff00" flood-opacity="0.5"/>
          <feComposite in2="SourceAlpha" operator="in"/>
        </filter>
      </defs>
      <rect width="16" height="16" fill="#2050d0"/>
      <g opacity="0.5"><rect x="4" y="4" width="8" height="8" fill="#ffffff"/></g>
      <rect x="2" y="2" width="4" height="4" fill="#000000" filter="url(#sheet-glow)"/>
    </g>
  </g>)svg";

std::string Sheet() {
  return "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>" + std::string(kSprites) +
         "</svg>";
}

/// A 32x16 host: a backdrop, one sprite occurrence over columns 0..15, a blurred host-owned shape
/// over columns 16..23, then a translucent overlay over everything. Columns 24..31 only ever show
/// the backdrop under the overlay.
std::string Host(std::string_view href, bool inlineSprites) {
  return std::string(
             "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'>"
             "<defs><filter id='host-blur' filterUnits='userSpaceOnUse' x='16' y='0' width='8' "
             "height='16'><feGaussianBlur stdDeviation='1'/></filter></defs>") +
         (inlineSprites ? std::string(kSprites) : std::string()) +
         "<rect width='32' height='16' fill='#7748b5'/>"
         "<use id='sprite' href='" +
         std::string(href) +
         "'/>"
         "<rect x='18' y='2' width='4' height='12' fill='#20a020' filter='url(#host-blur)'/>"
         "<rect id='dim' width='32' height='16' fill='#000000' opacity='0.5'/>"
         "</svg>";
}

/// The overlay halves the backdrop wherever nothing else draws.
void ExpectDimmedBackdrop(const RendererBitmap& bitmap) {
  EXPECT_THAT(PixelAt(bitmap, 28, 8), test::Rgba(Lt(70), Lt(45), Lt(100), 255));
}

void ExpectSpriteMatchesInDocument(std::string_view sprite) {
  SVGDocument external = Parse(Host("sheet.svg#" + std::string(sprite), false),
                               {{"sheet.svg", Sheet()}});
  SVGDocument local = Parse(Host("#" + std::string(sprite), true));
  Renderer externalRenderer;
  Renderer localRenderer;
  const RendererBitmap actual = Draw(externalRenderer, external);
  const RendererBitmap expected = Draw(localRenderer, local);
  ExpectDimmedBackdrop(expected);
  ExpectDimmedBackdrop(actual);
  ExpectSameBitmap(actual, expected, "sub-document-filters-" + std::string(sprite));
}

TEST(SubDocumentFiltersTest, FilteredGroupInExternalTargetMatchesInDocument) {
  ExpectSpriteMatchesInDocument("sprite-blur");
}

TEST(SubDocumentFiltersTest, FilteredShapeInExternalTargetMatchesInDocument) {
  ExpectSpriteMatchesInDocument("sprite-glow");
}

/// An animated sprite retargets one occurrence between frames with different filters.
TEST(SubDocumentFiltersTest, RetargetedFilteredFramesMatchInDocument) {
  SVGDocument external = Parse(Host("sheet.svg#sprite-blur", false), {{"sheet.svg", Sheet()}});
  SVGDocument local = Parse(Host("#sprite-blur", true));
  Renderer externalRenderer;
  Renderer localRenderer;
  constexpr std::string_view kFrames[] = {"sprite-blur", "sprite-glow", "sprite-blur",
                                          "sprite-glow"};
  for (size_t frame = 0; frame < std::size(kFrames); ++frame) {
    SCOPED_TRACE(frame);
    external.querySelector("#sprite")->cast<SVGUseElement>().setHref(
        RcString("sheet.svg#" + std::string(kFrames[frame])));
    local.querySelector("#sprite")->cast<SVGUseElement>().setHref(
        RcString("#" + std::string(kFrames[frame])));
    const RendererBitmap actual = Draw(externalRenderer, external);
    const RendererBitmap expected = Draw(localRenderer, local);
    ExpectDimmedBackdrop(actual);
    ExpectSameBitmap(actual, expected, "sub-document-filters-frame-" + std::to_string(frame));
  }
}

/// A whole referenced document drawn inside a translucent host group. The group's layer must stay
/// open until its own last child, whatever entity ids the referenced document draws.
TEST(SubDocumentFiltersTest, WholeDocumentUseKeepsHostGroupLayerAndFilters) {
  std::string scene =
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>"
      "<defs><filter id='scene-blur' filterUnits='userSpaceOnUse' x='0' y='0' width='16' "
      "height='16'><feGaussianBlur stdDeviation='1'/></filter></defs>";
  for (int i = 0; i < 16; ++i) {
    scene += "<rect x='" + std::to_string(i) + "' y='0' width='1' height='4' fill='#2050d0'/>";
  }
  scene += "<rect x='2' y='6' width='12' height='8' fill='#d02020' filter='url(#scene-blur)'/>";
  const std::string sceneContent = scene.substr(scene.find('>') + 1);
  scene += "</svg>";

  const auto host = [](std::string_view content) {
    return "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'>"
           "<rect width='32' height='16' fill='#ffffff'/>"
           "<g opacity='0.5'>" +
           std::string(content) +
           "<rect x='20' y='4' width='8' height='8' fill='#000000'/>"
           "</g></svg>";
  };
  SVGDocument local = Parse(host("<svg width='16' height='16'>" + sceneContent + "</svg>"));
  Renderer localRenderer;
  const RendererBitmap expected = Draw(localRenderer, local);
  // The host's last child stays inside the group's half-opacity layer.
  EXPECT_THAT(PixelAt(expected, 24, 8), test::Rgba(Gt(100), Gt(100), Gt(100), 255));
  for (const std::string_view reference :
       {"<use href='scene.svg'/>", "<image href='scene.svg' width='16' height='16'/>"}) {
    SCOPED_TRACE(reference);
    SVGDocument external = Parse(host(reference), {{"scene.svg", scene}});
    Renderer externalRenderer;
    const RendererBitmap actual = Draw(externalRenderer, external);
    EXPECT_THAT(PixelAt(actual, 24, 8), test::Rgba(Gt(100), Gt(100), Gt(100), 255));
    ExpectSameBitmap(actual, expected, "sub-document-filters-whole-document");
  }
}

/// A host filter can pull a referenced document's off-surface content into view, so that content
/// must still be drawn into the filter's input. The content starts 8 units clear of the surface.
TEST(SubDocumentFiltersTest, ReferencedContentUnderHostFilterIsNotCulled) {
  const std::string scene =
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>"
      "<rect width='16' height='16' fill='#2050d0'/></svg>";
  const auto host = [](std::string_view content) {
    return "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'>"
           "<defs><filter id='shift' filterUnits='userSpaceOnUse' x='-24' y='0' width='56' "
           "height='16'><feOffset dx='24'/></filter></defs>"
           "<rect width='32' height='16' fill='#ffffff'/>"
           "<g filter='url(#shift)'>" +
           std::string(content) + "</g></svg>";
  };
  SVGDocument local = Parse(host("<svg x='-24' width='16' height='16'>"
                                 "<rect width='16' height='16' fill='#2050d0'/></svg>"));
  Renderer localRenderer;
  const RendererBitmap expected = Draw(localRenderer, local);
  EXPECT_THAT(PixelAt(expected, 8, 8), test::Rgba(Lt(60), Lt(100), Gt(180), 255));
  for (const std::string_view reference :
       {"<use href='scene.svg' x='-24'/>",
        "<image href='scene.svg' x='-24' width='16' height='16'/>"}) {
    SCOPED_TRACE(reference);
    SVGDocument external = Parse(host(reference), {{"scene.svg", scene}});
    Renderer externalRenderer;
    const RendererBitmap actual = Draw(externalRenderer, external);
    EXPECT_THAT(PixelAt(actual, 8, 8), test::Rgba(Lt(60), Lt(100), Gt(180), 255));
    ExpectSameBitmap(actual, expected, "sub-document-filters-host-filter");
  }
}

}  // namespace
}  // namespace donner::svg
