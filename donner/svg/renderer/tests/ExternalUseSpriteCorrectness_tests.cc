/// @file
/// CPU-invariant counters for an animated external-use sprite: the sprite sheet is restyled only
/// when an occurrence's inputs change, never on frames that only move the `<use>`.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStopElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"

namespace donner::svg::tests {
namespace {

/// Counts constructions of \p Component in a document.
template <typename Component>
class ConstructionCounter {
public:
  explicit ConstructionCounter(SVGDocument document) : document_(std::move(document)) {
    document_.registry()
        .template on_construct<Component>()
        .template connect<&ConstructionCounter::onConstruct>(*this);
  }

  ~ConstructionCounter() {
    document_.registry()
        .template on_construct<Component>()
        .template disconnect<&ConstructionCounter::onConstruct>(*this);
  }

  ConstructionCounter(const ConstructionCounter&) = delete;
  ConstructionCounter& operator=(const ConstructionCounter&) = delete;

  std::size_t constructions() const { return constructions_; }

private:
  void onConstruct(Registry&, Entity) { ++constructions_; }

  SVGDocument document_;
  std::size_t constructions_ = 0;
};

/// Counts element restyles in a document: each restyle constructs a computed style.
class RestyleCounter : public ConstructionCounter<components::ComputedStyleComponent> {
public:
  using ConstructionCounter::ConstructionCounter;

  std::size_t restyles() const { return constructions(); }
};

/// Counts rendering-instance builds in a document: each render-tree rebuild constructs them anew.
using InstanceBuildCounter = ConstructionCounter<components::RenderingInstanceComponent>;

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

SVGDocument SpriteSheetOf(const ExternalUseDocument& host) {
  return SVGDocument::CreateFromHandle(*host.resource);
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

TEST(ExternalUseSpriteCorrectnessTest, EvictingADescendantForgetsItsRetainedAncestorsInputs) {
  // Drawing the child and then the group retains both, the child without inputs. The fillers then
  // take the remaining 30 of the 32 retention slots, and the last one evicts the least recently
  // drawn entry, the child, while the group stays retained.
  constexpr int kFillers = 31;
  std::string resource = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <g id="group"><rect id="child" width="8" height="8"/></g>)svg";
  for (int filler = 0; filler < kFillers; ++filler) {
    resource += "<rect id=\"t" + std::to_string(filler) + "\" width=\"8\" height=\"8\"/>";
  }
  resource += "</svg>";
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use id="tile-use" href="sprites.svg#child" fill="red"/>
      </svg>)svg",
      std::move(resource));
  ASSERT_TRUE(host.has_value());
  SVGUseElement use = host->document.querySelector("#tile-use")->cast<SVGUseElement>();
  Renderer renderer;
  renderer.draw(host->document);
  use.setHref(RcString("sprites.svg#group"));
  renderer.draw(host->document);
  for (int filler = 0; filler + 1 < kFillers; ++filler) {
    use.setHref(RcString("sprites.svg#t" + std::to_string(filler)));
    renderer.draw(host->document);
  }
  {
    RestyleCounter counter(SpriteSheetOf(*host));
    use.setHref(RcString("sprites.svg#t" + std::to_string(kFillers - 1)));
    renderer.draw(host->document);
    EXPECT_EQ(counter.restyles(), 2u) << "the last filler and the child it evicts";
  }
  {
    RestyleCounter counter(SpriteSheetOf(*host));
    use.setHref(RcString("sprites.svg#group"));
    renderer.draw(host->document);
    // The eviction forgets the inputs of the overlapping group, so the group and its child restyle
    // once more. The pixels do not depend on that restyle: the evicted child was restyled by
    // inheriting from the group, which is still styled for its occurrence.
    EXPECT_EQ(counter.restyles(), 2u) << "the group's restyle after its child's eviction";
  }

  const RendererBitmap expected = RenderStandalone(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="red"/>
      </svg>)svg");
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "external-use-evicted-descendant",
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
  EXPECT_THAT(style.fill.get().value(),
              testing::Eq(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 0xFF))))))
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

