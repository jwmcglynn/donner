#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/Blend.h"
#include "tiny_skia/filter/ColorMatrix.h"
#include "tiny_skia/filter/ColorSpace.h"
#include "tiny_skia/filter/ComponentTransfer.h"
#include "tiny_skia/filter/Composite.h"
#include "tiny_skia/filter/ConvolveMatrix.h"
#include "tiny_skia/filter/DisplacementMap.h"
#include "tiny_skia/filter/Flood.h"
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/GaussianBlur.h"
#include "tiny_skia/filter/Lighting.h"
#include "tiny_skia/filter/Merge.h"
#include "tiny_skia/filter/Morphology.h"
#include "tiny_skia/filter/Offset.h"
#include "tiny_skia/filter/Tile.h"
#include "tiny_skia/filter/Turbulence.h"

namespace {

using tiny_skia::IntSize;
using tiny_skia::Pixmap;
using tiny_skia::filter::BlendMode;
using tiny_skia::filter::BlurEdgeMode;
using tiny_skia::filter::CompositeOp;
using tiny_skia::filter::ConvolveEdgeMode;
using tiny_skia::filter::ConvolveParams;
using tiny_skia::filter::DiffuseLightingParams;
using tiny_skia::filter::DisplacementChannel;
using tiny_skia::filter::FloatPixmap;
using tiny_skia::filter::LightSourceParams;
using tiny_skia::filter::LightType;
using tiny_skia::filter::MorphologyOp;
using tiny_skia::filter::SpecularLightingParams;
using tiny_skia::filter::TurbulenceParams;
using tiny_skia::filter::TurbulenceType;

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

/// Create a second random pixmap with different seed.
Pixmap makeRandomPixmap2(std::uint32_t width, std::uint32_t height) {
  const std::size_t count = static_cast<std::size_t>(width) * height * 4;
  std::vector<std::uint8_t> data(count);
  std::mt19937 rng(123);
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& v : data) {
    v = static_cast<std::uint8_t>(dist(rng));
  }
  for (std::size_t i = 0; i + 3 < count; i += 4) {
    data[i + 3] = std::max({data[i], data[i + 1], data[i + 2], data[i + 3]});
  }
  return *Pixmap::fromVec(std::move(data), IntSize::fromWH(width, height).value());
}

void setCounters(benchmark::State& state, std::uint32_t size) {
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(size) * size);
  state.counters["pixelsPerSecond"] = benchmark::Counter(
      static_cast<double>(size) * size, benchmark::Counter::kIsIterationInvariantRate);
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
    FloatPixmap copy = fp;
    tiny_skia::filter::gaussianBlur(copy, sigma, sigma, BlurEdgeMode::None);
    benchmark::DoNotOptimize(copy.data().data());
  }
  setCounters(state, size);
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
  setCounters(state, size);
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
  setCounters(state, size);
}

void BM_LinearToSrgb_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);
  tiny_skia::filter::srgbToLinear(fp);

  for (auto _ : state) {
    FloatPixmap copy = fp;
    tiny_skia::filter::linearToSrgb(copy);
    benchmark::DoNotOptimize(copy.data().data());
  }
  setCounters(state, size);
}

void BM_SrgbToLinear_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);

  for (auto _ : state) {
    Pixmap copy = src;
    tiny_skia::filter::srgbToLinear(copy);
    benchmark::DoNotOptimize(copy.data().data());
  }
  setCounters(state, size);
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
  setCounters(state, size);
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
  setCounters(state, size);
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
  setCounters(state, size);
}

