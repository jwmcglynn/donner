/// @file
///
/// Golden-image regression test for composited drag promotion —
/// readds coverage that the old `FilterDragRepro_tests` (deleted with
/// the v1→v2 fixture migration) provided at a behavioural level, now
/// with pixel-level goldens so any regression in the composited
/// fast-path output trips loudly.
///
/// Replays `selection_composited_promote_test.rnr` against the real
/// editor stack with composited drag preview enabled, and diffs
/// bitmaps at three pipeline phases:
///
///   1. **Pre-drag** (frame before first mouse-down): flat bitmap —
///      the cold render the user sees before interacting.
///   2. **Mid-drag** (deep into drag gesture 2, where all three
///      split-static layers are active): the compositor's `bg` /
///      `promoted` / `fg` bitmaps are compared against goldens
///      individually, which is what the GPU composites for the
///      user's live drag view. Flat bitmap at this moment is
///      intentionally stale (see AsyncRenderer + compositor for the
///      `skipMainComposeDuringSplit` / split-layer cache rationale).
///   3. **Settle** (post-replay, selection cleared, one extra
///      render): the compositor demotes the entity and re-rasterizes
///      the flat bitmap against the mutated DOM — what the user sees
///      once they deselect.
///
/// Regenerating goldens after an intentional pixel-output change:
///
///   UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
///     bazel run //donner/editor/tests:composited_promote_replay_tests

#include <cmath>
#include <cstdint>
#include <filesystem>

#include "donner/editor/backend_lib/AsyncRenderer.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/editor/tests/ReproReplayHarness.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// Recording layout (from `selection_composited_promote_test_minimal.rnr`,
// a 14-frame trim of the original user `.rnr`):
//   f=0    viewport init + cold render baseline
//   f=210  pre-drag checkpoint (cold render before any gesture)
//   f=211  gesture 1 mdown, f=225 move, f=242 mup — short drag
//   f=378  pre-drag-2 idle
//   f=379  gesture 2 mdown, f=404 mid-drag checkpoint, f=428 mup — longest drag
//   f=579  pre-drag-3 idle
//   f=580  gesture 3 mdown, f=600 move, f=621 mup — near-zero drag
//   f=813  final frame (pre-settle)
//
// The drag delta at each kept frame is computed from the current
// mouse position minus the mdown mouse position, so skipping
// intermediate move frames doesn't change the DOM state visible at
// the checkpoints. The original `selection_composited_promote_test.rnr`
// is preserved for human inspection / longer-timeline rerecording.
constexpr std::uint64_t kPreDragFrame = 210;  // right before gesture 1 mdown (cold)
constexpr std::uint64_t kMidDragFrame = 404;  // mid gesture 2 — all split layers active

constexpr const char* kPreDragGolden =
    "donner/editor/tests/testdata/composited_promote_pre_drag.png";
constexpr const char* kMidDragBgGolden =
    "donner/editor/tests/testdata/composited_promote_mid_drag_bg.png";
constexpr const char* kMidDragPromotedGolden =
    "donner/editor/tests/testdata/composited_promote_mid_drag_promoted.png";
constexpr const char* kMidDragFgGolden =
    "donner/editor/tests/testdata/composited_promote_mid_drag_fg.png";
constexpr const char* kSettleGolden =
    "donner/editor/tests/testdata/composited_promote_settle.png";

