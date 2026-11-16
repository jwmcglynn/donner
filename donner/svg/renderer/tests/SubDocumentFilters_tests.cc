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
#include "donner/base/Vector2.h"
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

/// The renderer's current frame with tightly packed rows.
RendererBitmap TightSnapshot(Renderer& renderer) {
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

RendererBitmap Draw(Renderer& renderer, SVGDocument& document) {
  renderer.draw(document);
  return TightSnapshot(renderer);
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
  editor::tests::CompareBitmapToBitmap(actual, expected,
                                       "sub-document-filters-" + std::string(sprite),
                                       editor::tests::PixelmatchIdentityParams());
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
    editor::tests::CompareBitmapToBitmap(actual, expected,
                                         "sub-document-filters-frame-" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
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
    editor::tests::CompareBitmapToBitmap(actual, expected, "sub-document-filters-whole-document",
                                         editor::tests::PixelmatchIdentityParams());
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
    editor::tests::CompareBitmapToBitmap(actual, expected, "sub-document-filters-host-filter",
                                         editor::tests::PixelmatchIdentityParams());
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
  editor::tests::CompareBitmapToBitmap(Draw(externalRenderer, external), expected,
                                       "sub-document-filters-host-tint",
                                       editor::tests::PixelmatchIdentityParams());
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
    editor::tests::CompareBitmapToBitmap(Draw(externalRenderer, external), expected,
                                         "sub-document-host-effect-" + std::to_string(i),
                                         editor::tests::PixelmatchIdentityParams());
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
  editor::tests::CompareBitmapToBitmap(Draw(externalRenderer, external), Draw(localRenderer, local),
                                       label, editor::tests::PixelmatchIdentityParams());
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
      "<rect width='32' height='32' fill='#ffffff'/>"
      "<use href='{ref}sprite' transform='translate(2 1) scale(1.25)'/>");
}

TEST(SubDocumentFiltersTest, ScaledRotatedUseUnderBoxEffects) {
  ExpectExternalMatchesInDocument(
      "sub-document-scaled-rotated", "<rect id='sprite' width='8' height='6' fill='#2050d0'/>",
      "<defs>" + std::string(kTint) +
          "</defs><rect width='32' height='32' fill='#ffffff'/>"
          "<g filter='url(#tint)' clip-path='url(#left)' mask='url(#top)'>"
          "<use href='{ref}sprite' transform='translate(16 4) rotate(30) scale(1.5)'/></g>");
}

/// Box effects on a host group keep the left quarter of the bounding box of the external content in
/// the group's space. That box bounds the content's geometry through every transform between the
/// two, not the box of its own box, so transforms that cancel leave the content's own size.
TEST(SubDocumentFiltersTest, TransformedContentUnderBoxEffectsMatchesInDocument) {
  constexpr std::string_view kEffects[] = {
      "<clipPath id='quarter' clipPathUnits='objectBoundingBox'>"
      "<rect width='0.25' height='1'/></clipPath>",
      "<mask id='quarter' maskContentUnits='objectBoundingBox'>"
      "<rect width='0.25' height='1' fill='#ffffff'/></mask>",
      "<filter id='quarter' x='0' y='0' width='0.25' height='1'><feOffset/></filter>",
  };
  constexpr std::string_view kReferences[] = {"clip-path", "mask", "filter"};
  struct Placement {
    std::string_view label;
    std::string_view sprite;
    std::string_view useTransform;
  };
  constexpr std::string_view kSquare = "x='-5' y='-5' width='10' height='10' fill='#2050d0'";
  const std::string square(kSquare);
  const std::string rootRotated = "<rect id='sprite' " + square + " transform='rotate(45)'/>";
  const std::string nestedRotated = "<g id='sprite'><g transform='rotate(30)'><rect " + square +
                                    " transform='rotate(15)'/></g></g>";
  const std::string skewed = "<g id='sprite'><rect " + square + " transform='skewX(30)'/></g>";
  const std::string rotated = "<g id='sprite'><rect " + square + " transform='rotate(30)'/></g>";
  const Placement kPlacements[] = {
      {"cancelling-root-rotation", rootRotated, "rotate(-45)"},
      {"cancelling-nested-rotation", nestedRotated, "rotate(-45)"},
      {"cancelling-skew", skewed, "skewX(-30)"},
      {"adding-rotation", rotated, "rotate(30)"},
  };
  for (size_t effect = 0; effect < std::size(kEffects); ++effect) {
    for (const Placement& placement : kPlacements) {
      SCOPED_TRACE(std::string(kReferences[effect]) + " " + std::string(placement.label));
      ExpectExternalMatchesInDocument(
          "sub-document-transformed-" + std::string(kReferences[effect]) + "-" +
              std::string(placement.label),
          placement.sprite,
          "<defs>" + std::string(kEffects[effect]) +
              "</defs><rect width='32' height='32' fill='#ffffff'/><g " +
              std::string(kReferences[effect]) + "='url(#quarter)'><use href='{ref}sprite' " +
              "transform='translate(16 16) " + std::string(placement.useTransform) + "'/></g>");
    }
  }
}

/// A text's object box is mapped through every transform between the text and the effect owner at
/// once, so rotations that cancel leave the text's own box, for an external text as in-document.
TEST(SubDocumentFiltersTest, TransformedTextUnderBoxClipMatchesInDocument) {
  constexpr std::string_view kText =
      "x='-8' y='4' font-size='12' fill='#2050d0' transform='rotate(45)'>Hi</text>";
  const std::string sprites[] = {"<text id='sprite' " + std::string(kText),
                                 "<g id='sprite'><text " + std::string(kText) + "</g>"};
  for (size_t i = 0; i < std::size(sprites); ++i) {
    SCOPED_TRACE(sprites[i]);
    ExpectExternalMatchesInDocument(
        "sub-document-transformed-text-" + std::to_string(i), sprites[i],
        "<defs><clipPath id='quarter' clipPathUnits='objectBoundingBox'>"
        "<rect width='0.25' height='1'/></clipPath></defs>"
        "<rect width='32' height='32' fill='#ffffff'/><g clip-path='url(#quarter)'>"
        "<use href='{ref}sprite' transform='translate(16 16) rotate(-45)'/></g>");
  }
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
  editor::tests::CompareBitmapToBitmap(Draw(externalRenderer, external), expected,
                                       "sub-document-nested",
                                       editor::tests::PixelmatchIdentityParams());
}

/// An feImage that draws an external `<use>` keeps its offscreen copy of that use out of the box of
/// an unrelated element earlier in the document, on every frame. The offscreen copy numbers its
/// own draw order from zero, so its `<use>` could land inside the early group's draw-order range.
TEST(SubDocumentFiltersTest, FeImageExternalContentStaysOutOfOtherBoxes) {
  const std::string head = "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>";
  const std::string farDefs = "<rect id='far' x='22' y='22' width='8' height='8' fill='#20a020'/>";
  const auto host = [&](std::string_view defs, std::string_view farReference) {
    return head + "<defs>" + std::string(defs) +
           "<filter id='mark'><feFlood flood-color='#d02020' flood-opacity='0.5'/></filter>"
           "<filter id='paste' filterUnits='userSpaceOnUse' x='16' y='0' width='16' height='16'>"
           "<feImage href='#frag'/></filter><g id='frag'><rect width='1' height='1' "
           "fill='none'/><rect width='1' height='1' fill='none'/><use href='" +
           std::string(farReference) +
           "' transform='translate(-4 -20)'/></g></defs>"
           "<rect width='32' height='32' fill='#ffffff'/>"
           "<g filter='url(#mark)'><rect x='2' y='2' width='6' height='6' fill='#2050d0'/></g>"

           "<rect x='16' y='0' width='16' height='16' fill='#000000' filter='url(#paste)'/></svg>";
  };
  SVGDocument external = Parse(host("", "sheet.svg#far"),
                               {{"sheet.svg", head + "<defs>" + farDefs + "</defs></svg>"}});
  SVGDocument local = Parse(host(farDefs, "#far"));
  Renderer externalRenderer;
  Renderer localRenderer;
  for (int frame = 0; frame < 3; ++frame) {
    SCOPED_TRACE(frame);
    const RendererBitmap expected = Draw(localRenderer, local);
    // The flood covers only the small group's box, which ends above y = 9.
    EXPECT_THAT(PixelAt(expected, 4, 14), test::Rgba(255, 255, 255, 255));
    const RendererBitmap actual = Draw(externalRenderer, external);
    // Compare the left half, where the early group draws. The feImage paste on the right does not
    // yet draw external content from a fragment in <defs>.
    const auto leftHalf = [](const RendererBitmap& bitmap) {
      return editor::tests::CropBitmap(bitmap, Vector2i(0, 0), Vector2i(16, 32));
    };
    editor::tests::CompareBitmapToBitmap(
        leftHalf(actual), leftHalf(expected),
        "feimage_external_left_half_frame_" + std::to_string(frame),
        editor::tests::PixelmatchIdentityParams());
  }
}

/// Measuring recurses into each occurrence's own external uses, so a family of documents that each
/// use the next twice measures exponentially many occurrences. Measurement is charged to its own
/// frame allowance, which stops it at that allowance.
TEST(SubDocumentFiltersTest, BranchingExternalUseMeasurementStaysWithinItsAllowance) {
  constexpr int kDepth = 8;
  Documents documents;
  const std::string head = "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>";
  for (int level = 0; level < kDepth; ++level) {
    const std::string next = "d" + std::to_string(level + 1) + ".svg#f";
    documents["d" + std::to_string(level) + ".svg"] = head + "<defs><g id='f'><use href='" + next +
                                                      "'/><use href='" + next +
                                                      "' x='1'/></g></defs></svg>";
  }
  documents["d" + std::to_string(kDepth) + ".svg"] =
      head + "<defs><g id='f'><rect width='1' height='1' fill='#2050d0'/></g></defs></svg>";
  const auto host = [&](std::string_view open) {
    return head +
           "<defs><filter id='box'><feColorMatrix type='matrix' "
           "values='0 0 0 0 1  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0'/></filter></defs>" +
           std::string(open) + "<use href='d0.svg#f'/></g></svg>";
  };
  components::StyleResourceBudget::Limits limits;
  limits.ruleElementMatches = 64;
  limits.externalMeasureEntities = 64;
  SVGDocument filtered = Parse(host("<g filter='url(#box)'>"), documents);
  const auto work = std::make_shared<components::StyleResourceBudget>(limits);
  ExternalInstanceWork(filtered, work);
  EXPECT_THAT(work->externalMeasureWork(),
              testing::AllOf(Gt(0u), testing::Le(limits.externalMeasureEntities)));
}

/// Nested box-effect owners in different spaces measure each external occurrence once per frame,
/// so they charge the measurement allowance what a single owner does. At an allowance that admits
/// exactly that charge, both owners still bound all of the content, as they do in-document.
TEST(SubDocumentFiltersTest, NestedBoxEffectOwnersMeasureEachExternalUseOnce) {
  const std::string sprite =
      "<rect id='sprite' x='-2' y='-2' width='4' height='4' fill='#2050d0' "
      "transform='rotate(30)'/>";
  std::string uses;
  for (int x = 0; x < 4; ++x) {
    uses += "<use href='{ref}sprite' transform='translate(" + std::to_string(4 + x * 8) +
            " 16) rotate(-30)'/>";
  }
  const auto host = [&](std::string_view prefix, std::string_view owners, std::string_view closers,
                        bool inlineSprite) {
    std::string content =
        "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><defs>"
        "<filter id='half' x='0' y='0' width='0.5' height='1'><feOffset/>"
        "</filter><clipPath id='most' clipPathUnits='objectBoundingBox'>"
        "<rect width='0.75' height='1'/></clipPath>" +
        (inlineSprite ? sprite : std::string()) +
        "</defs><rect width='32' height='32' fill='#ffffff'/>" + std::string(owners) + uses +
        std::string(closers) + "</svg>";
    for (size_t at = content.find("{ref}"); at != std::string::npos; at = content.find("{ref}")) {
      content.replace(at, 5, prefix);
    }
    return content;
  };
  const Documents documents = {
      {"sheet.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><defs>" +
                        sprite + "</defs></svg>"}};
  constexpr std::string_view kSingle = "<g filter='url(#half)'>";
  constexpr std::string_view kNested =
      "<g filter='url(#half)'><g clip-path='url(#most)' transform='translate(0 2)'>";
  const auto measureWork = [&](std::string_view owners, std::string_view closers,
                               components::StyleResourceBudget::Limits limits,
                               RendererBitmap* bitmap) {
    SVGDocument document = Parse(host("sheet.svg#", owners, closers, false), documents);
    const auto work = std::make_shared<components::StyleResourceBudget>(limits);
    Renderer renderer;
    RendererDriver driver(renderer, false, nullptr, work);
    driver.draw(document);
    *bitmap = TightSnapshot(renderer);
    return work->externalMeasureWork();
  };

  RendererBitmap single;
  const std::size_t singleWork =
      measureWork(kSingle, "</g>", components::StyleResourceBudget::Limits(), &single);
  ASSERT_THAT(singleWork, Gt(0u));
  RendererBitmap nested;
  EXPECT_THAT(measureWork(kNested, "</g></g>", components::StyleResourceBudget::Limits(), &nested),
              testing::Eq(singleWork));

  components::StyleResourceBudget::Limits boundary;
  boundary.externalMeasureEntities = singleWork;
  const struct {
    std::string_view label;
    std::string_view owners;
    std::string_view closers;
  } kScenes[] = {{"single", kSingle, "</g>"}, {"nested", kNested, "</g></g>"}};
  for (const auto& scene : kScenes) {
    SCOPED_TRACE(scene.label);
    SVGDocument local = Parse(host("#", scene.owners, scene.closers, true));
    Renderer localRenderer;
    RendererBitmap actual;
    EXPECT_THAT(measureWork(scene.owners, scene.closers, boundary, &actual),
                testing::Eq(singleWork));
    editor::tests::CompareBitmapToBitmap(actual, Draw(localRenderer, local),
                                         "sub-document-nested-owners-" + std::string(scene.label),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A frame of @p document drawn through a driver whose external-use work is charged to a budget
/// with @p limits, which is returned. @p bitmap receives the frame.
std::shared_ptr<components::StyleResourceBudget> DrawWithBudget(
    SVGDocument& document, const components::StyleResourceBudget::Limits& limits,
    RendererBitmap* bitmap) {
  const auto work = std::make_shared<components::StyleResourceBudget>(limits);
  Renderer renderer;
  RendererDriver driver(renderer, false, nullptr, work);
  driver.draw(document);
  *bitmap = TightSnapshot(renderer);
  return work;
}

/// A host drawing @p uses occurrences of a path of thousands of segments inside a box filter that
/// keeps the left half of their box, with `{ref}` naming the path's document.
std::string LargePathHost(int uses) {
  std::string host =
      "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><defs>"
      "<filter id='half' x='0' y='0' width='0.5' height='1'><feOffset/></filter></defs>"
      "<rect width='32' height='32' fill='#ffffff'/><g filter='url(#half)'>";
  for (int i = 0; i < uses; ++i) {
    host += "<use href='{ref}p' transform='translate(" + std::to_string(i % 4) + " " +
            std::to_string(i / 4) + ")'/>";
  }
  return host + "</g></svg>";
}

/// A document holding the path @ref LargePathHost draws, zigzagging across x = 2..22.
std::string LargePathDefs() {
  std::string d = "M2 2";
  constexpr int kSegments = 4000;
  for (int i = 1; i <= kSegments; ++i) {
    d += " L" + std::to_string(i % 2 == 0 ? 2 : 22) + " " + std::to_string(2 + 20 * i / kSegments);
  }
  return "<path id='p' fill='#2050d0' d='" + d + "'/>";
}

/// @p host with every `{ref}` replaced by @p prefix.
std::string WithReference(std::string host, std::string_view prefix) {
  for (size_t at = host.find("{ref}"); at != std::string::npos; at = host.find("{ref}")) {
    host.replace(at, 5, prefix);
  }
  return host;
}

/// Measuring many occurrences of one large external path for a host effect retains one copy of
/// it for the frame, which every occurrence's geometry shares.
TEST(SubDocumentFiltersTest, ManyUsesOfOnePathRetainOneCopy) {
  const Documents documents = {
      {"sheet.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><defs>" +
                        LargePathDefs() + "</defs></svg>"}};
  const auto retainedBytes = [&](int uses) {
    SVGDocument document = Parse(WithReference(LargePathHost(uses), "sheet.svg#"), documents);
    RendererBitmap bitmap;
    return DrawWithBudget(document, components::StyleResourceBudget::Limits(), &bitmap)
        ->externalMeasureGeometryBytes();
  };
  const std::size_t oneCopy = retainedBytes(1);
  ASSERT_THAT(oneCopy, Gt(4000u * sizeof(Vector2d)));
  EXPECT_THAT(retainedBytes(32), testing::Eq(oneCopy));
}

/// At a geometry allowance that admits one copy of the path the copy is retained, and below it the
/// path's box stands in for it: either way the effect still bounds every occurrence, as in-document.
TEST(SubDocumentFiltersTest, GeometryAllowanceFallsBackToThePathBox) {
  const std::string head = "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>";
  const Documents documents = {
      {"sheet.svg", head + "<defs>" + LargePathDefs() + "</defs></svg>"}};
  const std::string host = LargePathHost(4);
  SVGDocument sized = Parse(WithReference(host, "sheet.svg#"), documents);
  RendererBitmap unused;
  const std::size_t oneCopy =
      DrawWithBudget(sized, components::StyleResourceBudget::Limits(), &unused)
          ->externalMeasureGeometryBytes();
  ASSERT_THAT(oneCopy, Gt(0u));

  SVGDocument local = Parse(head + "<defs>" + LargePathDefs() + "</defs>" +
                            WithReference(host, "#").substr(head.size()));
  Renderer localRenderer;
  const RendererBitmap expected = Draw(localRenderer, local);
  for (const std::size_t allowance : {oneCopy, oneCopy - 1}) {
    SCOPED_TRACE(testing::Message() << "allowance " << allowance);
    components::StyleResourceBudget::Limits limits;
    limits.externalMeasureGeometryBytes = allowance;
    SVGDocument external = Parse(WithReference(host, "sheet.svg#"), documents);
    RendererBitmap actual;
    EXPECT_THAT(DrawWithBudget(external, limits, &actual)->externalMeasureGeometryBytes(),
                testing::Eq(allowance == oneCopy ? oneCopy : 0u));
    editor::tests::CompareBitmapToBitmap(
        actual, expected, "sub-document-geometry-allowance-" + std::to_string(allowance),
        editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
