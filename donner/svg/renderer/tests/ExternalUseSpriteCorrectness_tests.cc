/// @file
/// CPU-invariant counters for an animated external-use sprite: the sprite sheet is restyled only
/// when an occurrence's inputs change, never on frames that only move the `<use>`.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStopElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"

namespace donner::svg::tests {
namespace {

/// Counts element restyles in a document: each restyle constructs a computed style.
class RestyleCounter {
public:
  explicit RestyleCounter(SVGDocument document) : document_(std::move(document)) {
    document_.registry()
        .on_construct<components::ComputedStyleComponent>()
        .connect<&RestyleCounter::onRestyle>(*this);
  }

  ~RestyleCounter() {
    document_.registry()
        .on_construct<components::ComputedStyleComponent>()
        .disconnect<&RestyleCounter::onRestyle>(*this);
  }

  RestyleCounter(const RestyleCounter&) = delete;
  RestyleCounter& operator=(const RestyleCounter&) = delete;

  std::size_t restyles() const { return restyles_; }

private:
  void onRestyle(Registry&, Entity) { ++restyles_; }

  SVGDocument document_;
  std::size_t restyles_ = 0;
};

/// Renders a self-contained document, recording a test failure if it does not parse.
RendererBitmap RenderStandalone(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  if (parsed.hasError()) {
    ADD_FAILURE() << parsed.error();
    return {};
  }
  Renderer renderer;
  renderer.draw(parsed.result());
  return renderer.takeSnapshot();
}

SVGDocument SpriteSheet(const ExternalUseSpriteScene& scene) {
  return SVGDocument::CreateFromHandle(*scene.host.resource);
}

TEST(ExternalUseSpriteCorrectnessTest, MovingTheUseDoesNotRestyleTheSpriteSheet) {
  std::optional<ExternalUseSpriteScene> scene = MakeExternalUseSpriteScene(SpriteKind::Vector);
  ASSERT_TRUE(scene.has_value());
  Renderer renderer;
  renderer.draw(scene->host.document);
  ASSERT_THAT(*scene->host.resource, testing::NotNull());

  RestyleCounter counter(SpriteSheet(*scene));
  for (int frame = 1; frame <= 8; ++frame) {
    MoveExternalUseSprite(*scene, frame);
    renderer.draw(scene->host.document);
  }
  EXPECT_EQ(counter.restyles(), 0u);
}

TEST(ExternalUseSpriteCorrectnessTest, ReturningToDrawnSpritesDoesNotRestyleTheSpriteSheet) {
  std::optional<ExternalUseSpriteScene> scene = MakeExternalUseSpriteScene(SpriteKind::Vector);
  ASSERT_TRUE(scene.has_value());
  Renderer renderer;
  for (int sprite = 0; sprite < kSpriteSheetSpriteCount; ++sprite) {
    SetExternalUseSprite(*scene, sprite);
    renderer.draw(scene->host.document);
  }

  RestyleCounter counter(SpriteSheet(*scene));
  for (int sprite = 0; sprite < kSpriteSheetSpriteCount; ++sprite) {
    SetExternalUseSprite(*scene, sprite);
    MoveExternalUseSprite(*scene, sprite);
    renderer.draw(scene->host.document);
  }
  EXPECT_EQ(counter.restyles(), 0u);
}

TEST(ExternalUseSpriteCorrectnessTest, AnimatedFramesMatchAFreshRenderOfTheSameState) {
  constexpr int kFrames = 3 * kFramesPerSpriteChange + 1;
  std::optional<ExternalUseSpriteScene> animated = MakeExternalUseSpriteScene(SpriteKind::Vector);
  ASSERT_TRUE(animated.has_value());
  Renderer animatedRenderer;
  for (int frame = 0; frame < kFrames; ++frame) {
    AdvanceExternalUseSpriteFrame(*animated, frame);
    animatedRenderer.draw(animated->host.document);
  }

  std::optional<ExternalUseSpriteScene> fresh = MakeExternalUseSpriteScene(SpriteKind::Vector);
  ASSERT_TRUE(fresh.has_value());
  MoveExternalUseSprite(*fresh, kFrames - 1);
  SetExternalUseSprite(*fresh, (kFrames - 1) / kFramesPerSpriteChange);
  Renderer freshRenderer;
  freshRenderer.draw(fresh->host.document);

  editor::tests::CompareBitmapToBitmap(animatedRenderer.takeSnapshot(),
                                       freshRenderer.takeSnapshot(), "external-use-sprite-animated",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(ExternalUseSpriteCorrectnessTest, HostStyleChangeRestylesTheRetainedTarget) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use id="tile-use" href="sprites.svg#tile" fill="red"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);

  host->document.querySelector("#tile-use")->setStyle("fill: blue");
  const RendererBitmap expected = RenderStandalone(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="blue"/>
      </svg>)svg");
  for (int frame = 0; frame < 2; ++frame) {
    renderer.draw(host->document);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         "external-use-host-style-change-" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(ExternalUseSpriteCorrectnessTest, TargetMutationRestylesEveryRetainedTarget) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#a" fill="red"/>
        <use href="sprites.svg#b" x="8" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="a" width="8" height="8"/>
        <rect id="b" width="8" height="8"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);

  SVGDocument resource = SVGDocument::CreateFromHandle(*host->resource);
  resource.querySelector("#b")->setAttribute(xml::XMLQualifiedNameRef("fill-opacity"), "0.5");
  renderer.draw(host->document);

  const RendererBitmap expected = RenderStandalone(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="red"/>
        <rect x="8" width="8" height="8" fill="blue" fill-opacity="0.5"/>
      </svg>)svg");
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "external-use-target-mutation",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(ExternalUseSpriteCorrectnessTest, EvictedTargetsInheritHostStyleWhenDrawnAgain) {
  constexpr int kTargets = 40;
  std::string resource = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">)svg";
  for (int target = 0; target < kTargets; ++target) {
    resource += "<rect id=\"t" + std::to_string(target) + "\" width=\"8\" height=\"8\"/>";
  }
  resource += "</svg>";
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use id="tile-use" href="sprites.svg#t0" fill="red"/>
      </svg>)svg",
      std::move(resource));
  ASSERT_TRUE(host.has_value());
  SVGUseElement use = host->document.querySelector("#tile-use")->cast<SVGUseElement>();
  Renderer renderer;
  for (int target = 0; target < kTargets; ++target) {
    use.setHref(RcString("sprites.svg#t" + std::to_string(target)));
    renderer.draw(host->document);
  }
  use.setHref(RcString("sprites.svg#t0"));
  renderer.draw(host->document);