void BM_FloatPixmap_ToPixmap(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);

  for (auto _ : state) {
    Pixmap result = fp.toPixmap();
    benchmark::DoNotOptimize(result.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Blend benchmarks
// ---------------------------------------------------------------------------

void BM_Blend_Multiply_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src1 = makeRandomPixmap(size, size);
  Pixmap src2 = makeRandomPixmap2(size, size);
  FloatPixmap bg = FloatPixmap::fromPixmap(src1);
  FloatPixmap fg = FloatPixmap::fromPixmap(src2);

  for (auto _ : state) {
    auto dst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::blend(bg, fg, dst, BlendMode::Multiply);
    benchmark::DoNotOptimize(dst.data().data());
  }
  setCounters(state, size);
}

void BM_Blend_Screen_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src1 = makeRandomPixmap(size, size);
  Pixmap src2 = makeRandomPixmap2(size, size);
  FloatPixmap bg = FloatPixmap::fromPixmap(src1);
  FloatPixmap fg = FloatPixmap::fromPixmap(src2);

  for (auto _ : state) {
    auto dst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::blend(bg, fg, dst, BlendMode::Screen);
    benchmark::DoNotOptimize(dst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Composite benchmarks
// ---------------------------------------------------------------------------

void BM_Composite_Over_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src1 = makeRandomPixmap(size, size);
  Pixmap src2 = makeRandomPixmap2(size, size);
  FloatPixmap in1 = FloatPixmap::fromPixmap(src1);
  FloatPixmap in2 = FloatPixmap::fromPixmap(src2);

  for (auto _ : state) {
    auto dst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::composite(in1, in2, dst, CompositeOp::Over);
    benchmark::DoNotOptimize(dst.data().data());
  }
  setCounters(state, size);
}

void BM_Composite_Arithmetic_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src1 = makeRandomPixmap(size, size);
  Pixmap src2 = makeRandomPixmap2(size, size);
  FloatPixmap in1 = FloatPixmap::fromPixmap(src1);
  FloatPixmap in2 = FloatPixmap::fromPixmap(src2);

  for (auto _ : state) {
    auto dst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::composite(in1, in2, dst, CompositeOp::Arithmetic, 0.5, 0.3, 0.2, 0.0);
    benchmark::DoNotOptimize(dst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// ColorMatrix benchmarks
// ---------------------------------------------------------------------------

void BM_ColorMatrix_Saturate_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);
  const auto matrix = tiny_skia::filter::saturateMatrix(0.5);

  for (auto _ : state) {
    FloatPixmap copy = fp;
    tiny_skia::filter::colorMatrix(copy, matrix);
    benchmark::DoNotOptimize(copy.data().data());
  }
  setCounters(state, size);
}

void BM_ColorMatrix_HueRotate_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fp = FloatPixmap::fromPixmap(src);
  const auto matrix = tiny_skia::filter::hueRotateMatrix(90.0);

  for (auto _ : state) {
    FloatPixmap copy = fp;
    tiny_skia::filter::colorMatrix(copy, matrix);
    benchmark::DoNotOptimize(copy.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// ConvolveMatrix benchmarks
// ---------------------------------------------------------------------------

void BM_ConvolveMatrix_3x3_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fpSrc = FloatPixmap::fromPixmap(src);

  // Edge-detect kernel.
  const double kernel[] = {-1, -1, -1, -1, 8, -1, -1, -1, -1};
  ConvolveParams params;
  params.orderX = 3;
  params.orderY = 3;
  params.kernel = kernel;
  params.divisor = 1.0;
  params.targetX = 1;
  params.targetY = 1;
  params.edgeMode = ConvolveEdgeMode::None;

  for (auto _ : state) {
    auto fpDst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::convolveMatrix(fpSrc, fpDst, params);
    benchmark::DoNotOptimize(fpDst.data().data());
  }
  setCounters(state, size);
}

void BM_ConvolveMatrix_5x5_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fpSrc = FloatPixmap::fromPixmap(src);

  // 5x5 Gaussian-like kernel.
  const double kernel[] = {1, 4,  7,  4,  1, 4,  16, 26, 16, 4, 7, 26, 41,
                           26, 7, 4,  16, 26, 16, 4,  1,  4,  7, 4, 1};
  ConvolveParams params;
  params.orderX = 5;
  params.orderY = 5;
  params.kernel = kernel;
  params.divisor = 273.0;
  params.targetX = 2;
  params.targetY = 2;
  params.edgeMode = ConvolveEdgeMode::None;

  for (auto _ : state) {
    auto fpDst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::convolveMatrix(fpSrc, fpDst, params);
    benchmark::DoNotOptimize(fpDst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Turbulence benchmarks
// ---------------------------------------------------------------------------

void BM_Turbulence_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));

  TurbulenceParams params;
  params.type = TurbulenceType::Turbulence;
  params.baseFrequencyX = 0.05;
  params.baseFrequencyY = 0.05;
  params.numOctaves = 4;
  params.seed = 42.0;

  for (auto _ : state) {
    auto fp = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::turbulence(fp, params);
    benchmark::DoNotOptimize(fp.data().data());
  }
  setCounters(state, size);
}

void BM_FractalNoise_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));

  TurbulenceParams params;
  params.type = TurbulenceType::FractalNoise;
  params.baseFrequencyX = 0.05;
  params.baseFrequencyY = 0.05;
  params.numOctaves = 4;
  params.seed = 42.0;

  for (auto _ : state) {
    auto fp = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::turbulence(fp, params);
    benchmark::DoNotOptimize(fp.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Lighting benchmarks
// ---------------------------------------------------------------------------

void BM_DiffuseLighting_Point_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fpSrc = FloatPixmap::fromPixmap(src);

  DiffuseLightingParams params;
  params.surfaceScale = 5.0;
  params.diffuseConstant = 1.0;
  params.lightR = 1.0;
  params.lightG = 1.0;
  params.lightB = 1.0;
  params.light.type = LightType::Point;
  params.light.x = static_cast<double>(size) / 2.0;
  params.light.y = static_cast<double>(size) / 2.0;
  params.light.z = 200.0;

  for (auto _ : state) {
    auto fpDst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::diffuseLighting(fpSrc, fpDst, params);
    benchmark::DoNotOptimize(fpDst.data().data());
  }
  setCounters(state, size);
}

void BM_SpecularLighting_Point_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  FloatPixmap fpSrc = FloatPixmap::fromPixmap(src);

  SpecularLightingParams params;
  params.surfaceScale = 5.0;
  params.specularConstant = 1.0;
  params.specularExponent = 20.0;
  params.lightR = 1.0;
  params.lightG = 1.0;
  params.lightB = 1.0;
  params.light.type = LightType::Point;
  params.light.x = static_cast<double>(size) / 2.0;
  params.light.y = static_cast<double>(size) / 2.0;
  params.light.z = 200.0;

  for (auto _ : state) {
    auto fpDst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::specularLighting(fpSrc, fpDst, params);
    benchmark::DoNotOptimize(fpDst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// DisplacementMap benchmarks
// ---------------------------------------------------------------------------

void BM_DisplacementMap_Float(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);
  Pixmap map = makeRandomPixmap2(size, size);
  FloatPixmap fpSrc = FloatPixmap::fromPixmap(src);
  FloatPixmap fpMap = FloatPixmap::fromPixmap(map);

  for (auto _ : state) {
    auto fpDst = FloatPixmap::fromSize(size, size).value();
    tiny_skia::filter::displacementMap(fpSrc, fpMap, fpDst, 20.0, DisplacementChannel::R,
                                       DisplacementChannel::G);
    benchmark::DoNotOptimize(fpDst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Flood benchmarks
// ---------------------------------------------------------------------------

void BM_Flood_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));

  for (auto _ : state) {
    auto pm = Pixmap::fromSize(size, size).value();
    tiny_skia::filter::flood(pm, 102, 61, 20, 204);
    benchmark::DoNotOptimize(pm.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Offset benchmarks
// ---------------------------------------------------------------------------

void BM_Offset_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);

  for (auto _ : state) {
    auto dst = Pixmap::fromSize(size, size).value();
    tiny_skia::filter::offset(src, dst, 10, 10);
    benchmark::DoNotOptimize(dst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Merge benchmarks
// ---------------------------------------------------------------------------

void BM_Merge_3Input_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src1 = makeRandomPixmap(size, size);
  Pixmap src2 = makeRandomPixmap2(size, size);
  Pixmap src3 = makeRandomPixmap(size, size);

  const Pixmap* layers[] = {&src1, &src2, &src3};

  for (auto _ : state) {
    auto dst = Pixmap::fromSize(size, size).value();
    tiny_skia::filter::merge(std::span<const Pixmap* const>(layers, 3), dst);
    benchmark::DoNotOptimize(dst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// ComponentTransfer benchmarks
// ---------------------------------------------------------------------------

void BM_ComponentTransfer_Table_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);

  // Build a gamma-like table with 256 entries.
  std::vector<double> tableValues(256);
  for (int i = 0; i < 256; ++i) {
    tableValues[static_cast<std::size_t>(i)] = std::pow(i / 255.0, 1.0 / 2.2);
  }

  using tiny_skia::filter::TransferFunc;
  using tiny_skia::filter::TransferFuncType;
  TransferFunc func;
  func.type = TransferFuncType::Table;
  func.tableValues = tableValues;

  TransferFunc identity;
  identity.type = TransferFuncType::Identity;

  for (auto _ : state) {
    Pixmap copy = src;
    tiny_skia::filter::componentTransfer(copy, func, func, func, identity);
    benchmark::DoNotOptimize(copy.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Tile benchmarks
// ---------------------------------------------------------------------------

void BM_Tile_Uint8(benchmark::State& state) {
  const auto size = static_cast<std::uint32_t>(state.range(0));
  Pixmap src = makeRandomPixmap(size, size);

  // Tile a 64x64 region across the full pixmap.
  for (auto _ : state) {
    auto dst = Pixmap::fromSize(size, size).value();
    tiny_skia::filter::tile(src, dst, 0, 0, 64, 64);
    benchmark::DoNotOptimize(dst.data().data());
  }
  setCounters(state, size);
}

// ---------------------------------------------------------------------------
// Register benchmarks
// ---------------------------------------------------------------------------

// Gaussian blur.
BENCHMARK(BM_GaussianBlur_Float)->Args({512, 3})->Args({512, 6})->Args({512, 20})->Args({1024, 6});
BENCHMARK(BM_GaussianBlur_Uint8)->Args({512, 3})->Args({512, 6})->Args({512, 20})->Args({1024, 6});

// Color space conversion.
BENCHMARK(BM_SrgbToLinear_Float)->Arg(512)->Arg(1024);
BENCHMARK(BM_LinearToSrgb_Float)->Arg(512)->Arg(1024);
BENCHMARK(BM_SrgbToLinear_Uint8)->Arg(512)->Arg(1024);

// Morphology.
BENCHMARK(BM_Morphology_Dilate_Float)->Args({512, 3})->Args({512, 10})->Args({512, 30});
BENCHMARK(BM_Morphology_Erode_Float)->Args({512, 3})->Args({512, 10})->Args({512, 30});

// FloatPixmap conversion.
BENCHMARK(BM_FloatPixmap_FromPixmap)->Arg(512)->Arg(1024);
BENCHMARK(BM_FloatPixmap_ToPixmap)->Arg(512)->Arg(1024);

// Blend.
BENCHMARK(BM_Blend_Multiply_Float)->Arg(512);
BENCHMARK(BM_Blend_Screen_Float)->Arg(512);

// Composite.
BENCHMARK(BM_Composite_Over_Float)->Arg(512);
BENCHMARK(BM_Composite_Arithmetic_Float)->Arg(512);

// ColorMatrix.
BENCHMARK(BM_ColorMatrix_Saturate_Float)->Arg(512);
BENCHMARK(BM_ColorMatrix_HueRotate_Float)->Arg(512);

// ConvolveMatrix.
BENCHMARK(BM_ConvolveMatrix_3x3_Float)->Arg(512);
BENCHMARK(BM_ConvolveMatrix_5x5_Float)->Arg(512);

// Turbulence.
BENCHMARK(BM_Turbulence_Float)->Arg(256)->Arg(512);
BENCHMARK(BM_FractalNoise_Float)->Arg(256)->Arg(512);

// Lighting.
BENCHMARK(BM_DiffuseLighting_Point_Float)->Arg(512);
BENCHMARK(BM_SpecularLighting_Point_Float)->Arg(512);

// DisplacementMap.
BENCHMARK(BM_DisplacementMap_Float)->Arg(512);

// Flood.
BENCHMARK(BM_Flood_Uint8)->Arg(512);

// Offset.
BENCHMARK(BM_Offset_Uint8)->Arg(512);

// Merge.
BENCHMARK(BM_Merge_3Input_Uint8)->Arg(512);

// ComponentTransfer.
BENCHMARK(BM_ComponentTransfer_Table_Uint8)->Arg(512);

// Tile.
BENCHMARK(BM_Tile_Uint8)->Arg(512);

}  // namespace
