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

}  // namespace
}  // namespace donner::svg::tests
