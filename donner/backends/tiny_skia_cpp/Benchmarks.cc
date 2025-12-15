#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "donner/backends/tiny_skia_cpp/Paint.h"
#include "donner/backends/tiny_skia_cpp/Rasterizer.h"
#include "donner/backends/tiny_skia_cpp/Shader.h"
#include "donner/backends/tiny_skia_cpp/Transform.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

using svg::PathSpline;

struct RunStats {
  uint64_t samples = 0;
  uint64_t checksum = 0;
};

struct BenchmarkResult {
  std::string name;
  uint64_t samples = 0;
  double elapsedMs = 0.0;
  double nanosPerSample = 0.0;
  uint64_t checksum = 0;
};

template <typename Fn>
BenchmarkResult runBenchmark(const std::string& name, int iterations, Fn&& fn) {
  constexpr int kWarmupIterations = 3;
  for (int i = 0; i < kWarmupIterations; ++i) {
    fn();
  }

  RunStats aggregate;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    const RunStats stats = fn();
    aggregate.samples += stats.samples;
    aggregate.checksum ^= stats.checksum;
  }
  const auto end = std::chrono::steady_clock::now();

  const double elapsedMs =
      std::chrono::duration<double, std::milli>(end - start).count();
  const double nanosPerSample = aggregate.samples == 0
                                    ? 0.0
                                    : (elapsedMs * 1'000'000.0) /
                                          static_cast<double>(aggregate.samples);

  return BenchmarkResult{name, aggregate.samples, elapsedMs, nanosPerSample,
                         aggregate.checksum};
}

Shader buildLinearRepeatShader() {
  std::vector<GradientStop> stops = {
      GradientStop{0.0f, Color::RGB(0x10, 0x20, 0x30)},
      GradientStop{0.5f, Color::RGB(0x90, 0x60, 0x40)},
      GradientStop{1.0f, Color::RGB(0xF0, 0xF0, 0xE0)},
  };

  auto shader = Shader::MakeLinearGradient({0.0, 0.0}, {256.0, 0.0},
                                           std::move(stops), SpreadMode::kRepeat,
                                           Transform());
  if (!shader.hasValue()) {
    std::cerr << "Failed to create gradient shader: " << shader.error() << "\n";
    std::exit(1);
  }
  return std::move(shader.value());
}

RunStats sampleGradientSpan(const ShaderContext& context, int width, int height) {
  uint64_t checksum = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color color = context.sample({static_cast<double>(x) + 0.5,
                                          static_cast<double>(y) + 0.5});
      checksum += static_cast<uint64_t>(color.r) + color.g + color.b + color.a;
    }
  }

  return RunStats{static_cast<uint64_t>(width) * static_cast<uint64_t>(height),
                  checksum};
}

PaintContext buildSolidPaint() {
  Paint paint;
  paint.color = Color::RGB(0xA0, 0x40, 0x30);
  paint.opacity = 0.85f;

  auto paintContext = PaintContext::Create(paint);
  if (!paintContext.hasValue()) {
    std::cerr << "Failed to create paint context: " << paintContext.error() << "\n";
    std::exit(1);
  }
  return std::move(paintContext.value());
}

RunStats blendSolidSpans(Pixmap& pixmap, const PaintContext& paintContext) {
  uint64_t checksum = 0;
  for (int y = 0; y < pixmap.height(); ++y) {
    BlendSpan(pixmap, 0, y, pixmap.width(), paintContext);
    const std::span<const uint8_t> row =
        pixmap.pixels().subspan(static_cast<size_t>(y) * pixmap.strideBytes(), 4);
    checksum += static_cast<uint64_t>(row[0]) + row[1] + row[2] + row[3];
  }

  return RunStats{static_cast<uint64_t>(pixmap.width()) *
                      static_cast<uint64_t>(pixmap.height()),
                  checksum};
}

PathSpline buildRasterSpline() {
  PathSpline spline;
  spline.moveTo({8.0, 8.0});
  spline.curveTo({128.0, 32.0}, {256.0, 128.0}, {384.0, 32.0});
  spline.lineTo({480.0, 192.0});
  spline.curveTo({320.0, 224.0}, {256.0, 352.0}, {192.0, 224.0});
  spline.lineTo({64.0, 256.0});
  spline.closePath();
  return spline;
}