// A descendant's rem lengths resolve against the sheet root's own font size, the 12px initial
// value here, not one an earlier occurrence of the root inherited from its host.
TEST(ExternalUseRetentionTest, DescendantAfterRootOccurrenceResolvesRemAgainstSheetRoot) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64">
        <use href="sprites.svg#sheet" font-size="8"/>
        <use href="sprites.svg#child" x="64"/>
      </svg>)svg",
      R"svg(<svg id="sheet" xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <rect id="child" width="64" height="64" fill="blue" clip-path="inset(1rem) fill-box"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64">
        <rect width="64" height="64" fill="blue" clip-path="inset(8px) fill-box"/>
        <g transform="translate(64 0)">
          <rect width="64" height="64" fill="blue" clip-path="inset(12px) fill-box"/>
        </g>
      </svg>)svg",
               "descendant-after-root-rem");
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

  EXPECT_THAT(afterDraw.elementFromWorld(), testing::Eq(own.elementFromWorld()));
  EXPECT_THAT(afterDraw.transform(), testing::Eq(own.transform()));
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
  EXPECT_THAT(color, testing::Eq(css::Color(css::RGBA(0, 0, 0, 0xFF))))
      << "stop color reflects the host color";
}

TEST(ExternalUseRetentionTest, SymbolAtTwoSizesWithOnePaintKeepsEachViewport) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#sym" width="8" height="8"/>
        <use href="sprites.svg#sym" x="8" width="4" height="4"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <symbol id="sym" viewBox="0 0 4 4"><rect width="4" height="4" fill="blue"/></symbol>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="blue"/><rect x="8" width="4" height="4" fill="blue"/>
      </svg>)svg",
               "symbol-two-sizes");
}

TEST(ExternalUseRetentionTest, RetainedTargetWithUseInsideDefsKeepsItsExpansion) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#t"/>
        <use href="sprites.svg#b" x="8"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><g id="t">
          <defs><rect id="r" width="8" height="8" fill="blue"/></defs><use href="#r"/>
        </g></defs>
        <rect id="b" width="8" height="8" fill="red"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="blue"/><rect x="8" width="8" height="8" fill="red"/>
      </svg>)svg",
               "use-inside-defs");
}

TEST(ExternalUseRetentionTest, UseTargetRootSurvivesInstanceOnlyRebuilds) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#t"/>
        <use href="sprites.svg#b" x="8"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><rect id="r" width="8" height="8" fill="blue"/></defs>
        <use id="t" href="#r"/>
        <rect id="b" width="8" height="8" fill="red"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ExpectFrames(host->document, renderer, 3,
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="blue"/><rect x="8" width="8" height="8" fill="red"/>
      </svg>)svg",
               "use-target-root");
}

/// Restyles of \p sheet over eight frames that only move one of two uses drawing different targets,
/// after two warm-up frames.
std::size_t CountRestylesWhileMovingOneOfTwoTargets(std::string sheet) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use id="u0" href="sprites.svg#s0" fill="red"/>
        <use href="sprites.svg#s1" x="8" fill="blue"/>
      </svg>)svg",
      std::move(sheet));
  if (!host) {
    return 0;
  }
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);
  RestyleCounter counter(SVGDocument::CreateFromHandle(*host->resource));
  SVGGraphicsElement use = host->document.querySelector("#u0")->cast<SVGGraphicsElement>();
  for (int frame = 1; frame <= 8; ++frame) {
    use.setTransform(Transform2d::Translate(Vector2d(frame * 1.0, 0)));
    renderer.draw(host->document);
  }
  return counter.restyles();
}

TEST(ExternalUseSpriteCorrectnessTest, AlternatingTargetsWithInnerUsesDoNotRestyle) {
  EXPECT_EQ(CountRestylesWhileMovingOneOfTwoTargets(
                R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <g id="s0"><defs><circle id="c0" r="2"/></defs><use href="#c0" x="2" y="2"/></g>
    <g id="s1"><defs><circle id="c1" r="2"/></defs><use href="#c1" x="4" y="4"/></g></svg>)svg"),
            0u);
}