  const RendererBitmap expected = RenderStandalone(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="red"/>
      </svg>)svg");
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "external-use-evicted-target",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Draws \p document \p frames times, comparing every frame with \p expected rendered standalone.
void ExpectFrames(SVGDocument& document, Renderer& renderer, int frames, std::string_view expected,
                  const std::string& label) {
  const RendererBitmap want = RenderStandalone(expected);
  for (int frame = 0; frame < frames; ++frame) {
    renderer.draw(document);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), want,
                                         label + "-" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(ExternalUseRetentionTest, CrossSubtreeGradientKeepsDocumentStyle) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#b" color="red"/>
        <use href="sprites.svg#a" x="8" color="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <g id="b">
          <linearGradient id="g"><stop offset="0" stop-color="currentColor"/><stop offset="1" stop-color="currentColor"/></linearGradient>
          <rect width="8" height="8" fill="url(#g)"/>
        </g>
        <rect id="a" width="8" height="8" fill="url(#g)"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <linearGradient id="r"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="red"/></linearGradient>
        <linearGradient id="k"><stop offset="0" stop-color="black"/><stop offset="1" stop-color="black"/></linearGradient>
        <rect width="8" height="8" fill="url(#r)"/>
        <rect x="8" width="8" height="8" fill="url(#k)"/>
      </svg>)svg",
               "cross-subtree-gradient");
}

