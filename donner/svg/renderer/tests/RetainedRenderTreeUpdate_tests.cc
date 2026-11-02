/// @file
/// Retained render-tree updates against the full rebuild: each DOM edit sequence is applied to two
/// copies of a document, one allowed to keep its computed components between frames and one forced
/// through the full rebuild, and every frame of the two must be pixel-identical.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"
#include "donner/svg/renderer/tests/SpriteFieldScene.h"

namespace donner::svg::tests {
namespace {

using components::RenderTreeState;

constexpr int kFrames = 4;

/// Which preparation path the edited frames of the retained copy are expected to take.
enum class ExpectedPath {
  Retained,     //!< Every edited frame keeps the computed components.
  FullRebuild,  //!< Every edited frame needs the full rebuild.
};

/// A DOM edit applied identically to both copies before frame \p frame is drawn.
using FrameEdit = std::function<void(SVGDocument& document, int frame)>;

RenderTreeState::Counters ReadCounters(const SVGDocument& document) {
  const DocumentReadAccess access = document.readAccess();
  const auto* state = document.unsafeRegistry().ctx().find<RenderTreeState>();
  return state != nullptr ? state->counters : RenderTreeState::Counters();
}

void DisableRetainedUpdates(SVGDocument& document) {
  const DocumentWriteAccess access = document.writeAccess();
  Registry& registry = document.unsafeRegistry();
  if (!registry.ctx().contains<RenderTreeState>()) {
    registry.ctx().emplace<RenderTreeState>();
  }
  registry.ctx().get<RenderTreeState>().retainedUpdatesEnabled = false;
}

/// Draws through \ref RendererDriver, which takes its own document access, so `ConcurrentDom`
/// documents can be drawn from the test thread.
class DriverRenderer {
public:
  void draw(SVGDocument& document) { RendererDriver(backend_).draw(document); }
  RendererBitmap takeSnapshot() const { return backend_.takeSnapshot(); }

private:
  RendererTinySkia backend_;
};

SVGDocument Parse(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  return std::move(parsed.result());
}

SVGGraphicsElement Element(SVGDocument& document, std::string_view selector) {
  return document.querySelector(selector)->cast<SVGGraphicsElement>();
}

/// Draws \p retained and \p reference through \ref kFrames edited frames, comparing every frame
/// and checking which path the retained copy took.
template <typename DrawingRenderer = Renderer>
void ExpectFramesMatchFullRebuild(SVGDocument retained, SVGDocument reference,
                                  const FrameEdit& edit, ExpectedPath expected,
                                  std::string_view label) {
  DisableRetainedUpdates(reference);
  DrawingRenderer retainedRenderer;
  DrawingRenderer referenceRenderer;
  retainedRenderer.draw(retained);
  referenceRenderer.draw(reference);

  const RenderTreeState::Counters before = ReadCounters(retained);
  for (int frame = 1; frame <= kFrames; ++frame) {
    edit(retained, frame);
    edit(reference, frame);
    retainedRenderer.draw(retained);
    referenceRenderer.draw(reference);
    editor::tests::CompareBitmapToBitmap(retainedRenderer.takeSnapshot(),
                                         referenceRenderer.takeSnapshot(),
                                         std::string(label) + "-frame-" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }

  const RenderTreeState::Counters after = ReadCounters(retained);
  const uint64_t retainedFrames = after.retainedUpdates - before.retainedUpdates;
  const uint64_t fullFrames = after.fullRebuilds - before.fullRebuilds;
  if (expected == ExpectedPath::Retained) {
    EXPECT_EQ(retainedFrames, kFrames) << label;
    EXPECT_EQ(fullFrames, 0u) << label;
  } else {
    EXPECT_EQ(retainedFrames, 0u) << label;
    EXPECT_EQ(fullFrames, kFrames) << label;
  }
  EXPECT_EQ(ReadCounters(reference).retainedUpdates, 0u) << label;
}

/// Same-document scene: \p edit is applied to two parses of \p source.
void ExpectSourceFramesMatchFullRebuild(std::string_view source, const FrameEdit& edit,
                                        ExpectedPath expected, std::string_view label) {
  ExpectFramesMatchFullRebuild(Parse(source), Parse(source), edit, expected, label);
}

FrameEdit MoveElement(std::string selector) {
  return [selector](SVGDocument& document, int frame) {
    Element(document, selector)
        .setTransform(Transform2d::Rotate(0.1 * frame) *
                      Transform2d::Translate(Vector2d(3.0 * frame, 2.0 * frame)));
  };
}

FrameEdit SetOpacity(std::string selector) {
  return [selector](SVGDocument& document, int frame) {
    Element(document, selector)
        .setAttribute(xml::XMLQualifiedNameRef("opacity"), std::to_string(1.0 - 0.2 * frame));
  };
}

FrameEdit Both(FrameEdit first, FrameEdit second) {
  return [first = std::move(first), second = std::move(second)](SVGDocument& document, int frame) {
    first(document, frame);
    second(document, frame);
  };
}

constexpr std::string_view kNestedScene = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
    <defs>
      <rect id="tile" width="10" height="10" fill="teal"/>
      <g id="pair"><use href="#tile"/><use href="#tile" x="12" fill="red"/></g>
      <symbol id="badge" viewBox="0 0 10 10"><circle cx="5" cy="5" r="4" fill="gold"/></symbol>
    </defs>
    <g id="parent" transform="translate(4 4)">
      <g id="child" transform="rotate(10)">
        <circle cx="20" cy="20" r="8" fill="orange"/>
        <use id="outer" href="#pair" x="30" y="5"/>
        <use id="symbol-use" href="#badge" x="10" y="50" width="30" height="20"/>
      </g>
    </g>
  </svg>)svg";

TEST(RetainedRenderTreeUpdateTest, AncestorTransformsMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kNestedScene,
                                     Both(MoveElement("#parent"), MoveElement("#child")),
                                     ExpectedPath::Retained, "ancestor-transform");
}

TEST(RetainedRenderTreeUpdateTest, NestedUsesMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kNestedScene, MoveElement("#outer"), ExpectedPath::Retained,
                                     "nested-use");
}

