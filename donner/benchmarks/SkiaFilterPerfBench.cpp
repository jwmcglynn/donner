/// @file SkiaFilterPerfBench.cpp
/// @brief Skia filter benchmarks for performance comparison against tiny-skia-cpp.
///
/// Uses identical input data (same RNG seed, same premultiply logic) as FilterPerfBench.cpp
/// to enable fair apples-to-apples comparison.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

// Skia
#include "include/core/SkCanvas.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkPoint3.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkPerlinNoiseShader.h"

namespace {

/// Create premultiplied RGBA pixel data identical to FilterPerfBench.cpp's makeRandomPixmap.
std::vector<std::uint8_t> makeRandomPremulPixels(std::uint32_t width, std::uint32_t height,
                                                  unsigned int seed = 42) {
  const std::size_t count = static_cast<std::size_t>(width) * height * 4;
  std::vector<std::uint8_t> data(count);
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& v : data) {
    v = static_cast<std::uint8_t>(dist(rng));
  }
  for (std::size_t i = 0; i + 3 < count; i += 4) {
    data[i + 3] = std::max({data[i], data[i + 1], data[i + 2], data[i + 3]});
  }
  return data;
}

sk_sp<SkImage> makeSkImage(const std::vector<std::uint8_t>& pixels, std::uint32_t width,
                           std::uint32_t height) {
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  const SkPixmap pixmap(info, pixels.data(), static_cast<std::size_t>(width) * 4);
  return SkImages::RasterFromPixmapCopy(pixmap);
}

