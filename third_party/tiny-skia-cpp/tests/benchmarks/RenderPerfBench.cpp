#include <benchmark/benchmark.h>

#include <array>
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
#include "tiny_skia/wide/backend/BackendConfig.h"
#include "tiny_skia_ffi.h"

namespace {

using tiny_skia::BlendMode;
using tiny_skia::Color;
using tiny_skia::FillRule;
using tiny_skia::GradientStop;
using tiny_skia::LinearGradient;
using tiny_skia::Paint;
using tiny_skia::Path;
using tiny_skia::PathBuilder;
using tiny_skia::Pixmap;
using tiny_skia::Point;
using tiny_skia::Rect;
using tiny_skia::SpreadMode;
using tiny_skia::Stroke;
using tiny_skia::Transform;
using tiny_skia::wide::backend::backendName;
using tiny_skia::wide::backend::selectedBackend;

constexpr std::int64_t kSceneSize = 512;
constexpr std::array<float, 6> kIdentityTransform = {1.0f, 0.0f, 0.0f,
                                                      1.0f, 0.0f, 0.0f};

class RustPixmap {
 public:
  RustPixmap(std::uint32_t width, std::uint32_t height)
      : handle_(ts_ffi_pixmap_new(width, height)) {}

  ~RustPixmap() {
    ts_ffi_pixmap_free(handle_);
  }

  RustPixmap(const RustPixmap&) = delete;
  RustPixmap& operator=(const RustPixmap&) = delete;

  [[nodiscard]] bool valid() const {
    return handle_ != nullptr;
  }

  void clear() {
    ts_ffi_pixmap_fill_color(handle_, 0, 0, 0, 0);
  }

  [[nodiscard]] TsFfiPixmap* raw() {
    return handle_;
  }

 private:
  TsFfiPixmap* handle_ = nullptr;
};

class RustPath {
 public:
  explicit RustPath(TsFfiPath* handle) : handle_(handle) {}

  ~RustPath() {
    ts_ffi_path_free(handle_);
  }

  RustPath(const RustPath&) = delete;
  RustPath& operator=(const RustPath&) = delete;

  [[nodiscard]] bool valid() const {
    return handle_ != nullptr;
  }

  [[nodiscard]] const TsFfiPath* raw() const {
    return handle_;
  }

 private:
  TsFfiPath* handle_ = nullptr;
};

class RustPaint {
 public:
  RustPaint(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a,
            bool antiAlias, std::uint8_t blendMode)
      : handle_(ts_ffi_paint_new_solid_rgba8(r, g, b, a, antiAlias, blendMode)) {}

  ~RustPaint() {
    ts_ffi_paint_free(handle_);
  }

  RustPaint(const RustPaint&) = delete;
  RustPaint& operator=(const RustPaint&) = delete;

  [[nodiscard]] bool valid() const {
    return handle_ != nullptr;
  }

  [[nodiscard]] const TsFfiPaint* raw() const {
    return handle_;
  }

 private:
  TsFfiPaint* handle_ = nullptr;
};

class RustRect {
 public:
  RustRect(float left, float top, float right, float bottom)
      : handle_(ts_ffi_rect_from_ltrb(left, top, right, bottom)) {}

  ~RustRect() {
    ts_ffi_rect_free(handle_);
  }

  RustRect(const RustRect&) = delete;
  RustRect& operator=(const RustRect&) = delete;

  [[nodiscard]] bool valid() const {
    return handle_ != nullptr;
  }

  [[nodiscard]] const TsFfiRect* raw() const {
    return handle_;
  }

 private:
  TsFfiRect* handle_ = nullptr;
};

class RustTransform {
 public:
  explicit RustTransform(const std::array<float, 6>& matrix)
      : handle_(ts_ffi_transform_from_row(matrix.data())) {}

  ~RustTransform() {
    ts_ffi_transform_free(handle_);
  }

  RustTransform(const RustTransform&) = delete;
  RustTransform& operator=(const RustTransform&) = delete;

  [[nodiscard]] bool valid() const {
    return handle_ != nullptr;
  }

  [[nodiscard]] const TsFfiTransform* raw() const {
    return handle_;
  }