TEST(RetainedRenderTreeUpdateTest, SymbolViewportsMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kNestedScene, MoveElement("#symbol-use"),
                                     ExpectedPath::Retained, "symbol-use");
}

TEST(RetainedRenderTreeUpdateTest, MovingAClonedSourceTakesTheFullRebuild) {
  // Shadow trees copy their source's transform when they are created.
  ExpectSourceFramesMatchFullRebuild(kNestedScene, MoveElement("#tile"), ExpectedPath::FullRebuild,
                                     "cloned-source");
}

constexpr std::string_view kClipMaskScene = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
    <defs>
      <clipPath id="user-clip"><circle id="clip-circle" cx="30" cy="30" r="18"/></clipPath>
      <clipPath id="box-clip" clipPathUnits="objectBoundingBox">
        <rect x="0.1" y="0.1" width="0.6" height="0.8"/>
      </clipPath>
      <mask id="fade"><rect width="96" height="96" fill="white" opacity="0.5"/>
        <circle cx="48" cy="48" r="20" fill="white"/></mask>
      <mask id="box-fade" maskContentUnits="objectBoundingBox">
        <rect width="0.5" height="1" fill="white"/></mask>
    </defs>
    <g id="group" transform="translate(2 2)">
      <rect id="user-clipped" width="60" height="60" fill="navy" clip-path="url(#user-clip)"/>
      <rect id="box-clipped" x="40" y="40" width="40" height="30" fill="green"
            clip-path="url(#box-clip)"/>
      <g id="masked" mask="url(#fade)"><rect x="10" y="50" width="70" height="30" fill="purple"/></g>
      <rect id="box-masked" x="60" y="10" width="30" height="30" fill="maroon"
            mask="url(#box-fade)"/>
    </g>
  </svg>)svg";

TEST(RetainedRenderTreeUpdateTest, ClipPathsMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(
      kClipMaskScene, Both(MoveElement("#user-clipped"), MoveElement("#box-clipped")),
      ExpectedPath::Retained, "clip-path");
}

TEST(RetainedRenderTreeUpdateTest, MasksMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kClipMaskScene,
                                     Both(MoveElement("#masked"), MoveElement("#box-masked")),
                                     ExpectedPath::Retained, "mask");
}

TEST(RetainedRenderTreeUpdateTest, ClippedAndMaskedAncestorsMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kClipMaskScene, MoveElement("#group"), ExpectedPath::Retained,
                                     "clip-mask-ancestor");
}

TEST(RetainedRenderTreeUpdateTest, MovingClipContentTakesTheFullRebuild) {
  // Clip content is not drawn on its own, so the retained update leaves it to the full rebuild.
  ExpectSourceFramesMatchFullRebuild(kClipMaskScene, MoveElement("#clip-circle"),
                                     ExpectedPath::FullRebuild, "clip-content");
}

