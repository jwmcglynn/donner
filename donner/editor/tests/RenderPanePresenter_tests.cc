#include "donner/editor/RenderPanePresenter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/Vector2.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/ViewportState.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

/// Worst-case outward miter offset (in stroke half-widths) that ImGui's
/// thick-line stroker would produce at the interior joints of `points` when
/// stroked as a single mitered run between [first, last]. This replicates
/// ImGui's `IM_FIXNORMAL2F` (cap inverse-length at 100) exactly, so it is a
/// faithful proxy for the visible "flare" spike. Returns a multiple of
/// half-width: 1.0 is a flat edge; >1 is an outward miter spike.
double WorstMiterOffsetHalfWidths(const std::vector<Vector2d>& points, std::size_t first,
                                  std::size_t last) {
  const auto unitNormal = [](const Vector2d& a, const Vector2d& b) {
    const Vector2d d = b - a;
    const double len = d.length();
    if (len < 1e-9) {
      return Vector2d(0.0, 0.0);
    }
    return Vector2d(d.y / len, -d.x / len);  // ImGui's left-normal convention.
  };

  double worst = 1.0;
  for (std::size_t i = first + 1; i + 1 <= last; ++i) {
    const Vector2d n1 = unitNormal(points[i - 1], points[i]);
    const Vector2d n2 = unitNormal(points[i], points[i + 1]);
    double dmx = (n1.x + n2.x) * 0.5;
    double dmy = (n1.y + n2.y) * 0.5;
    double d2 = dmx * dmx + dmy * dmy;
    if (d2 > 0.000001) {
      double invLen2 = 1.0 / d2;
      if (invLen2 > 100.0) {  // IM_FIXNORMAL2F_MAX_INVLEN2
        invLen2 = 100.0;
      }
      dmx *= invLen2;
      dmy *= invLen2;
    }
    worst = std::max(worst, std::sqrt(dmx * dmx + dmy * dmy));
  }
  return worst;
}

/// Worst miter offset across every run the fix would emit for `polyline`, given
/// the break indices `OverlayMiterBreakIndices` returns. Mirrors the run
/// splitting in `StrokeDocumentPath`.
double WorstMiterOffsetAcrossRuns(const OverlayStrokePolyline& polyline) {
  const std::vector<Vector2d>& pts = polyline.points;
  if (pts.size() < 2) {
    return 1.0;
  }
  const std::vector<std::size_t> breaks = OverlayMiterBreakIndices(pts, polyline.closed);
  if (breaks.empty()) {
    return WorstMiterOffsetHalfWidths(pts, 0, pts.size() - 1);
  }

  double worst = 1.0;
  if (polyline.closed) {
    const std::size_t n = pts.size();
    for (std::size_t b = 0; b < breaks.size(); ++b) {
      const std::size_t start = breaks[b];
      const std::size_t end = breaks[(b + 1) % breaks.size()];
      // Materialize the run (handles wrap-around) and analyze it as open.
      std::vector<Vector2d> run;
      std::size_t i = start;
      while (true) {
        run.push_back(pts[i]);
        if (i == end) {
          break;
        }
        i = (i + 1) % n;
      }
      worst = std::max(worst, WorstMiterOffsetHalfWidths(run, 0, run.size() - 1));
    }
  } else {
    std::size_t runStart = 0;
    for (const std::size_t breakIdx : breaks) {
      worst = std::max(worst, WorstMiterOffsetHalfWidths(pts, runStart, breakIdx));
      runStart = breakIdx;
    }
    worst = std::max(worst, WorstMiterOffsetHalfWidths(pts, runStart, pts.size() - 1));
  }
  return worst;
}

TEST(RenderPanePresenterTest, SuppressedDragTargetTileForSameLayerEntityIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(42);
  tile.isDragTarget = true;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, entt::null));
  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, SuppressedLayerEntityTileIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(42);
  tile.isDragTarget = false;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, SuppressedImmediateEntityTileIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Immediate;
  tile.layerEntity = static_cast<Entity>(42);
  tile.isDragTarget = false;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)))
      << "Immediate-mode promoted layers still carry layerEntity ownership, so deleting or hiding "
         "the selection must suppress them just like cached layer tiles.";
}

TEST(RenderPanePresenterTest, HiddenSelectionSuppressionKeepsDifferentDragTargetTileVisible) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(7);
  tile.isDragTarget = true;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)))
      << "Suppressing stale pixels for a display:none selection must not hide the freshly "
         "selected/drag-target layer for a different visible entity.";
}

TEST(RenderPanePresenterTest, CurrentDisplayNoneSelectionSuppressesDragTargetTileFallback) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = entt::null;
  tile.isDragTarget = true;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42),
                                           /*suppressDragTargetTiles=*/true))
      << "While the current selection is display:none, legacy elevated tiles without layer "
         "metadata should be hidden so the old selected shape disappears immediately.";
}

