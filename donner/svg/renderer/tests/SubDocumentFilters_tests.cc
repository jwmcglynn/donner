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
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
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

using Documents = std::map<std::string, std::string, std::less<>>;

/// Settings that load @p documents for this document and, recursively, for the documents it
/// references, so a referenced document can reference another.
SVGDocument::Settings SettingsFor(
    const Documents& documents, std::shared_ptr<components::DocumentResourceFamilyBudget> family) {
  SVGDocument::Settings settings;
  settings.resourceLoader = std::make_unique<InMemoryLoader>(documents);
  settings.resourceFamilyBudget = family;
  settings.svgParseCallback = [documents, family](
                                  const std::vector<uint8_t>& bytes,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    auto child = parser::SVGParser::ParseSVG(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), warnings,
        parser::SVGParser::Options(), SettingsFor(documents, family));
    return child.hasError() ? std::nullopt : std::optional(child.result().handle());
  };
  return settings;
}

SVGDocument Parse(std::string_view source, Documents documents = {}) {
  SVGDocument::Settings settings =
      SettingsFor(documents, std::make_shared<components::DocumentResourceFamilyBudget>());
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
  SVGDocument external =
      Parse(Host("sheet.svg#" + std::string(sprite), false), {{"sheet.svg", Sheet()}});
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
  SVGDocument local =
      Parse(host("<svg x='-24' width='16' height='16'>"
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

/// A filter on a host group whose only content is external `<use>` occurrences resolves its default
/// object-bounding-box region from the content those occurrences draw.
TEST(SubDocumentFiltersTest, HostGroupFilterAroundExternalUseMatchesInDocument) {
  const auto host = [](std::string_view href, bool inlineSprites) {
    return std::string(
               "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'>"
               "<defs><filter id='tint' color-interpolation-filters='sRGB'>"
               "<feColorMatrix type='matrix' values='0 0 0 0 1  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0'/>"
               "</filter></defs>") +
           (inlineSprites ? std::string(kSprites) : std::string()) +
           "<rect width='32' height='16' fill='#ffffff'/>"
           "<g filter='url(#tint)'><use href='" +
           std::string(href) + "' x='8'/></g></svg>";
  };
  SVGDocument external = Parse(host("sheet.svg#sprite-glow", false), {{"sheet.svg", Sheet()}});
  SVGDocument local = Parse(host("#sprite-glow", true));
  Renderer externalRenderer;
  Renderer localRenderer;
  const RendererBitmap expected = Draw(localRenderer, local);
  // The tint paints the occurrence red.
  EXPECT_THAT(PixelAt(expected, 16, 12), test::Rgba(Gt(200), Lt(30), Lt(30), 255));
  ExpectSameBitmap(Draw(externalRenderer, external), expected, "sub-document-filters-host-tint");
}

/// Masks and objectBoundingBox clip paths on a host group around an external `<use>` resolve
/// against the content the occurrence draws, as they do for the same content in-document.
TEST(SubDocumentFiltersTest, HostMaskAndBoxClipAroundExternalUseMatchInDocument) {
  constexpr std::string_view kEffects[] = {
      "<defs><mask id='effect'><rect x='0' y='0' width='16' height='8' fill='#ffffff'/></mask>"
      "</defs>",
      "<defs><clipPath id='effect' clipPathUnits='objectBoundingBox'>"
      "<rect width='0.5' height='1'/></clipPath></defs>",
  };
  constexpr std::string_view kReferences[] = {"mask='url(#effect)'", "clip-path='url(#effect)'"};
  for (size_t i = 0; i < std::size(kEffects); ++i) {
    SCOPED_TRACE(kReferences[i]);
    const auto host = [&](std::string_view href, bool inlineSprites) {
      return "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'>" +
             std::string(kEffects[i]) + (inlineSprites ? std::string(kSprites) : std::string()) +
             "<rect width='32' height='16' fill='#ffffff'/><g " + std::string(kReferences[i]) +
             "><use href='" + std::string(href) + "' x='8'/></g></svg>";
    };
    SVGDocument external = Parse(host("sheet.svg#sprite-glow", false), {{"sheet.svg", Sheet()}});
    SVGDocument local = Parse(host("#sprite-glow", true));
    Renderer externalRenderer;
    Renderer localRenderer;
    const RendererBitmap expected = Draw(localRenderer, local);
    // The effect keeps the occurrence's top-left quarter.
    EXPECT_THAT(PixelAt(expected, 9, 7), test::Rgba(Lt(60), Lt(120), Gt(180), 255));
    ExpectSameBitmap(Draw(externalRenderer, external), expected,
                     "sub-document-host-effect-" + std::to_string(i));
  }
}

/// External-use work a frame admits for @p document, drawn through a driver that shares @p work.
std::size_t ExternalInstanceWork(SVGDocument& document,
                                 const std::shared_ptr<components::StyleResourceBudget>& work,
                                 RendererBitmap* bitmap = nullptr) {
  Renderer renderer;
  RendererDriver driver(renderer, false, nullptr, work);
  driver.draw(document);
  if (bitmap != nullptr) {
    *bitmap = renderer.takeSnapshot();
  }
  return work->externalInstanceWork();
}

/// Measuring external content for a host effect charges no external-use work: a frame with an
/// effect around many occurrences admits exactly the work drawing them admits, and draws them all.
TEST(SubDocumentFiltersTest, EffectsAroundManyExternalUsesChargeNoExtraWork) {
  std::string uses;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 8; ++x) {
      uses += "<use href='sheet.svg#sprite-glow' transform='translate(" + std::to_string(x * 4) +
              " " + std::to_string(y * 4) + ") scale(0.25)'/>";
    }
  }
  const auto host = [&](std::string_view open, std::string_view close) {
    return "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16'><defs>"
           "<filter id='user' filterUnits='userSpaceOnUse' x='0' y='0' width='32' height='16'>"
           "<feColorMatrix type='matrix' values='0 0 0 0 1  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0'/>"
           "</filter><filter id='box'>"
           "<feColorMatrix type='matrix' values='0 0 0 0 1  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0'/>"
           "</filter><clipPath id='half' clipPathUnits='objectBoundingBox'>"
           "<rect width='1' height='1'/></clipPath><mask id='all'>"
           "<rect width='32' height='16' fill='#ffffff'/></mask></defs>"
           "<rect width='32' height='16' fill='#ffffff'/>" +
           std::string(open) + uses + std::string(close) + "</svg>";
  };
  SVGDocument plain = Parse(host("<g>", "</g>"), {{"sheet.svg", Sheet()}});
  const std::size_t drawingWork =
      ExternalInstanceWork(plain, std::make_shared<components::StyleResourceBudget>());
  ASSERT_THAT(drawingWork, Gt(0u));

  constexpr std::string_view kEffects[] = {
      "<g filter='url(#user)'>",
      "<g filter='url(#box)'>",
      "<g filter='url(#box)' mask='url(#all)' clip-path='url(#half)'>",
  };
  for (const std::string_view effect : kEffects) {
    SCOPED_TRACE(effect);
    SVGDocument document = Parse(host(effect, "</g>"), {{"sheet.svg", Sheet()}});
    EXPECT_EQ(ExternalInstanceWork(document, std::make_shared<components::StyleResourceBudget>()),
              drawingWork);

    // Every occurrence draws, tinted red.
    RendererBitmap bitmap;
    ExternalInstanceWork(document, std::make_shared<components::StyleResourceBudget>(), &bitmap);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 8; ++x) {
        SCOPED_TRACE(testing::Message() << "occurrence " << x << "," << y);
        EXPECT_THAT(PixelAt(bitmap, x * 4 + 1, y * 4 + 1),
                    test::Rgba(Gt(180), Lt(80), Lt(80), 255));
      }
    }
  }
}

