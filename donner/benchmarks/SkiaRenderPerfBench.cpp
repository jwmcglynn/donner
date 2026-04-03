/// @file SkiaRenderPerfBench.cpp
/// @brief Skia rendering benchmarks for performance comparison against tiny-skia-cpp.
///
/// Uses identical scene geometry and paint parameters as RenderPerfBench.cpp
/// to enable fair apples-to-apples comparison.

#include <benchmark/benchmark.h>

#include <cstdint>

// Skia
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"

namespace {

constexpr std::int64_t kSceneSize = 512;

/// Build the same scene path as RenderPerfBench.cpp's appendSceneToCppBuilder.
SkPath createScenePath(float d) {
  SkPathBuilder pb;
  pb.moveTo(0.10f * d, 0.14f * d);
  pb.cubicTo(0.30f * d, 0.02f * d, 0.70f * d, 0.02f * d, 0.90f * d, 0.18f * d);
  pb.lineTo(0.78f * d, 0.48f * d);
  pb.quadTo(0.64f * d, 0.90f * d, 0.36f * d, 0.82f * d);
  pb.lineTo(0.18f * d, 0.56f * d);
  pb.cubicTo(0.06f * d, 0.44f * d, 0.05f * d, 0.26f * d, 0.10f * d, 0.14f * d);
  pb.close();
  return pb.detach();
}

void recordThroughput(benchmark::State& state, std::int64_t dim) {
  const auto pixelsPerIteration = static_cast<double>(dim * dim);
  state.SetItemsProcessed(state.iterations() * dim * dim);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      pixelsPerIteration, benchmark::Counter::kIsIterationInvariantRate);
}

// ---------------------------------------------------------------------------
// FillPath — Skia (semi-transparent, SourceOver, anti-aliased)
// ---------------------------------------------------------------------------

void BM_FillPath_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPath path = createScenePath(static_cast<float>(dim));

  SkPaint paint;
  paint.setColor(SkColorSetARGB(200, 22, 158, 255));
  paint.setAntiAlias(true);

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// FillRect — Skia (semi-transparent, SourceOver, anti-aliased)
// ---------------------------------------------------------------------------

void BM_FillRect_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  const float d = static_cast<float>(dim);
  SkRect rect = SkRect::MakeLTRB(0.12f * d, 0.18f * d, 0.88f * d, 0.84f * d);

  SkPaint paint;
  paint.setColor(SkColorSetARGB(200, 22, 158, 255));
  paint.setAntiAlias(true);

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawRect(rect, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// StrokePath — Skia (3px round cap/join, semi-transparent)
// ---------------------------------------------------------------------------

void BM_StrokePath_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPath path = createScenePath(static_cast<float>(dim));

  SkPaint paint;
  paint.setColor(SkColorSetARGB(200, 22, 158, 255));
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(3.0f);
  paint.setStrokeCap(SkPaint::kRound_Cap);
  paint.setStrokeJoin(SkPaint::kRound_Join);

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// FillPath with LinearGradient — Skia
// ---------------------------------------------------------------------------

void BM_FillPath_LinearGradient_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  const float d = static_cast<float>(dim);
  SkPath path = createScenePath(d);

  SkPoint pts[2] = {{0.1f * d, 0.1f * d}, {0.9f * d, 0.9f * d}};
  SkColor colors[2] = {SkColorSetARGB(200, 50, 127, 150), SkColorSetARGB(180, 220, 140, 75)};
  sk_sp<SkShader> shader =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setShader(std::move(shader));

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// FillPath Opaque — Skia (alpha=255, enables Source blend fast path)
// ---------------------------------------------------------------------------

void BM_FillPath_Opaque_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPath path = createScenePath(static_cast<float>(dim));

  SkPaint paint;
  paint.setColor(SkColorSetARGB(255, 22, 158, 255));
  paint.setAntiAlias(true);

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// FillPath with RadialGradient — Skia
// ---------------------------------------------------------------------------

void BM_FillPath_RadialGradient_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  const float d = static_cast<float>(dim);
  SkPath path = createScenePath(d);

  SkPoint center = {0.5f * d, 0.5f * d};
  SkColor colors[2] = {SkColorSetARGB(200, 255, 100, 50), SkColorSetARGB(180, 50, 100, 255)};
  sk_sp<SkShader> shader =
      SkGradientShader::MakeRadial(center, 0.45f * d, colors, nullptr, 2, SkTileMode::kClamp);

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setShader(std::move(shader));

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// StrokePath Dashed — Skia (3px, 10/5 dash pattern)
// ---------------------------------------------------------------------------

void BM_StrokePath_Dashed_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPath path = createScenePath(static_cast<float>(dim));

  SkPaint paint;
  paint.setColor(SkColorSetARGB(200, 22, 158, 255));
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(3.0f);
  paint.setStrokeCap(SkPaint::kButt_Cap);
  paint.setStrokeJoin(SkPaint::kMiter_Join);
  const SkScalar intervals[] = {10.0f, 5.0f};
  paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, 0.0f));

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// StrokePath Thick — Skia (10px round cap/join)
// ---------------------------------------------------------------------------

