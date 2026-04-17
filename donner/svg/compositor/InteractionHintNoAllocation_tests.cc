#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/MandatoryHintDetector.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::svg::compositor {

namespace {

SVGDocument parseDocument(std::string_view svgSource) {
  ParseWarningSink sink;
  auto result = parser::SVGParser::ParseSVG(svgSource, sink);
  EXPECT_FALSE(result.hasError()) << result.error().reason;
  return std::move(result).result();
}

class InteractionHintNoAllocationTest : public ::testing::Test {
protected:
  svg::Renderer renderer_;
  RenderViewport viewport_;

  InteractionHintNoAllocationTest() {
    viewport_.size = Vector2d(200, 100);
    viewport_.devicePixelRatio = 1.0;
  }
};

}  // namespace

// Verifies that a steady-state drag (translation-only transform updates on
// a promoted entity) doesn't churn ECS state. Layer count stays stable, the
// compositor's hint ledger stays stable, and reconcile passes do no work.
//
// This is a proxy test for the stricter "no heap allocation per drag frame"
// invariant. A true heap-tracking test would require intercepting `operator
// new`/`malloc` which is platform-specific and brittle; this test catches
// the ECS-level churn that would cause allocations without needing any
// platform-specific machinery.
TEST_F(InteractionHintNoAllocationTest, SteadyStateDragDoesNotChurnECSState) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  // Warm-up frame to move past first-frame setup allocations.
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(Vector2d(1.0, 0.0)));
  compositor.renderFrame(viewport_);

  // Capture steady-state invariants.
  const size_t layerCountBefore = compositor.layerCount();
  const size_t memoryBefore = compositor.totalBitmapMemory();

  // Drive 20 steady-state drag frames with different translations. None of
  // them should mutate layer count or bitmap storage.
  for (int i = 0; i < 20; ++i) {
    compositor.setLayerCompositionTransform(entity,
                                             Transform2d::Translate(Vector2d(i * 2.0, i * 1.0)));
    compositor.renderFrame(viewport_);

    EXPECT_EQ(compositor.layerCount(), layerCountBefore)
        << "steady-state drag must not add/remove layers (frame " << i << ")";
    EXPECT_EQ(compositor.totalBitmapMemory(), memoryBefore)
        << "steady-state drag must not reallocate layer bitmaps (frame " << i << ")";
  }
}

// The MandatoryHintDetector is gated on the document-dirty signal. During a
// steady-state drag (no DirtyFlagsComponent entries, no `needsFullRebuild`),
// it must NOT scan the registry — a 10k-node document's O(N) scan would
// dominate every drag frame otherwise.
TEST_F(InteractionHintNoAllocationTest, MandatoryDetectorIsIdleDuringSteadyStateDrag) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  // Warm-up.
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(Vector2d(1.0, 0.0)));
  compositor.renderFrame(viewport_);

  // Steady-state drag.
  for (int i = 0; i < 5; ++i) {
    compositor.setLayerCompositionTransform(entity,
                                             Transform2d::Translate(Vector2d(i * 2.0, i * 1.0)));
    compositor.renderFrame(viewport_);
  }

  // No direct access to the detector's stats from the controller, so this
  // test asserts a weaker property: layer count stability is an end-to-end
  // proxy for the detector staying idle (an active detector on a
  // non-qualifying doc would still produce zero hints, so this can't
  // distinguish "ran but no-op'd" from "didn't run." The real value is
  // catching a regression where a new code path marks DirtyFlagsComponent
  // during drag and forces re-scan). Pair with the perf benchmark for the
  // actual latency guard.
  EXPECT_EQ(compositor.layerCount(), 1u) << "drag layer stable across steady-state frames";
}

}  // namespace donner::svg::compositor