constexpr std::string_view kEffectsScene = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
    <defs>
      <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
      <filter id="shadow" x="-0.2" y="-0.2" width="1.6" height="1.6">
        <feOffset dx="3" dy="3"/><feGaussianBlur stdDeviation="1"/>
        <feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge>
      </filter>
      <marker id="dot" markerWidth="6" markerHeight="6" refX="3" refY="3">
        <circle cx="3" cy="3" r="2" fill="crimson"/></marker>
      <pattern id="checks" width="8" height="8" patternUnits="userSpaceOnUse">
        <rect width="4" height="4" fill="black"/><rect x="4" y="4" width="4" height="4"/></pattern>
    </defs>
    <rect id="blurred" x="8" y="8" width="24" height="16" fill="tomato" filter="url(#blur)"/>
    <g id="shadowed" filter="url(#shadow)">
      <circle id="shadowed-child" cx="60" cy="20" r="10" fill="skyblue"/>
    </g>
    <path id="marked" d="M10 60 L40 70 L70 60" stroke="black" fill="none"
          marker-start="url(#dot)" marker-mid="url(#dot)" marker-end="url(#dot)"/>
    <rect id="patterned" x="50" y="50" width="30" height="30" fill="url(#checks)"/>
  </svg>)svg";

TEST(RetainedRenderTreeUpdateTest, ObjectBoundingBoxFiltersMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kEffectsScene,
                                     Both(MoveElement("#blurred"), MoveElement("#shadowed-child")),
                                     ExpectedPath::Retained, "filter");
}

TEST(RetainedRenderTreeUpdateTest, MarkersAndPatternsMatchFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kEffectsScene,
                                     Both(MoveElement("#marked"), MoveElement("#patterned")),
                                     ExpectedPath::Retained, "marker-pattern");
}

constexpr std::string_view kTextScene = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
    <defs><path id="curve" d="M10 80 Q48 20 86 80"/></defs>
    <text id="label" x="10" y="30" font-size="14">Moving</text>
    <text font-size="10"><textPath href="#curve">Along a path</textPath></text>
    <path id="visible-curve" d="M10 90 L86 90" stroke="black"/>
  </svg>)svg";

TEST(RetainedRenderTreeUpdateTest, TextNextToATextPathTakesTheFullRebuild) {
  // A <textPath> follows its path's transform, so moves in its document keep the full rebuild.
  ExpectSourceFramesMatchFullRebuild(kTextScene, MoveElement("#label"), ExpectedPath::FullRebuild,
                                     "text-path");
}

TEST(RetainedRenderTreeUpdateTest, TextMatchesFullRebuild) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
      <g id="caption"><text x="10" y="30" font-size="14">Moving <tspan fill="red">text</tspan></text>
      </g>
    </svg>)svg";
  ExpectSourceFramesMatchFullRebuild(kSource, MoveElement("#caption"), ExpectedPath::Retained,
                                     "text");
}

constexpr std::string_view kOpacityScene = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
    <defs><rect id="tile" width="20" height="20" fill="teal"/></defs>
    <g id="faded" opacity="0.9">
      <rect width="40" height="40" fill="navy"/>
      <g style="opacity: inherit"><circle cx="40" cy="40" r="16" fill="orange"/></g>
    </g>
    <use id="faded-use" href="#tile" x="50" y="50"/>
  </svg>)svg";

TEST(RetainedRenderTreeUpdateTest, OpacityTakesTheFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kOpacityScene,
                                     Both(SetOpacity("#faded"), SetOpacity("#faded-use")),
                                     ExpectedPath::FullRebuild, "opacity");
}

TEST(RetainedRenderTreeUpdateTest, OpacityWithMovesTakesTheFullRebuild) {
  ExpectSourceFramesMatchFullRebuild(kOpacityScene,
                                     Both(SetOpacity("#faded"), MoveElement("#faded")),
                                     ExpectedPath::FullRebuild, "opacity-move");
}

TEST(RetainedRenderTreeUpdateTest, OpacitySelectorsTakeTheFullRebuild) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
      <style>[opacity="0.6"] + rect { fill: red; }</style>
      <rect id="target" width="40" height="40" fill="navy"/>
      <rect x="50" width="40" height="40" fill="green"/>
    </svg>)svg";
  ExpectSourceFramesMatchFullRebuild(kSource, SetOpacity("#target"), ExpectedPath::FullRebuild,
                                     "opacity-selector");
}