TEST(ExternalUseSpriteCorrectnessTest, AlternatingTargetsWithDormantDefinitionUseDoNotRestyle) {
  EXPECT_EQ(CountRestylesWhileMovingOneOfTwoTargets(
                R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <g id="s0"><defs><circle id="c0" r="2"/><use id="d0" href="#c0"/></defs>
      <use href="#c0" x="2" y="2"/></g>
    <g id="s1"><defs><circle id="c1" r="2"/></defs><use href="#c1" x="4" y="4"/></g></svg>)svg"),
            0u);
}

TEST(ExternalUseSpriteCorrectnessTest, AlternatingTargetsWithCyclicUsesDoNotRestyle) {
  EXPECT_EQ(CountRestylesWhileMovingOneOfTwoTargets(
                R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <g id="s0"><use id="a0" href="#b0"/><use id="b0" href="#a0"/><circle r="2"/></g>
    <g id="s1"><defs><circle id="c1" r="2"/></defs><use href="#c1" x="4" y="4"/></g></svg>)svg"),
            0u);
}

// Pins the cost of drawing a sheet's root and one of its descendants on every frame. Each draw of
// the descendant restores the root, which is styled for its own occurrence, so the sheet's three
// elements restyle twice per frame: for the root's occurrence, and again when the descendant's
// draw restores the root. Eight moved frames measure 8 x (3 + 3) = 48 restyles. Keeping the root
// styled for its occurrence would cost 8 x (3 + 1) = 32, but the descendant would then resolve rem
// lengths against the root's occurrence font size.
TEST(ExternalUseSpriteCorrectnessTest, RootAndDescendantDrawnEveryFrameRestyleTheSheetTwice) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use id="root-use" href="sprites.svg#sheet" fill="red"/>
        <use href="sprites.svg#child" x="8" fill="blue"/>
      </svg>)svg",
      R"svg(<svg id="sheet" xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="child" width="8" height="8"/><rect id="other" width="4" height="4"/>
      </svg>)svg");
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  renderer.draw(host->document);
  RestyleCounter counter(SVGDocument::CreateFromHandle(*host->resource));
  SVGGraphicsElement use = host->document.querySelector("#root-use")->cast<SVGGraphicsElement>();
  for (int frame = 1; frame <= 8; ++frame) {
    use.setTransform(Transform2d::Translate(Vector2d(frame * 1.0, 0)));
    renderer.draw(host->document);
  }
  EXPECT_EQ(counter.restyles(), 48u);
}

/// A sheet whose stylesheet gives an element of the target an invalid fill, which only computing
/// that element's style reports.
constexpr std::string_view kStyleWarningSheet =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <style>.bad { fill: nonsense(1) }</style>
        <g id="a"><rect class="bad" width="8" height="8"/><rect id="other" width="4" height="4"/></g>
      </svg>)svg";

constexpr std::string_view kStyleWarningHost =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use id="u" href="sprites.svg#a"/>
      </svg>)svg";

std::vector<std::string> WarningReasons(const ParseWarningSink& sink) {
  std::vector<std::string> reasons;
  for (const ParseDiagnostic& warning : sink.warnings()) {
    reasons.emplace_back(warning.reason.str());
  }
  return reasons;
}

/// Warnings the first draw of a freshly loaded \p hostSource reports to a collecting sink.
std::vector<std::string> FirstCollectingDrawWarnings(std::string_view hostSource,
                                                     std::string_view sheet) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(hostSource, std::string(sheet));
  if (!host) {
    return {};
  }
  Renderer renderer;
  ParseWarningSink warnings;
  renderer.draw(host->document, warnings);
  return WarningReasons(warnings);
}

/// How a draw that does not collect warnings is made.
enum class SinklessDraw {
  NoSink,        //!< `Renderer::draw` without a sink.
  DisabledSink,  //!< `Renderer::draw` with `ParseWarningSink::Disabled()`.
};