TEST(ExternalUseRetentionTest, OutOfDrawComputedStyleOnRetainedRoot) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="sprites.svg#a" fill="red"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="a" width="8" height="8"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);
  SVGDocument resource = SVGDocument::CreateFromHandle(*host->resource);
  resource.querySelector("#a")->setAttribute(xml::XMLQualifiedNameRef("fill-opacity"), "1");
  (void)resource.querySelector("#a")->getComputedStyle();
  ExpectFrames(host->document, renderer, 2,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="red"/>
      </svg>)svg",
               "out-of-draw-computed-style");
}

TEST(ExternalUseRetentionTest, OutOfDrawComputedStyleObservesHostStyle) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="sprites.svg#a" fill="red"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="a" width="8" height="8"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  SVGDocument resource = SVGDocument::CreateFromHandle(*host->resource);
  const PropertyRegistry& style = resource.querySelector("#a")->getComputedStyle();
  EXPECT_EQ(style.fill.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 0xFF)))))
      << "target document's own computed style leaks the host occurrence";
}

TEST(ExternalUseRetentionTest, SameTargetTwoStylesOneFrame) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#a" fill="red"/>
        <use href="sprites.svg#a" x="8" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="a" width="8" height="8"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="red"/><rect x="8" width="8" height="8" fill="blue"/>
      </svg>)svg",
               "same-target-two-styles");
}

TEST(ExternalUseRetentionTest, OverlappingSubtrees) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#p" fill="red"/>
        <use href="sprites.svg#c" x="8" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <g id="p"><rect id="c" width="8" height="8"/></g>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="red"/><rect x="8" width="8" height="8" fill="blue"/>
      </svg>)svg",
               "overlapping");
}

TEST(ExternalUseRetentionTest, ImageInterleavedWithUse) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <use href="sprites.svg#a" fill="red"/>
        <image href="sprites.svg" x="8" width="8" height="8"/>
        <use href="sprites.svg#a" x="16" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="a" width="8" height="8" fill-opacity="0.5"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <rect width="8" height="8" fill="red" fill-opacity="0.5"/>
        <rect x="8" width="8" height="8" fill="black" fill-opacity="0.5"/>
        <rect x="16" width="8" height="8" fill="blue" fill-opacity="0.5"/>
      </svg>)svg",
               "image-interleaved");
}

TEST(ExternalUseRetentionTest, ContextFillFollowsHost) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use id="u" href="sprites.svg#a" fill="red"/>
        <use href="sprites.svg#a" x="8" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <g id="a"><rect width="8" height="8" fill="context-fill"/></g>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 2,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="red"/><rect x="8" width="8" height="8" fill="blue"/>
      </svg>)svg",
               "context-fill");
  host->document.querySelector("#u")->setStyle("fill: lime");
  ExpectFrames(host->document, renderer, 2,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="lime"/><rect x="8" width="8" height="8" fill="blue"/>
      </svg>)svg",
               "context-fill-changed");
}

TEST(ExternalUseRetentionTest, ReadsAfterHostDestructionSeeOnlyTheTargetDocument) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <linearGradient id="g"><stop stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient>
        <use href="sprites.svg#a" fill="url(#g)"/>
        <use href="sprites.svg#b" x="8" fill="url(#g)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="a" width="8" height="8"/>
        <rect id="b" width="8" height="8" fill="context-fill"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);

  SVGDocument resource = SVGDocument::CreateFromHandle(*host->resource);
  host->document = SVGDocument();

  for (const char* id : {"#a", "#b"}) {
    SCOPED_TRACE(id);
    const SVGElement element = *resource.querySelector(id);
    EXPECT_THAT(element.resolvedFillPaint(), testing::Eq(std::nullopt));
  }
  EXPECT_THAT(
      resource.querySelector("#a")->getComputedStyle().fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 0xFF))))));
  editor::tests::CompareBitmapToBitmap(
      [&] {
        Renderer standalone;
        standalone.draw(resource);
        return standalone.takeSnapshot();
      }(),
      RenderStandalone(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8"/>
      </svg>)svg"),
      "external-use-after-host-destruction", editor::tests::PixelmatchIdentityParams());
}