TEST(RetainedRenderTreeUpdateTest, AnimatedDocumentsTakeTheFullRebuild) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
      <rect id="moved" width="20" height="20" fill="navy">
        <animate attributeName="fill" values="navy;red" dur="1s"/>
      </rect>
    </svg>)svg";
  ExpectSourceFramesMatchFullRebuild(kSource, MoveElement("#moved"), ExpectedPath::FullRebuild,
                                     "animated");
}

/// Sprite-field scene: \p change is applied to two copies of a small field.
void ExpectSpriteFieldFramesMatchFullRebuild(SpriteFieldChange change, std::string_view label,
                                             ExpectedPath expected = ExpectedPath::Retained) {
  constexpr SpriteFieldOptions kField{.sprites = 16, .sheets = 3, .spritesPerSheet = 5};
  std::optional<SpriteFieldScene> retained = MakeSpriteFieldScene(kField);
  std::optional<SpriteFieldScene> reference = MakeSpriteFieldScene(kField);
  ASSERT_TRUE(retained.has_value() && reference.has_value());

  const auto advance = [&](SVGDocument& document, int frame) {
    SpriteFieldScene& scene = document == retained->document ? *retained : *reference;
    AdvanceSpriteField(scene, frame, change);
  };
  ExpectFramesMatchFullRebuild(retained->document, reference->document, advance, expected, label);
}

TEST(RetainedRenderTreeUpdateTest, MovingExternalUsesMatchFullRebuild) {
  ExpectSpriteFieldFramesMatchFullRebuild(SpriteFieldChange::Move, "sprite-move");
}

TEST(RetainedRenderTreeUpdateTest, FadingExternalUsesTakeTheFullRebuild) {
  ExpectSpriteFieldFramesMatchFullRebuild(SpriteFieldChange::Opacity, "sprite-opacity",
                                          ExpectedPath::FullRebuild);
}

TEST(RetainedRenderTreeUpdateTest, RetargetedExternalUsesMatchFullRebuild) {
  ExpectSpriteFieldFramesMatchFullRebuild(SpriteFieldChange::Retarget, "sprite-retarget");
}

TEST(RetainedRenderTreeUpdateTest, GameFramesMatchFullRebuild) {
  ExpectSpriteFieldFramesMatchFullRebuild(SpriteFieldChange::All, "sprite-game");
}

TEST(RetainedRenderTreeUpdateTest, ConcurrentDomFramesMatchFullRebuild) {
  // Fetching an external document while a renderer holds the document is refused in ConcurrentDom
  // mode, so this covers a same-document scene.
  SVGDocument retained = Parse(kOpacityScene);
  SVGDocument reference = Parse(kOpacityScene);
  retained.setThreadingMode(ThreadingMode::ConcurrentDom);
  reference.setThreadingMode(ThreadingMode::ConcurrentDom);
  ExpectFramesMatchFullRebuild<DriverRenderer>(
      retained, reference, Both(MoveElement("#faded"), MoveElement("#faded-use")),
      ExpectedPath::Retained, "concurrent-dom");
}

TEST(RetainedRenderTreeUpdateTest, RetargetingToTheSameDocumentTakesTheFullRebuild) {
  std::optional<SpriteFieldScene> retained = MakeSpriteFieldScene({.sprites = 4, .sheets = 2});
  std::optional<SpriteFieldScene> reference = MakeSpriteFieldScene({.sprites = 4, .sheets = 2});
  ASSERT_TRUE(retained.has_value() && reference.has_value());
  // A same-document target creates a shadow tree, which only the full rebuild instantiates.
  const auto retarget = [](SVGDocument& document, int frame) {
    document.querySelector("#u1")->cast<SVGUseElement>().setHref(
        RcString(frame % 2 == 1 ? "#u0" : "sheet-1.svg#sprite-1"));
  };
  ExpectFramesMatchFullRebuild(retained->document, reference->document, retarget,
                               ExpectedPath::FullRebuild, "retarget-local");
}

/// Sizes a `<use>` establishes for its symbol or nested `<svg>` instance, per frame.
struct UseGeometry {
  double x;
  double y;
  double width;
  double height;
};

/// Each frame changes one kind of geometry: width alone, height alone, then the position.
constexpr UseGeometry kUseGeometries[] = {
    {4, 4, 20, 20}, {4, 4, 40, 20}, {4, 4, 40, 50}, {12, 6, 40, 50}};

/// How a test writes a use's geometry.
enum class GeometryWrite {
  TypedSetters,  //!< `setX()`, `setY()`, `setWidth()`, `setHeight()`.
  Attributes,    //!< `setAttribute()` with the attribute name.
};