void DrawWithoutCollecting(Renderer& renderer, SVGDocument& document, SinklessDraw draw) {
  if (draw == SinklessDraw::NoSink) {
    renderer.draw(document);
  } else {
    ParseWarningSink disabled = ParseWarningSink::Disabled();
    renderer.draw(document, disabled);
  }
}

/// Expects a draw that collects no warnings, then a collecting draw, to report what the first
/// collecting draw of a fresh document reports.
void ExpectLaterSinkReceivesFirstDrawWarnings(std::string_view hostSource, std::string_view sheet) {
  const std::vector<std::string> expected = FirstCollectingDrawWarnings(hostSource, sheet);
  ASSERT_THAT(expected, testing::Not(testing::IsEmpty()));
  for (const SinklessDraw draw : {SinklessDraw::NoSink, SinklessDraw::DisabledSink}) {
    SCOPED_TRACE(draw == SinklessDraw::NoSink ? "no sink" : "disabled sink");
    std::optional<ExternalUseDocument> host =
        MakeExternalUseDocument(hostSource, std::string(sheet));
    ASSERT_TRUE(host.has_value());
    Renderer renderer;
    DrawWithoutCollecting(renderer, host->document, draw);
    ParseWarningSink later;
    renderer.draw(host->document, later);
    EXPECT_THAT(WarningReasons(later), testing::UnorderedElementsAreArray(expected));
  }
}

TEST(ExternalUseWarningReplayTest, StyleComputationWarningReachesALaterSink) {
  ExpectLaterSinkReceivesFirstDrawWarnings(kStyleWarningHost, kStyleWarningSheet);
}

/// A host whose `<use>` inherits a marker from the host document, which the sheet refuses.
constexpr std::string_view kMarkerHost =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><marker id="m" markerUnits="userSpaceOnUse" markerWidth="6" markerHeight="6"
          refX="3" refY="3"><rect width="6" height="6" fill="blue"/></marker></defs>
        <use id="u" href="sprites.svg#path" fill="none" stroke="black" marker-start="url(#m)"/>
      </svg>)svg";
constexpr std::string_view kMarkerSheet =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <path id="path" d="M4,4 L12,4"/>
      </svg>)svg";
constexpr std::string_view kHostRefusal =
    "External-use inherited cross-document URL resource is unsupported";

TEST(ExternalUseWarningReplayTest, InheritedResourceRefusalReachesALaterSink) {
  EXPECT_THAT(
      FirstCollectingDrawWarnings(kMarkerHost, kMarkerSheet),
      testing::IsSupersetOf({std::string("External-use inherited URL resources are unsupported"),
                             std::string(kHostRefusal)}));
  ExpectLaterSinkReceivesFirstDrawWarnings(kMarkerHost, kMarkerSheet);
}

// The host reports the refusal once per collecting draw, as it does when every draw collects.
TEST(ExternalUseWarningReplayTest, HostRefusalReachesACollectingDrawOnce) {
  constexpr std::string_view kHost =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><marker id="m" markerUnits="userSpaceOnUse" markerWidth="6" markerHeight="6"
          refX="3" refY="3"><rect width="6" height="6" fill="blue"/></marker></defs>
        <use href="sprites.svg#path" fill="none" stroke="black" marker-start="url(#m)"/>
        <use href="sprites.svg#path" fill="none" stroke="blue" marker-start="url(#m)"/>
      </svg>)svg";
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kHost, std::string(kMarkerSheet));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  ParseWarningSink later;
  renderer.draw(host->document, later);
  EXPECT_THAT(WarningReasons(later), testing::Contains(std::string(kHostRefusal)).Times(1));
}

TEST(ExternalUseWarningReplayTest, HostMutationDropsTheHostRefusal) {
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kMarkerHost, std::string(kMarkerSheet));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  host->document.querySelector("#u")->removeAttribute(xml::XMLQualifiedNameRef("marker-start"));
  ParseWarningSink later;
  renderer.draw(host->document, later);
  EXPECT_THAT(WarningReasons(later), testing::Not(testing::Contains(std::string(kHostRefusal))));
}