TEST(RenderPanePresenterTest, SuppressionDoesNotHideUnmatchedLayerTiles) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(7);
  tile.isDragTarget = false;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, SuppressionDoesNotHideSegmentTiles) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Segment;
  tile.layerEntity = entt::null;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, MissingTextureIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = 0;
  tile.isDragTarget = false;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, entt::null));
}

TEST(RenderPanePresenterTest, SelectionPrewarmLayerMatchesGroupedActiveDragPreview) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(43);
  tile.isDragTarget = false;

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = static_cast<Entity>(42),
      .extraEntities = {static_cast<Entity>(43)},
      .translation = Vector2d(8.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(8.0, 0.0)),
      .dragGeneration = 5,
  };

  EXPECT_TRUE(TileMatchesActiveDragPreview(tile, activeDrag))
      << "Grouped selection prewarm tiles are valid drag targets as soon as the active drag "
         "starts, "
         "even though the worker did not mark them as drag targets during the idle prewarm.";
}

TEST(RenderPanePresenterTest, PresentedTileQuadIntersectingPaneIsVisible) {
  PresentedTileQuad quad;
  quad.topLeft = Vector2d(10.0, 10.0);
  quad.topRight = Vector2d(30.0, 10.0);
  quad.bottomRight = Vector2d(30.0, 30.0);
  quad.bottomLeft = Vector2d(10.0, 30.0);

  EXPECT_TRUE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/20.0, /*height=*/20.0)));
}

TEST(RenderPanePresenterTest, PresentedTileQuadOutsidePaneIsCulled) {
  PresentedTileQuad quad;
  quad.topLeft = Vector2d(30.0, 10.0);
  quad.topRight = Vector2d(40.0, 10.0);
  quad.bottomRight = Vector2d(40.0, 20.0);
  quad.bottomLeft = Vector2d(30.0, 20.0);

  EXPECT_FALSE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/20.0, /*height=*/20.0)));
}

TEST(RenderPanePresenterTest, PresentedTileQuadTouchingPaneEdgeIsCulled) {
  PresentedTileQuad quad;
  quad.topLeft = Vector2d(20.0, 5.0);
  quad.topRight = Vector2d(30.0, 5.0);
  quad.bottomRight = Vector2d(30.0, 15.0);
  quad.bottomLeft = Vector2d(20.0, 15.0);

  EXPECT_FALSE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/20.0, /*height=*/20.0)));
}

TEST(RenderPanePresenterTest, PresentedImageClipRectIntersectsPaneWithArtboard) {
  const std::optional<Box2d> clip = PresentedImageClipRect(
      Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/100.0, /*height=*/80.0),
      Box2d::FromXYWH(/*x=*/20.0, /*y=*/10.0, /*width=*/120.0, /*height=*/40.0));

  ASSERT_TRUE(clip.has_value());
  EXPECT_EQ(*clip, Box2d::FromXYWH(/*x=*/20.0, /*y=*/10.0, /*width=*/80.0, /*height=*/40.0));
}

TEST(RenderPanePresenterTest, PresentedImageClipRectRejectsDisjointArtboard) {
  EXPECT_FALSE(PresentedImageClipRect(
                   Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/100.0, /*height=*/80.0),
                   Box2d::FromXYWH(/*x=*/100.0, /*y=*/10.0, /*width=*/40.0, /*height=*/40.0))
                   .has_value());
}

// --- Selection-overlay "flare" regression (QA: sharp-cusp shapes draw the ---
// --- selection outline with spikes shooting off the outline). ---

ViewportState IdentityViewport(double zoom = 1.0) {
  ViewportState viewport;
  viewport.zoom = zoom;
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 1000.0, 1000.0);
  return viewport;
}

// A sharp cusp (two segments meeting at an acute angle) must be flagged as a
// bevel break: an unbroken ImGui miter run there grows a spike far longer than
// the miter limit allows. This is the core mechanism behind the flare.
TEST(RenderPanePresenterTest, OverlayMiterBreakFlagsSharpCusp) {
  // A ~160° turn at (10, 0): the outgoing segment heads back at 160° from the
  // incoming one. This is the regime where ImGui's miter offset is largest
  // (its IM_FIXNORMAL2F-capped |dm| ~= 1/cos(phi/2) climbs past the limit).
  const std::vector<Vector2d> cusp = {{0.0, 0.0}, {10.0, 0.0}, {0.6, 3.4}};

  const double unbroken = WorstMiterOffsetHalfWidths(cusp, 0, cusp.size() - 1);
  EXPECT_GT(unbroken, kOverlayStrokeMiterLimit)
      << "sharp cusp should overshoot the miter limit when stroked unbroken";

  const std::vector<std::size_t> breaks = OverlayMiterBreakIndices(cusp, /*closed=*/false);
  EXPECT_EQ(breaks, (std::vector<std::size_t>{1u}))
      << "the cusp vertex must be a bevel break point";
}