 private:
  TsFfiTransform* handle_ = nullptr;
};

void appendSceneToCppBuilder(PathBuilder& pb, float d) {
  pb.moveTo(0.10f * d, 0.14f * d);
  pb.cubicTo(0.30f * d, 0.02f * d, 0.70f * d, 0.02f * d, 0.90f * d, 0.18f * d);
  pb.lineTo(0.78f * d, 0.48f * d);
  pb.quadTo(0.64f * d, 0.90f * d, 0.36f * d, 0.82f * d);
  pb.lineTo(0.18f * d, 0.56f * d);
  pb.cubicTo(0.06f * d, 0.44f * d, 0.05f * d, 0.26f * d, 0.10f * d, 0.14f * d);
  pb.close();
}

std::optional<Path> createCppPath(float d) {
  PathBuilder pb;
  appendSceneToCppBuilder(pb, d);
  return pb.finish();
}

TsFfiPath* createRustPath(float d) {
  TsFfiPathBuilder* pb = ts_ffi_path_builder_new();
  if (pb == nullptr) {
    return nullptr;
  }

  ts_ffi_path_builder_move_to(pb, 0.10f * d, 0.14f * d);
  ts_ffi_path_builder_cubic_to(pb, 0.30f * d, 0.02f * d, 0.70f * d, 0.02f * d,
                               0.90f * d, 0.18f * d);
  ts_ffi_path_builder_line_to(pb, 0.78f * d, 0.48f * d);
  ts_ffi_path_builder_quad_to(pb, 0.64f * d, 0.90f * d, 0.36f * d, 0.82f * d);
  ts_ffi_path_builder_line_to(pb, 0.18f * d, 0.56f * d);
  ts_ffi_path_builder_cubic_to(pb, 0.06f * d, 0.44f * d, 0.05f * d, 0.26f * d,
                               0.10f * d, 0.14f * d);
  ts_ffi_path_builder_close(pb);
  return ts_ffi_path_builder_finish(pb);
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

void BM_FillPath_Cpp(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate C++ pixmap");
    return;
  }

  auto path = createCppPath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create C++ path");
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

void BM_FillPath_Rust(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  RustPixmap pixmap(dim, dim);
  if (!pixmap.valid()) {
    state.SkipWithError("Failed to allocate Rust pixmap");
    return;
  }

  RustPath path(createRustPath(static_cast<float>(dim)));
  if (!path.valid()) {
    state.SkipWithError("Failed to create Rust path");
    return;
  }

  RustPaint paint(22, 158, 255, 200, true,
                  static_cast<std::uint8_t>(BlendMode::SourceOver));
  if (!paint.valid()) {
    state.SkipWithError("Failed to create Rust paint");
    return;
  }

  RustTransform transform(kIdentityTransform);
  if (!transform.valid()) {
    state.SkipWithError("Failed to create Rust transform");
    return;
  }

  for (auto _ : state) {
    pixmap.clear();
    const bool ok = ts_ffi_fill_path_prepared(
        pixmap.raw(), path.raw(), paint.raw(),
        static_cast<std::uint8_t>(FillRule::Winding), transform.raw());
    if (!ok) {
      state.SkipWithError("Rust fillPath failed");
      return;
    }

    benchmark::DoNotOptimize(ts_ffi_pixmap_data(pixmap.raw()));
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_FillRect_Cpp(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate C++ pixmap");
    return;
  }

  auto rect = Rect::fromLTRB(0.12f * static_cast<float>(dim),
                             0.18f * static_cast<float>(dim),
                             0.88f * static_cast<float>(dim),
                             0.84f * static_cast<float>(dim));
  if (!rect.has_value()) {
    state.SkipWithError("Failed to create C++ rect");
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

void BM_FillRect_Rust(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  RustPixmap pixmap(dim, dim);
  if (!pixmap.valid()) {
    state.SkipWithError("Failed to allocate Rust pixmap");
    return;
  }

  const float d = static_cast<float>(dim);
  RustRect rect(0.12f * d, 0.18f * d, 0.88f * d, 0.84f * d);
  if (!rect.valid()) {
    state.SkipWithError("Failed to create Rust rect");
    return;
  }

  RustPaint paint(22, 158, 255, 200, true,
                  static_cast<std::uint8_t>(BlendMode::SourceOver));
  if (!paint.valid()) {
    state.SkipWithError("Failed to create Rust paint");
    return;
  }

  RustTransform transform(kIdentityTransform);
  if (!transform.valid()) {
    state.SkipWithError("Failed to create Rust transform");
    return;
  }

  for (auto _ : state) {
    pixmap.clear();
    const bool ok = ts_ffi_fill_rect_prepared(
        pixmap.raw(), rect.raw(), paint.raw(), transform.raw());
    if (!ok) {
      state.SkipWithError("Rust fillRect failed");
      return;
    }

    benchmark::DoNotOptimize(ts_ffi_pixmap_data(pixmap.raw()));
    benchmark::ClobberMemory();
  }

  recordThroughput(state, state.range(0));
}

void BM_StrokePath_Cpp(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate C++ pixmap");
    return;
  }

  auto path = createCppPath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create C++ path");
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

void BM_FillPath_LinearGradient_Cpp(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate C++ pixmap");
    return;
  }

  auto path = createCppPath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create C++ path");
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

void BM_FillPath_Opaque_Cpp(benchmark::State& state) {
  const auto dim = static_cast<std::uint32_t>(state.range(0));
  auto pixmap = Pixmap::fromSize(dim, dim);
  if (!pixmap.has_value()) {
    state.SkipWithError("Failed to allocate C++ pixmap");
    return;
  }

  auto path = createCppPath(static_cast<float>(dim));
  if (!path.has_value()) {
    state.SkipWithError("Failed to create C++ path");
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

[[maybe_unused]] const bool kBenchmarkContextInitialized = []() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE)
  benchmark::AddCustomContext("simdMode", "native");
#else
  benchmark::AddCustomContext("simdMode", "scalar");
#endif
  benchmark::AddCustomContext("cppBackend", backendName(selectedBackend()));
  benchmark::AddCustomContext("comparisonMode", "engineCore");
  benchmark::AddCustomContext("rustFfiMode", "preparedState");
  return true;
}();

BENCHMARK(BM_FillPath_Cpp)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Rust)->Arg(kSceneSize);
BENCHMARK(BM_FillRect_Cpp)->Arg(kSceneSize);
BENCHMARK(BM_FillRect_Rust)->Arg(kSceneSize);
BENCHMARK(BM_StrokePath_Cpp)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_LinearGradient_Cpp)->Arg(kSceneSize);
BENCHMARK(BM_FillPath_Opaque_Cpp)->Arg(kSceneSize);

}  // namespace