/// Apply an SkImageFilter and force pixel materialization.
void runFilterBenchmark(benchmark::State& state, sk_sp<SkImage> srcImage,
                        sk_sp<SkImageFilter> filter, std::uint32_t size) {
  const SkImageInfo info =
      SkImageInfo::Make(size, size, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  SkPaint paint;
  paint.setImageFilter(std::move(filter));

  for (auto _ : state) {
    // Purge Skia's image filter cache to prevent cached results from skewing the benchmark.
    // Without this, only the first iteration computes the filter — subsequent iterations
    // return the cached result (~60μs blit), making Skia appear 30-100x faster than it is.
    SkGraphics::PurgeResourceCache();
    canvas->drawImage(srcImage, 0, 0, SkSamplingOptions(), &paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

// ---------------------------------------------------------------------------
// Gaussian Blur — Skia
// ---------------------------------------------------------------------------

void BM_GaussianBlur_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const auto sigma = static_cast<float>(state.range(1));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);
  sk_sp<SkImageFilter> filter =
      SkImageFilters::Blur(sigma, sigma, SkTileMode::kDecal, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Morphology — Skia
// ---------------------------------------------------------------------------

void BM_Morphology_Dilate_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const auto radius = static_cast<float>(state.range(1));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);
  sk_sp<SkImageFilter> filter = SkImageFilters::Dilate(radius, radius, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

void BM_Morphology_Erode_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const auto radius = static_cast<float>(state.range(1));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);
  sk_sp<SkImageFilter> filter = SkImageFilters::Erode(radius, radius, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Blend — Skia
// ---------------------------------------------------------------------------

void BM_Blend_Multiply_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels1 = makeRandomPremulPixels(size, size, 42);
  auto pixels2 = makeRandomPremulPixels(size, size, 123);
  sk_sp<SkImage> bg = makeSkImage(pixels1, size, size);
  sk_sp<SkImage> fg = makeSkImage(pixels2, size, size);

  sk_sp<SkImageFilter> bgFilter = SkImageFilters::Image(bg, SkFilterMode::kNearest);
  sk_sp<SkImageFilter> filter =
      SkImageFilters::Blend(SkBlendMode::kMultiply, std::move(bgFilter), nullptr);
  runFilterBenchmark(state, std::move(fg), std::move(filter), size);
}

void BM_Blend_Screen_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels1 = makeRandomPremulPixels(size, size, 42);
  auto pixels2 = makeRandomPremulPixels(size, size, 123);
  sk_sp<SkImage> bg = makeSkImage(pixels1, size, size);
  sk_sp<SkImage> fg = makeSkImage(pixels2, size, size);

  sk_sp<SkImageFilter> bgFilter = SkImageFilters::Image(bg, SkFilterMode::kNearest);
  sk_sp<SkImageFilter> filter =
      SkImageFilters::Blend(SkBlendMode::kScreen, std::move(bgFilter), nullptr);
  runFilterBenchmark(state, std::move(fg), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Composite — Skia
// ---------------------------------------------------------------------------

void BM_Composite_Over_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels1 = makeRandomPremulPixels(size, size, 42);
  auto pixels2 = makeRandomPremulPixels(size, size, 123);
  sk_sp<SkImage> src1 = makeSkImage(pixels1, size, size);
  sk_sp<SkImage> src2 = makeSkImage(pixels2, size, size);

  sk_sp<SkImageFilter> bg = SkImageFilters::Image(src2, SkFilterMode::kNearest);
  sk_sp<SkImageFilter> filter =
      SkImageFilters::Blend(SkBlendMode::kSrcOver, std::move(bg), nullptr);
  runFilterBenchmark(state, std::move(src1), std::move(filter), size);
}

void BM_Composite_Arithmetic_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels1 = makeRandomPremulPixels(size, size, 42);
  auto pixels2 = makeRandomPremulPixels(size, size, 123);
  sk_sp<SkImage> src1 = makeSkImage(pixels1, size, size);
  sk_sp<SkImage> src2 = makeSkImage(pixels2, size, size);

  sk_sp<SkImageFilter> bg = SkImageFilters::Image(src2, SkFilterMode::kNearest);
  sk_sp<SkImageFilter> filter =
      SkImageFilters::Arithmetic(0.5f, 0.3f, 0.2f, 0.0f, true, std::move(bg), nullptr);
  runFilterBenchmark(state, std::move(src1), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// ColorMatrix — Skia
// ---------------------------------------------------------------------------

void BM_ColorMatrix_Saturate_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  // Saturate matrix with s=0.5 matching tiny-skia's saturateMatrix(0.5).
  const float s = 0.5f;
  // clang-format off
  float matrix[20] = {
    0.2126f + 0.7874f * s, 0.7152f - 0.7152f * s, 0.0722f - 0.0722f * s, 0, 0,
    0.2126f - 0.2126f * s, 0.7152f + 0.2848f * s, 0.0722f - 0.0722f * s, 0, 0,
    0.2126f - 0.2126f * s, 0.7152f - 0.7152f * s, 0.0722f + 0.9278f * s, 0, 0,
    0,                     0,                      0,                      1, 0,
  };
  // clang-format on

  sk_sp<SkColorFilter> cf = SkColorFilters::Matrix(matrix);
  sk_sp<SkImageFilter> filter = SkImageFilters::ColorFilter(std::move(cf), nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// ConvolveMatrix — Skia
// ---------------------------------------------------------------------------

void BM_ConvolveMatrix_3x3_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  // Same edge-detect kernel as tiny-skia benchmark.
  SkScalar kernel[] = {-1, -1, -1, -1, 8, -1, -1, -1, -1};
  SkISize kernelSize = SkISize::Make(3, 3);
  SkIPoint kernelOffset = SkIPoint::Make(1, 1);

  sk_sp<SkImageFilter> filter = SkImageFilters::MatrixConvolution(
      kernelSize, kernel, 1.0f, 0.0f, kernelOffset, SkTileMode::kDecal, false, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

void BM_ConvolveMatrix_5x5_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  SkScalar kernel[] = {1, 4,  7,  4,  1, 4,  16, 26, 16, 4, 7, 26, 41,
                       26, 7, 4,  16, 26, 16, 4,  1,  4,  7, 4, 1};
  SkISize kernelSize = SkISize::Make(5, 5);
  SkIPoint kernelOffset = SkIPoint::Make(2, 2);

  sk_sp<SkImageFilter> filter = SkImageFilters::MatrixConvolution(
      kernelSize, kernel, 273.0f, 0.0f, kernelOffset, SkTileMode::kDecal, false, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Turbulence — Skia
// ---------------------------------------------------------------------------

void BM_Turbulence_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(size, size, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  sk_sp<SkShader> shader =
      SkShaders::MakeTurbulence(0.05f, 0.05f, 4, 42.0f);
  SkPaint paint;
  paint.setShader(std::move(shader));

  for (auto _ : state) {
    canvas->drawPaint(paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_FractalNoise_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const SkImageInfo info =
      SkImageInfo::Make(size, size, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  SkCanvas* canvas = surface->getCanvas();

  sk_sp<SkShader> shader =
      SkShaders::MakeFractalNoise(0.05f, 0.05f, 4, 42.0f);
  SkPaint paint;
  paint.setShader(std::move(shader));

  for (auto _ : state) {
    canvas->drawPaint(paint);
    SkPixmap readback;
    surface->peekPixels(&readback);
    benchmark::DoNotOptimize(readback.addr());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

// ---------------------------------------------------------------------------
// Lighting — Skia
// ---------------------------------------------------------------------------

void BM_DiffuseLighting_Point_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  SkPoint3 location = SkPoint3::Make(size / 2.0f, size / 2.0f, 200.0f);
  sk_sp<SkImageFilter> filter =
      SkImageFilters::PointLitDiffuse(location, SK_ColorWHITE, 5.0f, 1.0f, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

void BM_SpecularLighting_Point_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  SkPoint3 location = SkPoint3::Make(size / 2.0f, size / 2.0f, 200.0f);
  sk_sp<SkImageFilter> filter =
      SkImageFilters::PointLitSpecular(location, SK_ColorWHITE, 5.0f, 1.0f, 20.0f, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// DisplacementMap — Skia
// ---------------------------------------------------------------------------

void BM_DisplacementMap_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels1 = makeRandomPremulPixels(size, size, 42);
  auto pixels2 = makeRandomPremulPixels(size, size, 123);
  sk_sp<SkImage> colorImage = makeSkImage(pixels1, size, size);
  sk_sp<SkImage> displacementImage = makeSkImage(pixels2, size, size);

  // Skia takes (displacement, color) — displacement is the background input.
  sk_sp<SkImageFilter> displacementInput =
      SkImageFilters::Image(displacementImage, SkFilterMode::kNearest);
  sk_sp<SkImageFilter> filter = SkImageFilters::DisplacementMap(
      SkColorChannel::kR, SkColorChannel::kG, 20.0f, std::move(displacementInput), nullptr);
  runFilterBenchmark(state, std::move(colorImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Flood — Skia
// ---------------------------------------------------------------------------

void BM_Flood_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  // Create a dummy source image (flood doesn't use it, but we need a canvas).
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  // Flood: premultiplied (0.5*0.8, 0.3*0.8, 0.1*0.8, 0.8) = (102, 61, 20, 204)
  sk_sp<SkImageFilter> filter = SkImageFilters::ColorFilter(
      SkColorFilters::Blend(SkColorSetARGB(204, 102, 61, 20), SkBlendMode::kSrc), nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Offset — Skia
// ---------------------------------------------------------------------------

void BM_Offset_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  sk_sp<SkImageFilter> filter = SkImageFilters::Offset(10.0f, 10.0f, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Merge — Skia
// ---------------------------------------------------------------------------

void BM_Merge_3Input_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels1 = makeRandomPremulPixels(size, size, 42);
  auto pixels2 = makeRandomPremulPixels(size, size, 123);
  auto pixels3 = makeRandomPremulPixels(size, size, 42);
  sk_sp<SkImage> img1 = makeSkImage(pixels1, size, size);
  sk_sp<SkImage> img2 = makeSkImage(pixels2, size, size);
  sk_sp<SkImage> img3 = makeSkImage(pixels3, size, size);

  // Merge uses explicit inputs — the source image drawn to canvas is not used.
  sk_sp<SkImage> srcImage = makeSkImage(pixels1, size, size);

  sk_sp<SkImageFilter> in1 = SkImageFilters::Image(img1, SkFilterMode::kNearest);
  sk_sp<SkImageFilter> in2 = SkImageFilters::Image(img2, SkFilterMode::kNearest);
  sk_sp<SkImageFilter> in3 = SkImageFilters::Image(img3, SkFilterMode::kNearest);

  sk_sp<SkImageFilter> filters[] = {std::move(in1), std::move(in2), std::move(in3)};
  sk_sp<SkImageFilter> filter = SkImageFilters::Merge(filters, 3);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// ComponentTransfer — Skia (via TableARGB color filter)
// ---------------------------------------------------------------------------

void BM_ComponentTransfer_Table_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  // Same gamma-like table as tiny-skia benchmark.
  uint8_t table[256];
  for (int i = 0; i < 256; ++i) {
    table[i] = static_cast<uint8_t>(std::round(std::pow(i / 255.0, 1.0 / 2.2) * 255.0));
  }
  // Identity for alpha channel (nullptr = identity in Skia).
  uint8_t identity[256];
  for (int i = 0; i < 256; ++i) {
    identity[i] = static_cast<uint8_t>(i);
  }

  sk_sp<SkColorFilter> cf = SkColorFilters::TableARGB(identity, table, table, table);
  sk_sp<SkImageFilter> filter = SkImageFilters::ColorFilter(std::move(cf), nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Tile — Skia
// ---------------------------------------------------------------------------

void BM_Tile_Skia(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  auto pixels = makeRandomPremulPixels(size, size);
  sk_sp<SkImage> srcImage = makeSkImage(pixels, size, size);

  SkRect srcRect = SkRect::MakeWH(64, 64);
  SkRect dstRect = SkRect::MakeWH(static_cast<float>(size), static_cast<float>(size));
  sk_sp<SkImageFilter> filter = SkImageFilters::Tile(srcRect, dstRect, nullptr);
  runFilterBenchmark(state, std::move(srcImage), std::move(filter), size);
}

// ---------------------------------------------------------------------------
// Register benchmarks — same args as FilterPerfBench.cpp for direct comparison.
// ---------------------------------------------------------------------------

// Blur.
BENCHMARK(BM_GaussianBlur_Skia)->Args({512, 3})->Args({512, 6})->Args({512, 20})->Args({1024, 6});

// Morphology.
BENCHMARK(BM_Morphology_Dilate_Skia)->Args({512, 3})->Args({512, 10})->Args({512, 30});
BENCHMARK(BM_Morphology_Erode_Skia)->Args({512, 3})->Args({512, 10})->Args({512, 30});

// Blend.
BENCHMARK(BM_Blend_Multiply_Skia)->Arg(512);
BENCHMARK(BM_Blend_Screen_Skia)->Arg(512);

// Composite.
BENCHMARK(BM_Composite_Over_Skia)->Arg(512);
BENCHMARK(BM_Composite_Arithmetic_Skia)->Arg(512);

// ColorMatrix.
BENCHMARK(BM_ColorMatrix_Saturate_Skia)->Arg(512);

// ConvolveMatrix.
BENCHMARK(BM_ConvolveMatrix_3x3_Skia)->Arg(512);
BENCHMARK(BM_ConvolveMatrix_5x5_Skia)->Arg(512);

// Turbulence.
BENCHMARK(BM_Turbulence_Skia)->Arg(256)->Arg(512);
BENCHMARK(BM_FractalNoise_Skia)->Arg(256)->Arg(512);

// Lighting.
BENCHMARK(BM_DiffuseLighting_Point_Skia)->Arg(512);
BENCHMARK(BM_SpecularLighting_Point_Skia)->Arg(512);

// DisplacementMap.
BENCHMARK(BM_DisplacementMap_Skia)->Arg(512);

// Flood.
BENCHMARK(BM_Flood_Skia)->Arg(512);

// Offset.
BENCHMARK(BM_Offset_Skia)->Arg(512);

// Merge.
BENCHMARK(BM_Merge_3Input_Skia)->Arg(512);

// ComponentTransfer.
BENCHMARK(BM_ComponentTransfer_Table_Skia)->Arg(512);

// Tile.
BENCHMARK(BM_Tile_Skia)->Arg(512);

}  // namespace
