/// @file TinySkiaRenderPerfBench.cpp
/// @brief tiny-skia-cpp rendering benchmarks (C++ only, no Rust FFI).
///
/// Uses identical scene geometry and paint parameters as RenderPerfBench.cpp.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <optional>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Point.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/shaders/Gradient.h"
#include "tiny_skia/shaders/LinearGradient.h"
#include "tiny_skia/shaders/Pattern.h"
#include "tiny_skia/shaders/RadialGradient.h"

namespace {

using tiny_skia::BlendMode;
using tiny_skia::Color;
using tiny_skia::FillRule;
using tiny_skia::FilterQuality;
using tiny_skia::GradientStop;
using tiny_skia::LinearGradient;
using tiny_skia::Paint;
using tiny_skia::Path;
using tiny_skia::PathBuilder;
using tiny_skia::Pattern;
using tiny_skia::Pixmap;
using tiny_skia::Point;
using tiny_skia::RadialGradient;
using tiny_skia::Rect;
using tiny_skia::SpreadMode;
using tiny_skia::Stroke;
using tiny_skia::StrokeDash;
using tiny_skia::Transform;

constexpr std::int64_t kSceneSize = 512;

std::optional<Path> createScenePath(float d) {
  PathBuilder pb;
  pb.moveTo(0.10f * d, 0.14f * d);
  pb.cubicTo(0.30f * d, 0.02f * d, 0.70f * d, 0.02f * d, 0.90f * d, 0.18f * d);
  pb.lineTo(0.78f * d, 0.48f * d);
  pb.quadTo(0.64f * d, 0.90f * d, 0.36f * d, 0.82f * d);
  pb.lineTo(0.18f * d, 0.56f * d);
  pb.cubicTo(0.06f * d, 0.44f * d, 0.05f * d, 0.26f * d, 0.10f * d, 0.14f * d);
  pb.close();
  return pb.finish();
}

Paint createPaint() {
  Paint paint;
  paint.setColorRgba8(22, 158, 255, 200);
  paint.antiAlias = true;
  return paint;
}

void recordThroughput(benchmark::State& state, std::int64_t dim) {
  const auto pixelsPerIteration = static_cast<double>(dim * dim);
  state.SetItemsProcessed(state.iterations() * dim * dim);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      pixelsPerIteration, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_FillPath_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const Paint paint = createPaint();
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillRect_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto rect = Rect::fromLTRB(0.12f * static_cast<float>(dim),
                             0.18f * static_cast<float>(dim),
                             0.88f * static_cast<float>(dim),
                             0.84f * static_cast<float>(dim));
  if (!rect.has_value()) {
    state.SkipWithError("Failed to create rect");
    return;
  }

  const Paint paint = createPaint();
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_StrokePath_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const Paint paint = createPaint();
  Stroke stroke;
  stroke.width = 3.0f;
  stroke.lineCap = tiny_skia::LineCap::Round;
  stroke.lineJoin = tiny_skia::LineJoin::Round;
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::strokePath(mut, *path, paint, stroke, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillPath_LinearGradient_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const auto d = static_cast<float>(dim);
  auto gradient = LinearGradient::create(
      Point::fromXY(0.1f * d, 0.1f * d), Point::fromXY(0.9f * d, 0.9f * d),
      {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
       GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
      SpreadMode::Pad, Transform::identity());
  if (!gradient.has_value()) {
    state.SkipWithError("Failed to create linear gradient");
    return;
  }

  Paint paint;
  paint.antiAlias = true;
  paint.shader = std::get<LinearGradient>(std::move(*gradient));
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillPath_Opaque_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  Paint paint;
  paint.setColorRgba8(22, 158, 255, 255);
  paint.antiAlias = true;
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillPath_RadialGradient_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const auto d = static_cast<float>(dim);
  auto gradient = RadialGradient::create(
      Point::fromXY(0.5f * d, 0.5f * d), 0.0f, Point::fromXY(0.5f * d, 0.5f * d), 0.45f * d,
      {GradientStop::create(0.0f, Color::fromRgba8(255, 100, 50, 200)),
       GradientStop::create(1.0f, Color::fromRgba8(50, 100, 255, 180))},
      SpreadMode::Pad, Transform::identity());
  if (!gradient.has_value()) {
    state.SkipWithError("Failed to create radial gradient");
    return;
  }

  Paint paint;
  paint.antiAlias = true;
  paint.shader = std::get<RadialGradient>(std::move(*gradient));
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_StrokePath_Dashed_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const Paint paint = createPaint();
  Stroke stroke;
  stroke.width = 3.0f;
  stroke.lineCap = tiny_skia::LineCap::Butt;
  stroke.lineJoin = tiny_skia::LineJoin::Miter;
  stroke.dash = StrokeDash::create({10.0f, 5.0f}, 0.0f);
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::strokePath(mut, *path, paint, stroke, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_StrokePath_Thick_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const Paint paint = createPaint();
  Stroke stroke;
  stroke.width = 10.0f;
  stroke.lineCap = tiny_skia::LineCap::Round;
  stroke.lineJoin = tiny_skia::LineJoin::Round;
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::strokePath(mut, *path, paint, stroke, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillPath_Transformed_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const Paint paint = createPaint();
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  // 30-degree rotation around center.
  constexpr float kCos30 = 0.866025f;
  constexpr float kSin30 = 0.5f;
  const float cx = static_cast<float>(dim) * 0.5f;
  const float cy = cx;
  const Transform xform = Transform::fromRow(kCos30, kSin30, -kSin30, kCos30,
                                             cx * (1.0f - kCos30) + cy * kSin30,
                                             cy * (1.0f - kCos30) - cx * kSin30);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, xform);
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillPath_EvenOdd_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  const Paint paint = createPaint();
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::EvenOdd, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillPath_Pattern_TinySkia(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate pixmap");
    return;
  }

  auto path = createScenePath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create path");
    return;
  }

  // Create a 64x64 checkerboard pattern tile.
  constexpr std::uint32_t kTileSize = 64;
  auto tilePm = Pixmap::fromSize(kTileSize, kTileSize);
  if (!tilePm.has_value()) {
    state.SkipWithError("Failed to allocate tile pixmap");
    return;
  }
  {
    auto tileData = tilePm->mutableView().data();
    for (std::uint32_t y = 0; y < kTileSize; ++y) {
      for (std::uint32_t x = 0; x < kTileSize; ++x) {
        const std::size_t off = (y * kTileSize + x) * 4;
        const bool light = ((x / 8) + (y / 8)) % 2 == 0;
        tileData[off + 0] = light ? 200 : 50;
        tileData[off + 1] = light ? 180 : 80;
        tileData[off + 2] = light ? 160 : 120;
        tileData[off + 3] = 220;
      }
    }
  }

  Paint paint;
  paint.antiAlias = true;
  paint.shader = Pattern(tilePm->view(), SpreadMode::Repeat, FilterQuality::Bilinear, 1.0f,
                         Transform::identity());
  const Color clearColor = Color::fromRgba8(0, 0, 0, 0);

  for (auto _ : state) {
    pixmap->fill(clearColor);
    auto mut = pixmap->mutableView();
    tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
    benchmark::DoNotOptimize(pixmap->data().data());
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

BENCHMARK(BM_FillPath_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_FillRect_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_StrokePath_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_LinearGradient_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Opaque_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_RadialGradient_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_StrokePath_Dashed_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_StrokePath_Thick_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Transformed_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_EvenOdd_TinySkia)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Pattern_TinySkia)->Arg(kSceneSize);

}  // namespace