/// Sprite b: a gradient whose stops take the host color, and a rect painted with it.
constexpr std::string_view kGradB = R"svg(<g id="b">
  <linearGradient id="g"><stop offset="0" stop-color="currentColor"/><stop offset="1" stop-color="currentColor"/></linearGradient>
  <rect width="8" height="8" fill="url(#g)"/>
</g>)svg";

/// A sheet holding sprite b followed by \p rest.
std::string Sheet(std::string_view rest) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">)svg") +
         std::string(kGradB) + std::string(rest) + "</svg>";
}

constexpr std::string_view kCrossSubtreeHost =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
  <use href="sprites.svg#b" color="red"/>
  <use href="sprites.svg#a" x="8" color="blue"/>
</svg>)svg";

/// The expected host rendering: b in the host color, then \p aBody with \p defs in the sheet's
/// own style.
std::string Expected(std::string_view defs, std::string_view aBody) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
  <linearGradient id="r"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="red"/></linearGradient>
  <linearGradient id="g"><stop offset="0" stop-color="black"/><stop offset="1" stop-color="black"/></linearGradient>
  <rect width="8" height="8" fill="url(#r)"/>)svg") +
         std::string(defs) + "<g transform=\"translate(8 0)\">" + std::string(aBody) + "</g></svg>";
}

TEST(ExternalUseRetentionTest, PatternContentReadsGradientInOtherSubtree) {
  constexpr std::string_view kDefs =
      R"svg(<pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse"><rect width="8" height="8" fill="url(#g)"/></pattern>)svg";
  constexpr std::string_view kA = R"svg(<rect id="a" width="8" height="8" fill="url(#p)"/>)svg";
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kCrossSubtreeHost, Sheet(std::string(kDefs) + std::string(kA)));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               Expected(kDefs, R"svg(<rect width="8" height="8" fill="url(#p)"/>)svg"),
               "pattern-content");
}