RunStats rasterizeFill(const PathSpline& spline, int width, int height) {
  const Mask mask = RasterizeFill(spline, width, height, FillRule::kNonZero, true,
                                  Transform());
  uint64_t checksum = 0;
  for (const uint8_t value : mask.pixels()) {
    checksum += value;
  }

  return RunStats{static_cast<uint64_t>(width) * static_cast<uint64_t>(height),
                  checksum};
}

void printResult(const BenchmarkResult& result) {
  std::cout << result.name << "\n";
  std::cout << "  samples: " << result.samples << "\n";
  std::cout << "  elapsed_ms: " << result.elapsedMs << "\n";
  std::cout << "  ns_per_sample: " << result.nanosPerSample << "\n";
  std::cout << "  checksum: " << result.checksum << "\n";
}

}  // namespace

struct BenchmarkConfig {
  int iterations = 50;
  bool emitJson = false;
};

void printJsonResult(const BenchmarkResult& result) {
  std::cout << "{\n";
  std::cout << "  \"name\": \"" << result.name << "\",\n";
  std::cout << "  \"samples\": " << result.samples << ",\n";
  std::cout << "  \"elapsed_ms\": " << result.elapsedMs << ",\n";
  std::cout << "  \"ns_per_sample\": " << result.nanosPerSample << ",\n";
  std::cout << "  \"checksum\": " << result.checksum << "\n";
  std::cout << "}\n";
}

BenchmarkConfig parseArgs(const std::span<const char*> args) {
  BenchmarkConfig config;
  for (const std::string_view arg : args) {
    if (arg == "--json") {
      config.emitJson = true;
    } else if (arg.rfind("--iterations=", 0) == 0) {
      const std::string_view value = arg.substr(std::string_view("--iterations=").size());
      int iterations = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), iterations);
      if (result.ec != std::errc{}) {
        std::cerr << "Invalid iteration count: " << value << "\n";
        std::exit(1);
      }
      config.iterations = iterations;
    } else {
      std::cerr << "Unknown flag: " << arg << "\n";
      std::exit(1);
    }
  }

  if (config.iterations <= 0) {
    std::cerr << "Iteration count must be positive\n";
    std::exit(1);
  }

  return config;
}

int RunBenchmarksMain(const BenchmarkConfig& config) {
  constexpr int kSampleWidth = 512;
  constexpr int kSampleHeight = 512;

  auto gradientContext = ShaderContext::Create(buildLinearRepeatShader());
  if (!gradientContext.hasValue()) {
    std::cerr << "Failed to create gradient context: " << gradientContext.error() << "\n";
    return 1;
  }
  const BenchmarkResult gradientResult = runBenchmark(
      "linear_gradient_sample", config.iterations,
      [&]() {
        return sampleGradientSpan(gradientContext.value(), kSampleWidth, kSampleHeight);
      });

  Pixmap pixmap = Pixmap::Create(kSampleWidth, kSampleHeight);
  const PaintContext paintContext = buildSolidPaint();
  const BenchmarkResult spanResult =
      runBenchmark("blend_span", config.iterations,
                   [&]() { return blendSolidSpans(pixmap, paintContext); });

  const PathSpline spline = buildRasterSpline();
  const BenchmarkResult rasterResult = runBenchmark(
      "rasterize_fill", config.iterations,
      [&]() { return rasterizeFill(spline, kSampleWidth, kSampleHeight); });

  if (config.emitJson) {
    printJsonResult(gradientResult);
    printJsonResult(spanResult);
    printJsonResult(rasterResult);
  } else {
    printResult(gradientResult);
    printResult(spanResult);
    printResult(rasterResult);
  }

  return 0;
}

}  // namespace donner::backends::tiny_skia_cpp

int main(int argc, char** argv) {
  std::vector<const char*> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      arguments.push_back(argv[i]);
    }
  }

  const auto config =
      donner::backends::tiny_skia_cpp::parseArgs(std::span<const char*>(arguments));
  return donner::backends::tiny_skia_cpp::RunBenchmarksMain(config);
}