TEST(CompositedPromoteReplayTest, GoldenImagesAcrossDragLifecycle) {
  const std::filesystem::path reproPath =
      "donner/editor/tests/selection_composited_promote_test_minimal.rnr";
  const std::filesystem::path svgPath = "donner_splash.svg";

  if (!std::filesystem::exists(reproPath) || !std::filesystem::exists(svgPath)) {
    GTEST_SKIP() << "Required data files not available in runfiles: " << reproPath << " or "
                 << svgPath;
  }

  svg::RendererBitmap preDragBitmap;
  svg::RendererBitmap midDragBg;
  svg::RendererBitmap midDragPromoted;
  svg::RendererBitmap midDragFg;
  Vector2d midDragPromotedTranslation = Vector2d::Zero();
  bool preDragLanded = false;
  bool midDragLanded = false;
  bool midDragHasComposite = false;

  ReplayConfig config;
  // Always on — this test's entire reason to exist is to regress-check
  // the composited drag-preview pipeline. The recording's metadata
  // may or may not carry `exp=1`; either way, turn composited drag
  // preview on so the compositor fast path fires.
  config.forceCompositedDragPreview = true;
  config.appendSettleFrame = true;
  config.checkpointFrames = {kPreDragFrame, kMidDragFrame};
  config.onCheckpoint = [&](std::size_t checkpointIdx, const RenderResult* result) {
    if (result == nullptr) {
      ADD_FAILURE() << "checkpoint " << checkpointIdx
                    << " had no render result — renderer was busy at that frame";
      return;
    }
    switch (checkpointIdx) {
      case 0:
        preDragBitmap = result->bitmap;
        preDragLanded = true;
        break;
      case 1:
        midDragLanded = true;
        if (result->compositedPreview.has_value()) {
          midDragHasComposite = true;
          midDragBg = result->compositedPreview->backgroundBitmap;
          midDragPromoted = result->compositedPreview->promotedBitmap;
          midDragFg = result->compositedPreview->foregroundBitmap;
          midDragPromotedTranslation = result->compositedPreview->promotedTranslationDoc;
        }
        break;
      default:
        ADD_FAILURE() << "unexpected checkpoint index " << checkpointIdx;
        break;
    }
  };

  ReplayResults results = ReplayRepro(reproPath, svgPath, config);
  ASSERT_EQ(results.mouseDownFrameIndices.size(), 3u) << "expected exactly three gestures";
  ASSERT_EQ(results.mouseUpFrameIndices.size(), 3u);

  ASSERT_TRUE(preDragLanded) << "pre-drag checkpoint (frame " << kPreDragFrame
                             << ") did not land a render result";
  ASSERT_TRUE(midDragLanded) << "mid-drag checkpoint (frame " << kMidDragFrame
                             << ") did not land a render result";
  ASSERT_TRUE(midDragHasComposite)
      << "mid-drag checkpoint (frame " << kMidDragFrame
      << ") did not produce a compositedPreview — the compositor fast path never "
         "engaged. Check that `forceCompositedDragPreview = true` is actually "
         "promoting the dragged entity.";
  ASSERT_TRUE(results.settleRender.has_value())
      << "settle render (post-replay) did not land — the harness failed to "
         "demote + re-rasterize after the final gesture";

  // Mid-drag promotion must have carried a nonzero translation —
  // otherwise the drag did nothing and the "composited" label is a
  // false positive. Gesture 2 is ~5 doc units in the recording; any
  // threshold above zero distinguishes real drag from no drag.
  EXPECT_GT(std::hypot(midDragPromotedTranslation.x, midDragPromotedTranslation.y), 0.0)
      << "compositedPreview reported a zero translation mid-drag — the "
         "composition-transform fast path didn't track the cursor. Translation: ("
      << midDragPromotedTranslation.x << ", " << midDragPromotedTranslation.y << ")";

  // Tight thresholds (threshold 0.01, max 200 mismatched) on the
  // composited layers — these are cached bitmaps from deterministic
  // rasterization, so there's no AA variation between runs; any pixel
  // drift is a real regression. The slightly-widened max-pixels
  // (200 vs. default 100) covers small rounding around filter-layer
  // edges if filter-bounds math shifts by a subpixel.
  tests::BitmapGoldenCompareParams strict{.threshold = 0.01f, .maxMismatchedPixels = 200};

  tests::CompareBitmapToGolden(preDragBitmap, kPreDragGolden, "pre_drag");
  tests::CompareBitmapToGolden(midDragBg, kMidDragBgGolden, "mid_drag_bg", strict);
  tests::CompareBitmapToGolden(midDragPromoted, kMidDragPromotedGolden, "mid_drag_promoted",
                               strict);
  tests::CompareBitmapToGolden(midDragFg, kMidDragFgGolden, "mid_drag_fg", strict);
  tests::CompareBitmapToGolden(results.settleRender->bitmap, kSettleGolden, "settle");
}

}  // namespace
}  // namespace donner::editor