TEST(ExternalUseRetentionTest, MarkerContentReadsGradientInOtherSubtree) {
  constexpr std::string_view kDefs =
      R"svg(<marker id="m" markerWidth="8" markerHeight="8" refX="0" refY="0" markerUnits="userSpaceOnUse" overflow="visible"><rect width="8" height="8" fill="url(#g)"/></marker>)svg";
  constexpr std::string_view kA =
      R"svg(<path id="a" d="M0,0 L1,0" stroke="none" marker-start="url(#m)"/>)svg";
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kCrossSubtreeHost, Sheet(std::string(kDefs) + std::string(kA)));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(
      host->document, renderer, 3,
      Expected(kDefs, R"svg(<path d="M0,0 L1,0" stroke="none" marker-start="url(#m)"/>)svg"),
      "marker-content");
}

TEST(ExternalUseRetentionTest, MaskContentReadsGradientInOtherSubtree) {
  constexpr std::string_view kDefs =
      R"svg(<mask id="k" maskUnits="userSpaceOnUse" x="0" y="0" width="8" height="8"><rect width="8" height="8" fill="white"/><rect width="4" height="8" fill="url(#g)"/></mask>)svg";
  constexpr std::string_view kA =
      R"svg(<rect id="a" width="8" height="8" fill="green" mask="url(#k)"/>)svg";
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kCrossSubtreeHost, Sheet(std::string(kDefs) + std::string(kA)));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               Expected(kDefs, R"svg(<rect width="8" height="8" fill="green" mask="url(#k)"/>)svg"),
               "mask-content");
}

TEST(ExternalUseRetentionTest, TextMetricsAfterDrawUseDocumentStyle) {
  constexpr std::string_view kSheet =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="50">
    <g id="a"><text id="t" y="20">Hello world</text></g></svg>)svg";
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="50">
        <use href="sprites.svg#a" font-size="40"/></svg>)svg",
      std::string(kSheet));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);
  SVGDocument resource = SVGDocument::CreateFromHandle(*host->resource);
  const double afterDraw =
      resource.querySelector("#t")->cast<SVGTextElement>().getComputedTextLength();

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto standalone = parser::SVGParser::ParseSVG(kSheet, warnings);
  ASSERT_FALSE(standalone.hasError());
  const double own =
      standalone.result().querySelector("#t")->cast<SVGTextElement>().getComputedTextLength();
  EXPECT_GT(own, 0.0);
  EXPECT_DOUBLE_EQ(afterDraw, own)
      << "text metrics on the target document reflect the host font-size";
}

TEST(ExternalUseRetentionTest, LayoutAndGeometryReadsAfterDrawUseDocumentStyle) {
  constexpr std::string_view kSheet =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="50">
    <g id="a"><rect id="r" style="width: 2em; height: 8px; transform: translateX(1em)"/></g></svg>)svg";
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="50">
        <use href="sprites.svg#a" font-size="40"/></svg>)svg",
      std::string(kSheet));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);
  SVGDocument resource = SVGDocument::CreateFromHandle(*host->resource);
  const SVGRectElement afterDraw = resource.querySelector("#r")->cast<SVGRectElement>();

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto standalone = parser::SVGParser::ParseSVG(kSheet, warnings);
  ASSERT_FALSE(standalone.hasError());
  const SVGRectElement own = standalone.result().querySelector("#r")->cast<SVGRectElement>();

  EXPECT_EQ(afterDraw.elementFromWorld(), own.elementFromWorld());
  EXPECT_EQ(afterDraw.transform(), own.transform());
  EXPECT_THAT(afterDraw.worldBounds(), testing::Eq(own.worldBounds()));
  EXPECT_THAT(afterDraw.computedWidth(), testing::Eq(own.computedWidth()));
}

TEST(ExternalUseRetentionTest, StylesheetRuleReadsGradientInOtherSubtree) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#b" color="red"/>
        <use href="sprites.svg#a" x="8" color="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <style>#a { fill: url(#g) }</style>
        <g id="b">
          <linearGradient id="g"><stop offset="0" stop-color="currentColor"/><stop offset="1" stop-color="currentColor"/></linearGradient>
          <rect width="8" height="8" fill="url(#g)"/>
        </g>
        <rect id="a" width="8" height="8"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  const RendererBitmap want = RenderStandalone(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <linearGradient id="r"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="red"/></linearGradient>
        <linearGradient id="k"><stop offset="0" stop-color="black"/><stop offset="1" stop-color="black"/></linearGradient>
        <rect width="8" height="8" fill="url(#r)"/>
        <rect x="8" width="8" height="8" fill="url(#k)"/>
      </svg>)svg");
  for (int frame = 0; frame < 3; ++frame) {
    renderer.draw(host->document);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), want,
                                         "stylesheet-rule-" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(ExternalUseRetentionTest, StopColorAfterDrawUsesDocumentStyle) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="sprites.svg#a" color="red"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <g id="a">
          <linearGradient id="g"><stop id="s" offset="0" stop-color="currentColor"/><stop offset="1" stop-color="currentColor"/></linearGradient>
          <rect width="8" height="8" fill="url(#g)"/>
        </g>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);
  SVGDocument resource = SVGDocument::CreateFromHandle(*host->resource);
  const css::Color color = resource.querySelector("#s")->cast<SVGStopElement>().computedStopColor();
  EXPECT_EQ(color, css::Color(css::RGBA(0, 0, 0, 0xFF))) << "stop color reflects the host color";
}

}  // namespace
}  // namespace donner::svg::tests
