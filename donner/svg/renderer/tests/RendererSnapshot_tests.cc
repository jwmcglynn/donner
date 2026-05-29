#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <vector>

#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::AtLeast;

namespace donner::svg {
namespace {

using MockRendererInterface = tests::MockRendererInterface;

SVGDocument MakeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
  return instantiateSubtree(svg, parser::SVGParser::Options(), size);
}

std::optional<css::RGBA> SolidFillColor(const PaintParams& paint) {
  const auto* solid = std::get_if<PaintServer::Solid>(&paint.fill);
  if (solid == nullptr) {
    return std::nullopt;
  }

  return solid->color.resolve(paint.currentColor.rgba(), paint.fillOpacity);
}

const components::PaintResolvedReference* PaintReference(
    const components::ResolvedPaintServer& paint) {
  return std::get_if<components::PaintResolvedReference>(&paint);
}

TEST(RendererSnapshotTests, ReplayingCapturedSnapshotIgnoresLaterDomMutations) {
  SVGDocument document = MakeDocument(R"svg(
    <rect x="1" y="2" width="8" height="6" fill="red" />
  )svg");

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver(renderer);

  RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
  ASSERT_GT(snapshot.commandCount(), 0u);
  EXPECT_GE(snapshot.estimatedCommandStorageBytes(), snapshot.commandCount());

  std::optional<SVGElement> maybeRect = document.querySelector("rect");
  ASSERT_TRUE(maybeRect.has_value());
  maybeRect->setAttribute("fill", "blue");

  std::vector<css::RGBA> replayedFills;
  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setPaint(_)).WillRepeatedly([&](const PaintParams& paint) {
    if (std::optional<css::RGBA> color = SolidFillColor(paint)) {
      replayedFills.push_back(*color);
    }
  });
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  driver.draw(snapshot);

  ASSERT_FALSE(replayedFills.empty());
  EXPECT_EQ(replayedFills.back(), css::RGBA(255, 0, 0, 255));
}

TEST(RendererSnapshotTests, LaterSnapshotObservesLaterDomRevision) {
  SVGDocument document = MakeDocument(R"svg(
    <rect x="1" y="2" width="8" height="6" fill="red" />
  )svg");

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver(renderer);

  RenderSnapshot initialSnapshot = driver.captureRenderSnapshot(document);

  std::optional<SVGElement> maybeRect = document.querySelector("rect");
  ASSERT_TRUE(maybeRect.has_value());
  maybeRect->setAttribute("fill", "blue");

  RenderSnapshot laterSnapshot = driver.captureRenderSnapshot(document);
  EXPECT_GT(laterSnapshot.sourceRevision(), initialSnapshot.sourceRevision());

  std::vector<css::RGBA> replayedFills;
  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setPaint(_)).WillRepeatedly([&](const PaintParams& paint) {
    if (std::optional<css::RGBA> color = SolidFillColor(paint)) {
      replayedFills.push_back(*color);
    }
  });
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  driver.draw(laterSnapshot);

  ASSERT_FALSE(replayedFills.empty());
  EXPECT_EQ(replayedFills.back(), css::RGBA(0, 0, 255, 255));
}