void WriteUseLength(SVGUseElement use, std::string_view name, double value, GeometryWrite write) {
  if (write == GeometryWrite::Attributes) {
    use.setAttribute(xml::XMLQualifiedNameRef(name), std::to_string(value));
  } else if (name == "x") {
    use.setX(Lengthd(value));
  } else if (name == "y") {
    use.setY(Lengthd(value));
  } else if (name == "width") {
    use.setWidth(Lengthd(value));
  } else {
    use.setHeight(Lengthd(value));
  }
}

/// Writes only the attributes that differ between \p from and \p to.
void ApplyUseGeometry(SVGUseElement use, const UseGeometry& from, const UseGeometry& to,
                      GeometryWrite write) {
  const std::array<std::tuple<std::string_view, double, double>, 4> fields = {{
      {"x", from.x, to.x},
      {"y", from.y, to.y},
      {"width", from.width, to.width},
      {"height", from.height, to.height},
  }};
  for (const auto& [name, before, after] : fields) {
    if (before != after) {
      WriteUseLength(use, name, after, write);
    }
  }
}

std::string UseAttributes(const UseGeometry& geometry) {
  return "x=\"" + std::to_string(geometry.x) + "\" y=\"" + std::to_string(geometry.y) +
         "\" width=\"" + std::to_string(geometry.width) + "\" height=\"" +
         std::to_string(geometry.height) + "\"";
}

constexpr std::string_view kViewportTargets = R"svg(
    <symbol id="symbol" viewBox="0 0 10 10" preserveAspectRatio="none">
      <rect width="10" height="10" fill="teal"/><circle cx="5" cy="5" r="3" fill="red"/>
    </symbol>
    <svg id="nested" viewBox="0 0 10 10">
      <rect width="10" height="10" fill="navy"/><circle cx="5" cy="5" r="3" fill="gold"/>
    </svg>)svg";

/// A host whose `<use>` elements point at \p prefix + `#symbol` and `#nested`, with geometry \p g.
std::string ViewportHost(std::string_view prefix, const UseGeometry& g, bool localTargets) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">)";
  if (localTargets) {
    source += "<defs>" + std::string(kViewportTargets) + "</defs>";
  }
  source += "<use id=\"to-symbol\" href=\"" + std::string(prefix) + "#symbol\" " +
            UseAttributes(g) + "/>";
  source += "<g transform=\"translate(0 30)\"><use id=\"to-svg\" href=\"" + std::string(prefix) +
            "#nested\" " + UseAttributes(g) + "/></g></svg>";
  return source;
}

std::string ViewportSheet() {
  return R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><defs>)" +
         std::string(kViewportTargets) + "</defs></svg>";
}

/// Edits both uses' x, y, width and height after each draw, and compares every frame with a
/// fresh parse of the same state.
void ExpectUseViewportEditsMatchFreshDocuments(
    const std::function<std::optional<SVGDocument>(const UseGeometry&)>& makeDocument,
    GeometryWrite write, std::string_view label,
    std::optional<ExpectedPath> expected = std::nullopt) {
  std::optional<SVGDocument> edited = makeDocument(kUseGeometries[0]);
  ASSERT_TRUE(edited.has_value());
  Renderer editedRenderer;
  editedRenderer.draw(*edited);
  const RenderTreeState::Counters before = ReadCounters(*edited);
  for (std::size_t frame = 1; frame < std::size(kUseGeometries); ++frame) {
    for (const std::string_view id : {"#to-symbol", "#to-svg"}) {
      ApplyUseGeometry(edited->querySelector(id)->cast<SVGUseElement>(), kUseGeometries[frame - 1],
                       kUseGeometries[frame], write);
    }
    editedRenderer.draw(*edited);

    std::optional<SVGDocument> fresh = makeDocument(kUseGeometries[frame]);
    ASSERT_TRUE(fresh.has_value());
    Renderer freshRenderer;
    freshRenderer.draw(*fresh);
    // Both instances must have drawn something, or a blank pair would compare equal.
    const RendererBitmap freshBitmap = freshRenderer.takeSnapshot();
    EXPECT_GT(std::count_if(freshBitmap.pixels.begin(), freshBitmap.pixels.end(),
                            [](uint8_t channel) { return channel != 0; }),
              0)
        << label;
    editor::tests::CompareBitmapToBitmap(editedRenderer.takeSnapshot(), freshBitmap,
                                         std::string(label) + "-frame-" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }

  if (expected == ExpectedPath::Retained) {
    const RenderTreeState::Counters after = ReadCounters(*edited);
    EXPECT_EQ(after.fullRebuilds, before.fullRebuilds) << label;
    EXPECT_EQ(after.retainedUpdates, before.retainedUpdates + std::size(kUseGeometries) - 1)
        << label;
  }
}

std::optional<SVGDocument> MakeLocalViewportHost(const UseGeometry& geometry) {
  return Parse(ViewportHost("", geometry, /*localTargets=*/true));
}

std::optional<SVGDocument> MakeExternalViewportHost(const UseGeometry& geometry) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      ViewportHost(kExternalResourceUrl, geometry, /*localTargets=*/false), ViewportSheet());
  if (!host) {
    return std::nullopt;
  }
  return std::move(host->document);
}