/// Restyles and rendering-instance builds of a sheet over a sequence of frames.
struct SheetCost {
  std::size_t restyles = 0;
  std::size_t instanceBuilds = 0;

  bool operator==(const SheetCost&) const = default;

  friend std::ostream& operator<<(std::ostream& os, const SheetCost& cost) {
    return os << "{restyles=" << cost.restyles << ", instanceBuilds=" << cost.instanceBuilds << "}";
  }
};

/// How a frame mutates the sheet of \ref kStyleWarningSheet before it is drawn.
enum class SheetMutation {
  OtherTransform,    //!< Moves the element without a warning.
  WarningTransform,  //!< Moves the element whose style warns.
  WarningStyle,      //!< Restyles the element whose style warns, which finds its warning again.
};

void MutateSheet(SVGDocument sheet, SheetMutation mutation, int frame) {
  const std::string_view selector = mutation == SheetMutation::OtherTransform ? "#other" : ".bad";
  SVGElement element = *sheet.querySelector(selector);
  if (mutation == SheetMutation::WarningStyle) {
    element.setAttribute(xml::XMLQualifiedNameRef("fill-opacity"), frame % 2 == 0 ? "0.5" : "0.6");
  } else {
    element.cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(frame + 1.0, 0)));
  }
}

/**
 * Cost of six frames that each mutate the sheet and then draw, after two warm-up draws.
 *
 * @param hostSource Host document drawing \ref kStyleWarningSheet.
 * @param firstDrawCollects Whether the first warm-up draw delivers the sheet's warning.
 * @param framesCollect Whether the warm-up draw after it and every measured frame collect.
 * @param mutation How each frame mutates the sheet.
 */
SheetCost MutatedFramesCost(std::string_view hostSource, bool firstDrawCollects, bool framesCollect,
                            SheetMutation mutation) {
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(hostSource, std::string(kStyleWarningSheet));
  if (!host) {
    ADD_FAILURE() << "host did not parse";
    return {};
  }
  Renderer renderer;
  const auto draw = [&](bool collect) {
    ParseWarningSink warnings = collect ? ParseWarningSink() : ParseWarningSink::Disabled();
    renderer.draw(host->document, warnings);
  };
  draw(firstDrawCollects);
  draw(framesCollect);
  RestyleCounter restyles(SpriteSheetOf(*host));
  InstanceBuildCounter instanceBuilds(SpriteSheetOf(*host));
  for (int frame = 0; frame < 6; ++frame) {
    MutateSheet(SpriteSheetOf(*host), mutation, frame);
    draw(framesCollect);
  }
  return {.restyles = restyles.restyles(), .instanceBuilds = instanceBuilds.constructions()};
}

// Mutating a sheet whose warning is held, or was delivered, costs a sinkless draw only what the
// mutation dirtied, the same as a collecting draw: dropping a held warning never restyles the
// sheet. Two uses of the target with different fills restyle it, and find its warning, on every
// draw.
TEST(ExternalUseWarningReplayTest, SinklessDrawsOfAMutatedSheetCostOnlyTheMutation) {
  constexpr std::string_view kRestyledEveryDraw =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="sprites.svg#a" fill="red"/><use href="sprites.svg#a" x="8" fill="blue"/>
      </svg>)svg";
  for (const std::string_view hostSource : {kStyleWarningHost, kRestyledEveryDraw}) {
    for (const SheetMutation mutation :
         {SheetMutation::OtherTransform, SheetMutation::WarningTransform,
          SheetMutation::WarningStyle}) {
      const SheetCost collecting = MutatedFramesCost(hostSource, true, true, mutation);
      for (const bool delivered : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << (hostSource == kStyleWarningHost ? "one use" : "two fills") << ", mutation "
                     << static_cast<int>(mutation)
                     << (delivered ? ", delivered" : ", never delivered"));
        EXPECT_EQ(MutatedFramesCost(hostSource, delivered, false, mutation), collecting);
      }
    }
  }
  EXPECT_EQ(
      MutatedFramesCost(kStyleWarningHost, true, true, SheetMutation::OtherTransform).restyles, 0u)
      << "a transform restyles nothing";
}