TEST(RendererSnapshotTests, ConcurrentDomMutationsCanCompleteWhileSnapshotReplays) {
  SVGDocument document = MakeDocument(R"svg(
    <rect x="1" y="2" width="8" height="6" fill="red" />
  )svg");
  std::optional<SVGElement> maybeRect = document.querySelector("rect");
  ASSERT_TRUE(maybeRect.has_value());
  SVGRectElement rect = maybeRect->cast<SVGRectElement>();
  SVGSVGElement root = document.svgElement();

  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver(renderer);

  std::promise<void> replayStarted;
  std::promise<void> releaseReplay;
  std::shared_future<void> releaseReplayFuture = releaseReplay.get_future().share();
  std::atomic<bool> replayCallbackHadWriteAccess = false;

  EXPECT_CALL(renderer, beginFrame(_)).WillOnce([&](const RenderViewport&) {
    replayCallbackHadWriteAccess.store(document.handle()->currentThreadHasWriteAccess(),
                                       std::memory_order_relaxed);
    replayStarted.set_value();
    releaseReplayFuture.wait();
  });
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  std::future<void> renderFinished =
      std::async(std::launch::async, [&]() { driver.draw(document); });

  ASSERT_EQ(replayStarted.get_future().wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  std::future<void> mutationFinished = std::async(std::launch::async, [&]() {
    rect.setX(Lengthd(42, Lengthd::Unit::None));
    SVGRectElement appended = SVGRectElement::Create(document);
    root.appendChild(appended);
    appended.remove();
  });

  EXPECT_EQ(mutationFinished.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  releaseReplay.set_value();
  EXPECT_EQ(renderFinished.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_FALSE(replayCallbackHadWriteAccess.load(std::memory_order_relaxed));
  rect.withReadAccess([rect](DocumentReadAccess&, EntityHandle) mutable {
    EXPECT_EQ(rect.x(), Lengthd(42, Lengthd::Unit::None));
  });
}

TEST(RendererSnapshotTests, GradientPaintReferencesAreSnapshotOwned) {
  SVGDocument document = MakeDocument(R"svg(
    <defs>
      <linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
        <stop offset="0" stop-color="red" />
        <stop offset="1" stop-color="red" />
      </linearGradient>
    </defs>
    <rect x="1" y="2" width="8" height="6" fill="url(#g) blue" />
  )svg");

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver(renderer);

  RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
  EXPECT_EQ(snapshot.liveRegistryReferenceCountForTesting(document.registry()), 0u);

  std::optional<SVGElement> maybeGradient = document.querySelector("linearGradient");
  ASSERT_TRUE(maybeGradient.has_value());
  maybeGradient->remove();
  EXPECT_EQ(snapshot.liveRegistryReferenceCountForTesting(document.registry()), 0u);

  std::vector<components::PaintResolvedReference> replayedFillRefs;
  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setPaint(_)).WillRepeatedly([&](const PaintParams& paint) {
    if (const auto* ref = PaintReference(paint.fill)) {
      replayedFillRefs.push_back(*ref);
    }
  });
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  driver.draw(snapshot);

  ASSERT_FALSE(replayedFillRefs.empty());
  const components::PaintResolvedReference& ref = replayedFillRefs.back();
  EXPECT_NE(ref.reference.handle.registry(), &document.registry());

  const auto* gradient = ref.reference.handle.try_get<components::ComputedGradientComponent>();
  ASSERT_NE(gradient, nullptr);
  ASSERT_FALSE(gradient->stops.empty());
  EXPECT_EQ(gradient->stops.front().color.resolve(css::RGBA(0, 0, 0, 255),
                                                  gradient->stops.front().opacity),
            css::RGBA(255, 0, 0, 255));
}

TEST(RendererSnapshotTests, PatternPaintReferencesAreSnapshotOwned) {
  SVGDocument document = MakeDocument(R"svg(
    <defs>
      <pattern id="p" patternUnits="userSpaceOnUse" width="4" height="4">
        <rect width="4" height="4" fill="red" />
      </pattern>
    </defs>
    <rect x="1" y="2" width="8" height="6" fill="url(#p) green" />
  )svg");

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver(renderer);

  RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
  EXPECT_EQ(snapshot.liveRegistryReferenceCountForTesting(document.registry()), 0u);

  std::optional<SVGElement> maybePattern = document.querySelector("pattern");
  ASSERT_TRUE(maybePattern.has_value());
  maybePattern->remove();
  EXPECT_EQ(snapshot.liveRegistryReferenceCountForTesting(document.registry()), 0u);

  std::vector<components::PaintResolvedReference> replayedPatternRefs;
  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setPaint(_)).WillRepeatedly([&](const PaintParams& paint) {
    if (const auto* ref = PaintReference(paint.fill);
        ref != nullptr &&
        ref->reference.handle.try_get<components::ComputedPatternComponent>() != nullptr) {
      replayedPatternRefs.push_back(*ref);
    }
  });
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  driver.draw(snapshot);

  ASSERT_FALSE(replayedPatternRefs.empty());
  const components::PaintResolvedReference& ref = replayedPatternRefs.back();
  EXPECT_NE(ref.reference.handle.registry(), &document.registry());
  EXPECT_NE(ref.reference.handle.try_get<components::ComputedPatternComponent>(), nullptr);
}

TEST(RendererSnapshotTests, FeImageFragmentReferencesAreClearedBeforeReplay) {
  SVGDocument document = MakeDocument(R"svg(
    <defs>
      <filter id="f">
        <feImage href="#source" />
      </filter>
      <rect id="source" x="0" y="0" width="2" height="2" fill="red" />
    </defs>
    <rect x="1" y="2" width="8" height="6" filter="url(#f)" fill="blue" />
  )svg");

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver(renderer);

  RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
  EXPECT_EQ(snapshot.liveRegistryReferenceCountForTesting(document.registry()), 0u);

  std::vector<components::FilterGraph> replayedFilterGraphs;
  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, pushFilterLayer(_, _))
      .WillRepeatedly([&](const components::FilterGraph& filterGraph, const std::optional<Box2d>&) {
        replayedFilterGraphs.push_back(filterGraph);
      });
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  driver.draw(snapshot);

  ASSERT_FALSE(replayedFilterGraphs.empty());
  ASSERT_FALSE(replayedFilterGraphs.front().nodes.empty());
  const auto* image = std::get_if<components::filter_primitive::Image>(
      &replayedFilterGraphs.front().nodes.front().primitive);
  ASSERT_NE(image, nullptr);
  EXPECT_TRUE(image->fragmentId.empty());
  EXPECT_FALSE(image->svgSubDocument);
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererSnapshotTests, TextPaintReferencesAreSnapshotOwned) {
  SVGDocument document = MakeDocument(R"svg(
    <defs>
      <linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
        <stop offset="0" stop-color="red" />
        <stop offset="1" stop-color="red" />
      </linearGradient>
    </defs>
    <text x="1" y="12" fill="url(#g) blue">Hello</text>
  )svg");

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver(renderer);

  RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
  EXPECT_EQ(snapshot.liveRegistryReferenceCountForTesting(document.registry()), 0u);

  std::optional<SVGElement> maybeGradient = document.querySelector("linearGradient");
  ASSERT_TRUE(maybeGradient.has_value());
  maybeGradient->remove();
  EXPECT_EQ(snapshot.liveRegistryReferenceCountForTesting(document.registry()), 0u);

  std::vector<components::PaintResolvedReference> replayedSpanFillRefs;
  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, drawText(_, _, _))
      .WillRepeatedly([&](Registry& registry, const components::ComputedTextComponent& text,
                          const TextParams&) {
        EXPECT_NE(&registry, &document.registry());
        for (const auto& span : text.spans) {
          if (const auto* ref = PaintReference(span.resolvedFill)) {
            replayedSpanFillRefs.push_back(*ref);
          }
        }
      });

  driver.draw(snapshot);

  ASSERT_FALSE(replayedSpanFillRefs.empty());
  const components::PaintResolvedReference& ref = replayedSpanFillRefs.back();
  EXPECT_NE(ref.reference.handle.registry(), &document.registry());
  EXPECT_NE(ref.reference.handle.try_get<components::ComputedGradientComponent>(), nullptr);
}
#endif

}  // namespace
}  // namespace donner::svg
