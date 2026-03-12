#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/ColorSpace.h"
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/GaussianBlur.h"
#include "tiny_skia/filter/Morphology.h"

namespace {

using tiny_skia::IntSize;
using tiny_skia::Pixmap;
using tiny_skia::filter::BlurEdgeMode;
using tiny_skia::filter::FloatPixmap;
using tiny_skia::filter::MorphologyOp;

/// Create a pixmap filled with pseudo-random RGBA data (deterministic seed for reproducibility).
Pixmap makeRandomPixmap(std::uint32_t width, std::uint32_t height) {
  const std::size_t count = static_cast<std::size_t>(width) * height * 4;
  std::vector<std::uint8_t> data(count);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& v : data) {
    v = static_cast<std::uint8_t>(dist(rng));
  }
  // Make alpha >= max(r,g,b) to ensure valid premultiplied data.
  for (std::size_t i = 0; i + 3 < count; i += 4) {
    data[i + 3] = std::max({data[i], data[i + 1], data[i + 2], data[i + 3]});
  }
  return *Pixmap::fromVec(std::move(data), IntSize::fromWH(width, height).value());
}

// ---------------------------------------------------------------------------
// Gaussian Blur benchmarks
// ---------------------------------------------------------------------------

void BM_GaussianBlur_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const double sigma = state.range(1);
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);

  for (auto _ : state) {
    FloatPixmap copy = fp;  // Copy so each iteration starts from same data.
    tiny_skia::filter::gaussianBlur(copy, sigma, sigma, BlurEdgeMode::None);
    benchmark::DoNotOptimize(copy.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_GaussianBlur_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const double sigma = state.range(1);
  Pixmap src = makeRandomPixmap(size, size);

  for (auto _ : state) {
    Pixmap copy = src;
    tiny_skia::filter::gaussianBlur(copy, sigma, sigma, BlurEdgeMode::None);
    benchmark::DoNotOptimize(copy.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

// ---------------------------------------------------------------------------
// Color Space Conversion benchmarks
// ---------------------------------------------------------------------------

void BM_SrgbToLinear_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);

  for (auto _ : state) {
    FloatPixmap copy = fp;
    tiny_skia::filter::srgbToLinear(copy);
    benchmark::DoNotOptimize(copy.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_LinearToSrgb_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);
  // Convert to linear first so the data is in the right domain.
  tiny_skia::filter::srgbToLinear(fp);

  for (auto _ : state) {
    FloatPixmap copy = fp;
    tiny_skia::filter::linearToSrgb(copy);
    benchmark::DoNotOptimize(copy.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_SrgbToLinear_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);

  for (auto _ : state) {
    Pixmap copy = src;
    tiny_skia::filter::srgbToLinear(copy);
    benchmark::DoNotOptimize(copy.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

// ---------------------------------------------------------------------------
// Morphology benchmarks
// ---------------------------------------------------------------------------

void BM_Morphology_Dilate_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const int radius = static_cast<int>(state.range(1));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fpSrc = FloatPixmap::fromPixmap(src);

  for (auto _ : state) {
    auto fpDst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::morphology(fpSrc, fpDst, MorphologyOp::Dilate, radius, radius);
    benchmark::DoNotOptimize(fpDst.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_Morphology_Erode_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  const int radius = static_cast<int>(state.range(1));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fpSrc = FloatPixmap::fromPixmap(src);

  for (auto _ : state) {
    auto fpDst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::morphology(fpSrc, fpDst, MorphologyOp::Erode, radius, radius);
    benchmark::DoNotOptimize(fpDst.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

// ---------------------------------------------------------------------------
// FloatPixmap Conversion benchmarks
// ---------------------------------------------------------------------------

void BM_FloatPixmap_FromPixmap(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);

  for (auto _ : state) {
    FloatPixmap fp = FloatPixmap::fromPixmap(src);
    benchmark::DoNotOptimize(fp.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_FloatPixmap_ToPixmap(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);

  for (auto _ : state) {
    Pixmap result = fp.toPixmap();
    benchmark::DoNotOptimize(result.data().data());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
}

// ---------------------------------------------------------------------------
// Register benchmarks
// ---------------------------------------------------------------------------

// Gaussian blur: test at different sizes and sigma values.
// sigma=3 (small, weighted kernel), sigma=6 (medium, box blur), sigma=20 (large box blur).
BENCHMARK(BM_GaussianBlur_Float)->Args({512, 3})->Args({512, 6})->Args({512, 20})->Args({1024, 6});
BENCHMARK(BM_GaussianBlur_Uint8)->Args({512, 3})->Args({512, 6})->Args({512, 20})->Args({1024, 6});

// Color space conversion.
BENCHMARK(BM_SrgbToLinear_Float)->Arg(512)->Arg(1024);
BENCHMARK(BM_LinearToSrgb_Float)->Arg(512)->Arg(1024);
BENCHMARK(BM_SrgbToLinear_Uint8)->Arg(512)->Arg(1024);

// Morphology: test at different radii.
BENCHMARK(BM_Morphology_Dilate_Float)->Args({512, 3})->Args({512, 10})->Args({512, 30});
BENCHMARK(BM_Morphology_Erode_Float)->Args({512, 3})->Args({512, 10})->Args({512, 30});

// FloatPixmap conversion.
BENCHMARK(BM_FloatPixmap_FromPixmap)->Arg(512)->Arg(1024);
BENCHMARK(BM_FloatPixmap_ToPixmap)->Arg(512)->Arg(1024);

}  // namespace