TEST(RetainedRenderTreeUpdateTest, SameDocumentUseViewportSettersMatchFreshDocuments) {
  ExpectUseViewportEditsMatchFreshDocuments(MakeLocalViewportHost, GeometryWrite::TypedSetters,
                                            "local-use-viewport-setters");
}

TEST(RetainedRenderTreeUpdateTest, SameDocumentUseViewportAttributesMatchFreshDocuments) {
  ExpectUseViewportEditsMatchFreshDocuments(MakeLocalViewportHost, GeometryWrite::Attributes,
                                            "local-use-viewport-attributes");
}

TEST(RetainedRenderTreeUpdateTest, ExternalUseViewportSettersMatchFreshDocuments) {
  // An external instance takes its viewport when it is drawn, so these frames keep the tree.
  ExpectUseViewportEditsMatchFreshDocuments(MakeExternalViewportHost, GeometryWrite::TypedSetters,
                                            "external-use-viewport-setters",
                                            ExpectedPath::Retained);
}

TEST(RetainedRenderTreeUpdateTest, ExternalUseViewportAttributesMatchFreshDocuments) {
  ExpectUseViewportEditsMatchFreshDocuments(MakeExternalViewportHost, GeometryWrite::Attributes,
                                            "external-use-viewport-attributes");
}

FrameEdit SetTransformAttribute(std::string selector) {
  return [selector](SVGDocument& document, int frame) {
    Element(document, selector)
        .setAttribute(
            xml::XMLQualifiedNameRef("transform"),
            "translate(" + std::to_string(3 * frame) + " " + std::to_string(2 * frame) + ")");
  };
}

TEST(RetainedRenderTreeUpdateTest, TransformAttributeWritesTakeTheFullRebuild) {
  // An XML transform edit is a settled change that also re-rasterizes filtered layers.
  ExpectSourceFramesMatchFullRebuild(kNestedScene, SetTransformAttribute("#parent"),
                                     ExpectedPath::FullRebuild, "transform-attribute");
}

TEST(RetainedRenderTreeUpdateTest, TransformSelectorsTakeTheFullRebuild) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
      <style>[transform] > rect { fill: red; }</style>
      <g id="moved"><rect width="40" height="40" fill="navy"/></g>
    </svg>)svg";
  ExpectSourceFramesMatchFullRebuild(kSource, SetTransformAttribute("#moved"),
                                     ExpectedPath::FullRebuild, "transform-selector");
}

TEST(RetainedRenderTreeUpdateTest, NonScalingStrokesMatchFullRebuild) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
      <g id="scaled">
        <path d="M5 10 L40 10 L40 40 Z" stroke="black" stroke-width="3" fill="gold"
              vector-effect="non-scaling-stroke"/>
      </g>
    </svg>)svg";
  const FrameEdit scale = [](SVGDocument& document, int frame) {
    Element(document, "#scaled")
        .setTransform(Transform2d::Scale(Vector2d(2.0, 1.0)) *
                      Transform2d::Translate(Vector2d(frame, 2.0 * frame)));
  };
  ExpectSourceFramesMatchFullRebuild(kSource, scale, ExpectedPath::Retained, "non-scaling-stroke");
}

/// Points spread over a 96x96 canvas for hit testing.
std::vector<Vector2d> ProbePoints() {
  std::vector<Vector2d> points;
  for (int y = 2; y < 96; y += 6) {
    for (int x = 2; x < 96; x += 6) {
      points.emplace_back(x, y);
    }
  }
  return points;
}

std::string HitId(DonnerController& controller, const Vector2d& point) {
  const std::optional<SVGGraphicsElement> hit = controller.findIntersecting(point);
  return hit ? std::string(hit->id()) + "/" + std::string(hit->tagName().name) : "none";
}