// The packaged fallback font warning is reported once per document, which its marker records.
// When a mutation drops held warnings, possibly that report among them, the marker is removed so
// the report is made again; a sheet whose held warnings were not dropped keeps it.
TEST(ExternalUseWarningReplayTest, DroppingHeldWarningsLetsTheFallbackFontReportRepeat) {
  for (const bool mutate : {false, true}) {
    SCOPED_TRACE(mutate ? "mutated" : "not mutated");
    std::optional<ExternalUseDocument> host =
        MakeExternalUseDocument(kStyleWarningHost, std::string(kStyleWarningSheet));
    ASSERT_TRUE(host.has_value());
    Renderer renderer;
    renderer.draw(host->document);
    SVGDocument sheet = SpriteSheetOf(*host);
    sheet.registry().ctx().insert_or_assign(components::MissingFallbackFontReported{});
    if (mutate) {
      MutateSheet(sheet, SheetMutation::OtherTransform, 0);
    }
    renderer.draw(host->document);
    EXPECT_EQ(sheet.registry().ctx().contains<components::MissingFallbackFontReported>(), !mutate);
  }
}

// A draw that collects no warnings between collecting draws must not make the next collecting draw
// restyle or rebuild a retained sheet that nothing changed.
TEST(ExternalUseWarningReplayTest, TogglingTheSinkDoesNotRestyleOrRebuildTheSheet) {
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kStyleWarningHost, std::string(kStyleWarningSheet));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ParseWarningSink first;
  renderer.draw(host->document, first);
  ASSERT_THAT(WarningReasons(first), testing::Not(testing::IsEmpty()));
  renderer.draw(host->document);

  RestyleCounter restyles(SpriteSheetOf(*host));
  InstanceBuildCounter instanceBuilds(SpriteSheetOf(*host));
  for (int frame = 0; frame < 8; ++frame) {
    if (frame % 2 == 0) {
      ParseWarningSink collecting;
      renderer.draw(host->document, collecting);
      EXPECT_THAT(WarningReasons(collecting), testing::IsEmpty()) << "frame " << frame;
    } else {
      DrawWithoutCollecting(renderer, host->document,
                            frame % 4 == 1 ? SinklessDraw::NoSink : SinklessDraw::DisabledSink);
    }
  }
  EXPECT_EQ(restyles.restyles(), 0u);
  EXPECT_EQ(instanceBuilds.constructions(), 0u);
}

TEST(ExternalUseWarningReplayTest, MutationDropsWarningsHeldFromBeforeIt) {
  for (const bool sinklessDrawAfterMutation : {false, true}) {
    SCOPED_TRACE(sinklessDrawAfterMutation ? "sinkless draw after the mutation"
                                           : "collecting draw right after the mutation");
    std::optional<ExternalUseDocument> host =
        MakeExternalUseDocument(kStyleWarningHost, std::string(kStyleWarningSheet));
    ASSERT_TRUE(host.has_value());
    Renderer renderer;
    renderer.draw(host->document);
    SpriteSheetOf(*host).querySelector(".bad")->setAttribute(xml::XMLQualifiedNameRef("class"),
                                                             "good");
    if (sinklessDrawAfterMutation) {
      renderer.draw(host->document);
    }
    ParseWarningSink later;
    renderer.draw(host->document, later);
    EXPECT_THAT(WarningReasons(later), testing::IsEmpty());
  }
}