/// Compares @p host drawn with `{ref}` naming fragments of an external sheet holding @p sheetDefs
/// with the same host drawing the same definitions in-document.
void ExpectExternalMatchesInDocument(const std::string& label, std::string_view sheetDefs,
                                     std::string_view host, Documents extraDocuments = {}) {
  const auto withReference = [&](std::string_view prefix) {
    std::string result(host);
    for (size_t at = result.find("{ref}"); at != std::string::npos; at = result.find("{ref}")) {
      result.replace(at, 5, prefix);
    }
    return result;
  };
  const std::string head = "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>";
  Documents documents = std::move(extraDocuments);
  documents["sheet.svg"] = head + "<defs>" + std::string(sheetDefs) + "</defs></svg>";
  SVGDocument external = Parse(head + withReference("sheet.svg#") + "</svg>", documents);
  SVGDocument local =
      Parse(head + "<defs>" + std::string(sheetDefs) + "</defs>" + withReference("#") + "</svg>",
            documents);
  Renderer externalRenderer;
  Renderer localRenderer;
  ExpectSameBitmap(Draw(externalRenderer, external), Draw(localRenderer, local), label);
}

constexpr std::string_view kTint =
    "<filter id='tint'><feColorMatrix type='matrix' "
    "values='0 0 0 0 1  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0'/></filter>"
    "<clipPath id='left' clipPathUnits='objectBoundingBox'><rect width='0.5' height='1'/>"
    "</clipPath><mask id='top'><rect width='32' height='12' fill='#ffffff'/></mask>";