TEST(RetainedRenderTreeUpdateTest, BoundsAndHitTestsMatchFullRebuild) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">
      <defs><rect id="tile" width="10" height="10" fill="teal"/></defs>
      <g id="parent">
        <circle id="dot" cx="20" cy="20" r="8" fill="orange"/>
        <use id="tile-use" href="#tile" x="30" y="5"/>
        <rect id="box" x="10" y="50" width="30" height="20" fill="navy"/>
      </g>
    </svg>)svg";
  SVGDocument retained = Parse(kSource);
  SVGDocument reference = Parse(kSource);
  DisableRetainedUpdates(reference);
  Renderer retainedRenderer;
  Renderer referenceRenderer;
  retainedRenderer.draw(retained);
  referenceRenderer.draw(reference);

  const uint64_t retainedBefore = ReadCounters(retained).retainedUpdates;
  for (int frame = 1; frame <= kFrames; ++frame) {
    for (SVGDocument* document : {&retained, &reference}) {
      MoveElement("#parent")(*document, frame);
    }
    retainedRenderer.draw(retained);
    referenceRenderer.draw(reference);

    for (const std::string_view id : {"#dot", "#box"}) {
      EXPECT_EQ(retained.querySelector(id)->cast<SVGGeometryElement>().worldBounds(),
                reference.querySelector(id)->cast<SVGGeometryElement>().worldBounds())
          << id << " frame " << frame;
    }
    DonnerController retainedController(retained);
    DonnerController referenceController(reference);
    for (const Vector2d& point : ProbePoints()) {
      EXPECT_EQ(HitId(retainedController, point), HitId(referenceController, point))
          << point << " frame " << frame;
    }
  }
  EXPECT_EQ(ReadCounters(retained).retainedUpdates, retainedBefore + kFrames);
}

/// A host with one external `<use>` of the sprite sheet, plus \p style as its stylesheet.
std::optional<SVGDocument> MakeStyledSpriteHost(std::string_view style) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96"><style>)" +
          std::string(style) +
          R"(</style><use id="sprite" href="sprites.svg#sprite-0" data-kind="hero"/></svg>)",
      MakeSpriteSheetSource(SpriteKind::Vector));
  if (!host) {
    return std::nullopt;
  }
  return std::move(host->document);
}

const FrameEdit kNextSprite = [](SVGDocument& document, int frame) {
  document.querySelector("#sprite")->cast<SVGUseElement>().setHref(
      RcString("sprites.svg#sprite-" + std::to_string(frame % kSpriteSheetSpriteCount)));
};

TEST(RetainedRenderTreeUpdateTest, HrefSelectorsTakeTheFullRebuild) {
  constexpr std::string_view kStyle = R"(use[href$="sprite-1"] { fill: red; })";
  std::optional<SVGDocument> retained = MakeStyledSpriteHost(kStyle);
  std::optional<SVGDocument> reference = MakeStyledSpriteHost(kStyle);
  ASSERT_TRUE(retained.has_value() && reference.has_value());
  ExpectFramesMatchFullRebuild(*retained, *reference, kNextSprite, ExpectedPath::FullRebuild,
                               "href-selector");
}

TEST(RetainedRenderTreeUpdateTest, OtherAttributeSelectorsKeepRetargetsRetained) {
  constexpr std::string_view kStyle = R"([data-kind="hero"] { fill: red; })";
  std::optional<SVGDocument> retained = MakeStyledSpriteHost(kStyle);
  std::optional<SVGDocument> reference = MakeStyledSpriteHost(kStyle);
  ASSERT_TRUE(retained.has_value() && reference.has_value());
  ExpectFramesMatchFullRebuild(*retained, *reference, kNextSprite, ExpectedPath::Retained,
                               "other-selector");
}

TEST(RetainedRenderTreeUpdateTest, CrossDocumentRetargetsMatchFullRebuild) {
  // Two uses draw from sheets 0 and 1; sheet 2 is referenced by nothing until a retarget, so its
  // first retarget loads and admits a document the last full rebuild never saw.
  constexpr SpriteFieldOptions kField{.sprites = 2, .sheets = 3, .spritesPerSheet = 4};
  std::optional<SpriteFieldScene> retained = MakeSpriteFieldScene(kField);
  std::optional<SpriteFieldScene> reference = MakeSpriteFieldScene(kField);
  ASSERT_TRUE(retained.has_value() && reference.has_value());
  const FrameEdit hop = [](SVGDocument& document, int frame) {
    const int sheet = (frame + 1) % 3;
    document.querySelector("#u0")->cast<SVGUseElement>().setHref(
        RcString("sheet-" + std::to_string(sheet) + ".svg#sprite-" + std::to_string(frame % 4)));
  };
  ExpectFramesMatchFullRebuild(retained->document, reference->document, hop, ExpectedPath::Retained,
                               "cross-document");
}