TEST(ExternalUseWarningReplayTest, UnrelatedMutationKeepsCurrentWarningsForALaterSink) {
  const std::vector<std::string> expected =
      FirstCollectingDrawWarnings(kStyleWarningHost, kStyleWarningSheet);
  ASSERT_THAT(expected, testing::Not(testing::IsEmpty()));
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kStyleWarningHost, std::string(kStyleWarningSheet));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  // A transform change restyles nothing, so only a restyle the mutation forces finds the
  // stylesheet's warning again.
  SpriteSheetOf(*host).querySelector("#other")->cast<SVGGraphicsElement>().setTransform(
      Transform2d::Translate(Vector2d(1, 0)));
  ParseWarningSink later;
  renderer.draw(host->document, later);
  EXPECT_THAT(WarningReasons(later), testing::UnorderedElementsAreArray(expected));
}

// Two occurrences of one target with different fills restyle it for every draw, so each draw finds
// the same warnings again.
TEST(ExternalUseWarningReplayTest, CollectingSinkReceivesEachWarningOnce) {
  constexpr std::string_view kHost =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use id="u" href="sprites.svg#a" fill="red"/>
        <use href="sprites.svg#a" x="8" fill="blue"/>
      </svg>)svg";
  const std::vector<std::string> expected = FirstCollectingDrawWarnings(kHost, kStyleWarningSheet);
  ASSERT_THAT(expected, testing::Not(testing::IsEmpty()));
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(kHost, std::string(kStyleWarningSheet));
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  SVGGraphicsElement use = host->document.querySelector("#u")->cast<SVGGraphicsElement>();
  std::vector<std::string> received;
  for (int frame = 0; frame < 4; ++frame) {
    use.setTransform(Transform2d::Translate(Vector2d(frame * 1.0, 0)));
    ParseWarningSink warnings;
    renderer.draw(host->document, warnings);
    const std::vector<std::string> reasons = WarningReasons(warnings);
    received.insert(received.end(), reasons.begin(), reasons.end());
  }
  EXPECT_THAT(received, testing::UnorderedElementsAreArray(expected));
}

TEST(ExternalUseWarningReplayTest, HeldWarningsKeepTheSinkLimit) {
  std::string sheet =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><g id="a">)svg";
  for (std::size_t i = 0; i <= ParseWarningSink::kMaximumWarnings; ++i) {
    sheet += "<use href=\"#missing" + std::to_string(i) + "\"/>";
  }
  sheet += "</g></svg>";
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(kStyleWarningHost, sheet);
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  renderer.draw(host->document);
  ParseWarningSink later;
  renderer.draw(host->document, later);
  EXPECT_THAT(later.warnings(), testing::SizeIs(ParseWarningSink::kMaximumWarnings));
  EXPECT_TRUE(later.resourceLimitExceeded());
}

// Once a sheet over the sink limit has delivered its warnings and the truncation, finding them
// again holds nothing, so a mutation followed by a sinkless draw leaves no held warnings for the
// next mutation to drop. A transform restyles nothing, so neither draw restyles the sheet.
TEST(ExternalUseWarningReplayTest, DeliveredTruncationIsNotHeldAgain) {
  std::string sheet = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">)svg"
                      R"svg(<g id="a"><rect id="other" width="4" height="4"/>)svg";
  for (std::size_t i = 0; i <= ParseWarningSink::kMaximumWarnings; ++i) {
    sheet += "<use href=\"#missing" + std::to_string(i) + "\"/>";
  }
  sheet += "</g></svg>";
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(kStyleWarningHost, sheet);
  ASSERT_TRUE(host.has_value());
  Renderer renderer;
  ParseWarningSink first;
  renderer.draw(host->document, first);
  ASSERT_TRUE(first.resourceLimitExceeded());

  RestyleCounter restyles(SpriteSheetOf(*host));
  MutateSheet(SpriteSheetOf(*host), SheetMutation::OtherTransform, 0);
  renderer.draw(host->document);
  MutateSheet(SpriteSheetOf(*host), SheetMutation::OtherTransform, 1);
  ParseWarningSink later;
  renderer.draw(host->document, later);
  EXPECT_EQ(restyles.restyles(), 0u);
}

}  // namespace
}  // namespace donner::svg::tests