// A smooth, gently-curving polyline must NOT be broken: breaking every joint
// would needlessly butt-cap a clean curve.
TEST(RenderPanePresenterTest, OverlayMiterBreakLeavesSmoothCurveIntact) {
  std::vector<Vector2d> smooth;
  for (int i = 0; i <= 16; ++i) {
    const double t = static_cast<double>(i) / 16.0;
    smooth.emplace_back(t * 100.0, std::sin(t * 3.14159) * 20.0);
  }
  EXPECT_TRUE(OverlayMiterBreakIndices(smooth, /*closed=*/false).empty())
      << "a smooth curve should produce no bevel breaks";
}

// After the fix, no run emitted for any selection-overlay subpath has an
// interior miter joint that overshoots the miter limit. This is the red->green
// gate: on the broken code (single mitered run per subpath) the unbroken
// worst-offset exceeds the limit; after the fix the per-run worst-offset is
// bounded.
TEST(RenderPanePresenterTest, OverlayStrokeRunsBoundMiterAtSharpCusps) {
  // A closed "dart" with two ~160° inward cusps at (10, 0) and (10, 6.8) —
  // the regime where ImGui's miter offset climbs past the limit. Exercises the
  // closed-polyline wrap-around run splitting.
  Path spike = PathBuilder()
                   .moveTo({0.0, 0.0})
                   .lineTo({10.0, 0.0})
                   .lineTo({0.6, 3.4})
                   .lineTo({10.0, 6.8})
                   .lineTo({0.0, 6.8})
                   .closePath()
                   .build();

  const ViewportState viewport = IdentityViewport();
  const std::vector<OverlayStrokePolyline> polylines =
      OverlayScreenPolylinesForPath(viewport, spike);
  ASSERT_FALSE(polylines.empty());

  bool sawOvershootUnbroken = false;
  for (const OverlayStrokePolyline& polyline : polylines) {
    const double unbroken =
        WorstMiterOffsetHalfWidths(polyline.points, 0, polyline.points.size() - 1);
    if (unbroken > kOverlayStrokeMiterLimit) {
      sawOvershootUnbroken = true;
    }
    // The fix's run splitting must bound every emitted run's worst miter.
    EXPECT_LE(WorstMiterOffsetAcrossRuns(polyline), kOverlayStrokeMiterLimit + 1e-6)
        << "a run still overshoots the miter limit -> flare not fixed";
  }
  EXPECT_TRUE(sawOvershootUnbroken)
      << "test geometry must actually trigger an over-limit miter when unbroken";
}

// End-to-end on the real splash content the QA reported: selecting
// #Mid_yellow_lightning must not leave any overlay stroke run with a runaway
// miter. Skips gracefully if the data file is unavailable.
TEST(RenderPanePresenterTest, SplashLightningOverlayHasNoMiterFlare) {
  std::ifstream stream("donner_splash_v0_8_editable.svg");
  if (!stream.is_open()) {
    GTEST_SKIP() << "donner_splash_v0_8_editable.svg not found in runfiles";
  }
  std::ostringstream buf;
  buf << stream.rdbuf();
  const std::string source = buf.str();
  ASSERT_FALSE(source.empty());

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(source));
  app.document().document().setCanvasSize(900, 600);

  auto group = app.document().document().querySelector("#Mid_yellow_lightning");
  ASSERT_TRUE(group.has_value());
  app.setSelection(*group);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d());
  ASSERT_FALSE(snapshot.paths.empty());

  // Render the overlay at a representative zoom (3x) where the flattened
  // polyline is fine and cusps are sharpest.
  const ViewportState viewport = IdentityViewport(/*zoom=*/3.0);
  bool sawOvershootUnbroken = false;
  for (const SelectionChromeSnapshot::PathItem& item : snapshot.paths) {
    for (const OverlayStrokePolyline& polyline :
         OverlayScreenPolylinesForPath(viewport, item.pathDoc)) {
      if (polyline.points.size() < 3) {
        continue;
      }
      if (WorstMiterOffsetHalfWidths(polyline.points, 0, polyline.points.size() - 1) >
          kOverlayStrokeMiterLimit) {
        sawOvershootUnbroken = true;
      }
      EXPECT_LE(WorstMiterOffsetAcrossRuns(polyline), kOverlayStrokeMiterLimit + 1e-6)
          << "splash lightning overlay still has a miter flare";
    }
  }
  EXPECT_TRUE(sawOvershootUnbroken)
      << "the real lightning path must exercise the flare (else the test proves nothing)";
}

}  // namespace
}  // namespace donner::editor