/// Promotes the external `<use>` `#use` of \p document into a compositor layer, applies
/// \p edit, and returns the composited frame that follows. With no edit, renders the same steps
/// on an unedited document, which is the uncached reference for the edited state.
RendererBitmap CompositeAfterPromotion(SVGDocument& document,
                                       const std::function<void(SVGUseElement&)>& edit,
                                       bool editBeforePromotion) {
  const RenderViewport viewport{Vector2d(96, 96), 1.0};
  Renderer renderer;
  compositor::CompositorController compositor(document, renderer);
  compositor.renderFrame(viewport);
  SVGUseElement use = document.querySelector("#use")->cast<SVGUseElement>();
  const Entity entity = use.unsafeEntityHandle().entity();
  if (editBeforePromotion && edit) {
    edit(use);
  }
  compositor.promoteEntity(entity);
  compositor.renderFrame(viewport);
  if (!editBeforePromotion && edit) {
    edit(use);
  }
  compositor.renderFrame(viewport);
  compositor.renderFrame(viewport);
  return renderer.takeSnapshot();
}

std::optional<SVGDocument> MakeCompositedHost(std::string_view useAttributes) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">)"
      R"(<rect width="96" height="96" fill="white"/><use id="use" opacity="0.5" )" +
          std::string(useAttributes) + "/></svg>",
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><defs>)" +
          std::string(kViewportTargets) +
          R"(<g id="plain"><rect width="10" height="10" fill="purple"/></g></defs></svg>)");
  if (!host) {
    return std::nullopt;
  }
  return std::move(host->document);
}

TEST(RetainedRenderTreeUpdateTest, PromotedExternalUseResizeMatchesUncachedRender) {
  std::optional<SVGDocument> edited =
      MakeCompositedHost(R"(href="sprites.svg#symbol" x="4" y="4" width="20" height="20")");
  std::optional<SVGDocument> fresh =
      MakeCompositedHost(R"(href="sprites.svg#symbol" x="4" y="4" width="50" height="30")");
  ASSERT_TRUE(edited.has_value() && fresh.has_value());
  const RendererBitmap actual = CompositeAfterPromotion(
      *edited,
      [](SVGUseElement& use) {
        use.setWidth(Lengthd(50));
        use.setHeight(Lengthd(30));
      },
      /*editBeforePromotion=*/false);
  const RendererBitmap expected = CompositeAfterPromotion(*fresh, {}, false);
  editor::tests::CompareBitmapToBitmap(actual, expected, "promoted-use-resize",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RetainedRenderTreeUpdateTest, PromotedExternalUseMoveMatchesUncachedRender) {
  std::optional<SVGDocument> edited =
      MakeCompositedHost(R"(href="sprites.svg#symbol" x="4" y="4" width="20" height="20")");
  std::optional<SVGDocument> fresh =
      MakeCompositedHost(R"svg(href="sprites.svg#symbol" x="4" y="4" width="20" height="20" )svg"
                         R"svg(transform="translate(30 12)")svg");
  ASSERT_TRUE(edited.has_value() && fresh.has_value());
  const RendererBitmap actual = CompositeAfterPromotion(
      *edited,
      [](SVGUseElement& use) { use.setTransform(Transform2d::Translate(Vector2d(30, 12))); },
      /*editBeforePromotion=*/false);
  const RendererBitmap expected = CompositeAfterPromotion(*fresh, {}, false);
  editor::tests::CompareBitmapToBitmap(actual, expected, "promoted-use-move",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RetainedRenderTreeUpdateTest, PromotingAJustRetargetedExternalUseMatchesUncachedRender) {
  std::optional<SVGDocument> edited =
      MakeCompositedHost(R"(href="sprites.svg#symbol" width="40" height="40")");
  std::optional<SVGDocument> fresh =
      MakeCompositedHost(R"(href="sprites.svg#plain" width="40" height="40")");
  ASSERT_TRUE(edited.has_value() && fresh.has_value());
  // Promotion happens between the retarget and the next prepare, while the instance is stale.
  const RendererBitmap actual = CompositeAfterPromotion(
      *edited, [](SVGUseElement& use) { use.setHref(RcString("sprites.svg#plain")); },
      /*editBeforePromotion=*/true);
  const RendererBitmap expected = CompositeAfterPromotion(*fresh, {}, true);
  editor::tests::CompareBitmapToBitmap(actual, expected, "promoted-use-retarget",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg::tests