TEST(SubDocumentFiltersTest, BoxUnitsFilterInsideReferencedDocument) {
  ExpectExternalMatchesInDocument(
      "sub-document-box-units-inside",
      "<filter id='shift' primitiveUnits='objectBoundingBox'><feOffset dx='0.25'/></filter>"
      "<g id='sprite' filter='url(#shift)'><rect x='4' y='4' width='16' height='16' "
      "fill='#2050d0'/></g>",
      "<rect width='32' height='32' fill='#ffffff'/><use href='{ref}sprite'/>");
}

TEST(SubDocumentFiltersTest, FeImageInsideReferencedDocument) {
  ExpectExternalMatchesInDocument(
      "sub-document-feimage-inside",
      "<rect id='picture' width='8' height='8' fill='#d02020'/>"
      "<filter id='paste' filterUnits='userSpaceOnUse' x='0' y='0' width='32' height='32'>"
      "<feImage href='#picture'/></filter>"
      "<rect id='sprite' x='4' y='4' width='16' height='16' fill='#2050d0' filter='url(#paste)'/>",
      "<rect width='32' height='32' fill='#ffffff'/><use href='{ref}sprite'/>");
}

TEST(SubDocumentFiltersTest, ScaledRotatedUseUnderBoxEffects) {
  ExpectExternalMatchesInDocument(
      "sub-document-scaled-rotated", "<rect id='sprite' width='8' height='6' fill='#2050d0'/>",
      "<defs>" + std::string(kTint) +
          "</defs><rect width='32' height='32' fill='#ffffff'/>"
          "<g filter='url(#tint)' clip-path='url(#left)' mask='url(#top)'>"
          "<use href='{ref}sprite' transform='translate(16 4) rotate(30) scale(1.5)'/></g>");
}

TEST(SubDocumentFiltersTest, SymbolTargetViewportUnderBoxClip) {
  ExpectExternalMatchesInDocument(
      "sub-document-symbol-viewport",
      "<symbol id='sprite' viewBox='0 0 10 10'><rect width='10' height='10' fill='#2050d0'/>"
      "</symbol>",
      "<defs>" + std::string(kTint) +
          "</defs><rect width='32' height='32' fill='#ffffff'/>"
          "<g clip-path='url(#left)'><use href='{ref}sprite' x='2' y='4' width='24' "
          "height='12'/></g>");
}