void BM_StrokePath_Thick_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPath path = createScenePath(static_cast<float>(dim));

  SkPaint paint;
  paint.setColor(SkColorSetARGB(200, 22, 158, 255));
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(10.0f);
  paint.setStrokeCap(SkPaint::kRound_Cap);
  paint.setStrokeJoin(SkPaint::kRound_Join);

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// FillPath Transformed — Skia (30-degree rotation around center)
// ---------------------------------------------------------------------------

void BM_FillPath_Transformed_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPath path = createScenePath(static_cast<float>(dim));

  SkPaint paint;
  paint.setColor(SkColorSetARGB(200, 22, 158, 255));
  paint.setAntiAlias(true);

  const float cx = static_cast<float>(dim) * 0.5f;

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->save();
    canvas->rotate(30.0f, cx, cx);
    canvas->drawPath(path, paint);
    canvas->restore();
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// FillPath EvenOdd — Skia
// ---------------------------------------------------------------------------

void BM_FillPath_EvenOdd_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPath path = createScenePath(static_cast<float>(dim));
  path.setFillType(SkPathFillType::kEvenOdd);

  SkPaint paint;
  paint.setColor(SkColorSetARGB(200, 22, 158, 255));
  paint.setAntiAlias(true);

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// FillPath Pattern — Skia (64x64 checkerboard tile)
// ---------------------------------------------------------------------------

void BM_FillPath_Pattern_Skia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(dim, dim, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  const float d = static_cast<float>(dim);
  SkPath path = createScenePath(d);

  // Create 64x64 checkerboard tile.
  constexpr int kTileSize = 64;
  const SkImageInfo tileInfo =
      SkImageInfo::Make(kTileSize, kTileSize, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> tileSurface = SkSurfaces::Raster(tileInfo);
  SkCanvas* tileCanvas = tileSurface->getCanvas();
  for (int y = 0; y < kTileSize; y += 8) {
    for (int x = 0; x < kTileSize; x += 8) {
      SkPaint cellPaint;
      const bool light = ((x / 8) + (y / 8)) % 2 == 0;
      cellPaint.setColor(light ? SkColorSetARGB(220, 200, 180, 160)
                               : SkColorSetARGB(220, 50, 80, 120));
      tileCanvas->drawRect(SkRect::MakeXYWH(x, y, 8, 8), cellPaint);
    }
  }
  sk_sp<SkImage> tileImage = tileSurface->makeImageSnapshot();
  sk_sp<SkShader> shader =
      tileImage->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions(SkFilterMode::kLinear));

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setShader(std::move(shader));

  for (auto _ : state) {
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->drawPath(path, paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

// ---------------------------------------------------------------------------
// Register benchmarks — same args as RenderPerfBench.cpp for direct comparison.
// ---------------------------------------------------------------------------

BENCHMARK(BM_FillPath_Skia)->Arg(kSceneSize);
BENCHMARK(BM_FillRect_Skia)->Arg(kSceneSize);
BENCHMARK(BM_StrokePath_Skia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_LinearGradient_Skia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Opaque_Skia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_RadialGradient_Skia)->Arg(kSceneSize);
BENCHMARK(BM_StrokePath_Dashed_Skia)->Arg(kSceneSize);
BENCHMARK(BM_StrokePath_Thick_Skia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Transformed_Skia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_EvenOdd_Skia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Pattern_Skia)->Arg(kSceneSize);

}  // namespace