/// A referenced document that itself references a third: its filters and its own external content
/// draw, and a host box effect covers all of it, as the same three levels do in one document.
TEST(SubDocumentFiltersTest, NestedExternalUsesUnderBoxEffects) {
  const std::string leafDefs =
      "<filter id='leaf-glow' filterUnits='userSpaceOnUse' x='0' y='0' width='32' height='32'>"
      "<feGaussianBlur stdDeviation='1'/></filter>"
      "<rect id='leaf' x='12' y='12' width='12' height='12' fill='#20a020' "
      "filter='url(#leaf-glow)'/>";
  const auto middle = [](std::string_view leafReference) {
    return "<g id='middle'><rect x='2' y='2' width='8' height='8' fill='#2050d0'/><use href='" +
           std::string(leafReference) + "'/></g>";
  };
  const std::string head = "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>";
  const auto host = [&](std::string_view defs, std::string_view middleReference) {
    return head + "<defs>" + std::string(kTint) + std::string(defs) +
           "</defs><rect width='32' height='32' fill='#ffffff'/>"
           "<g clip-path='url(#left)' filter='url(#tint)'><use href='" +
           std::string(middleReference) + "'/></g></svg>";
  };
  const Documents documents = {
      {"sheet.svg", head + "<defs>" + middle("leaf.svg#leaf") + "</defs></svg>"},
      {"leaf.svg", head + "<defs>" + leafDefs + "</defs></svg>"},
  };
  SVGDocument external = Parse(host("", "sheet.svg#middle"), documents);
  SVGDocument local = Parse(host(leafDefs + middle("#leaf"), "#middle"));
  Renderer externalRenderer;
  Renderer localRenderer;
  const RendererBitmap expected = Draw(localRenderer, local);
  // The box spans both levels, so its left half keeps part of the leaf, tinted red.
  EXPECT_THAT(PixelAt(expected, 12, 18), test::Rgba(Gt(120), Lt(80), Lt(80), 255));
  ExpectSameBitmap(Draw(externalRenderer, external), expected, "sub-document-nested");
}

/// Measuring an occurrence in the filter pre-pass, while the referenced document's render tree was
/// last built for another occurrence of the same document, lays the measured one out first. The
/// other occurrence draws before and after the filtered one, so every frame ends on it; each frame
/// matches a fresh render, for different targets and for one symbol drawn into two viewports.
TEST(SubDocumentFiltersTest, MeasuredOccurrenceAfterAnotherMatchesFreshRender) {
  const std::string head = "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>";
  const Documents documents = {
      {"sheet.svg", head + "<defs><rect id='small' width='4' height='4' fill='#2050d0'/>"
                           "<rect id='big' width='24' height='12' fill='#20a020'/>"
                           "<symbol id='symbol' viewBox='0 0 10 10'><rect width='10' height='10' "
                           "fill='#d0a020'/></symbol></defs></svg>"},
  };
  struct Occurrences {
    std::string measured;  ///< The occurrence the host filter measures.
    std::string other;     ///< Another occurrence of the same document, drawn around it.
  };
  const Occurrences cases[] = {
      {"<use href='sheet.svg#big' y='16'/>", "<use href='sheet.svg#small' x='26' y='2'/>"},
      {"<use href='sheet.svg#symbol' y='16' width='24' height='12'/>",
       "<use href='sheet.svg#symbol' x='24' width='6' height='6'/>"},
  };
  constexpr std::string_view kBoxFlood =
      "<filter id='box'><feFlood flood-color='#d02020' flood-opacity='0.5'/>"
      "<feMerge><feMergeNode/><feMergeNode in='SourceGraphic'/></feMerge></filter>";
  for (const auto& [measured, other] : cases) {
    SCOPED_TRACE(measured);
    const std::string source = head + "<defs>" + std::string(kBoxFlood) +
                               "</defs><rect width='32' height='32' fill='#ffffff'/>" + other +
                               "<g filter='url(#box)'>" + measured + "</g>" + other + "</svg>";
    SVGDocument document = Parse(source, documents);
    Renderer renderer;
    for (int frame = 0; frame < 4; ++frame) {
      SCOPED_TRACE(frame);
      SVGDocument fresh = Parse(source, documents);
      Renderer freshRenderer;
      ExpectSameBitmap(Draw(renderer, document), Draw(freshRenderer, fresh),
                       "sub-document-measured-after-other");
    }
  }
}

}  // namespace
}  // namespace donner::svg
