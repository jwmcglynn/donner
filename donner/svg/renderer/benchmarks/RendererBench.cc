/// @file
/// Geode renderer benchmark — wall-clock and GPU-timestamp timing for
/// RendererGeode across SVGs of varying complexity.
///
/// Reports per-SVG per-phase median/min/max frame times and a geometric-mean
/// summary. Designed for `bazel run --config=geode -c opt`:
///
/// ```
/// bazel run --config=geode -c opt \
///   //donner/svg/renderer/benchmarks:renderer_bench
/// ```
///
/// Additional SVG files can be passed as positional arguments:
///
/// ```
/// bazel run --config=geode -c opt \
///   //donner/svg/renderer/benchmarks:renderer_bench -- \
///   donner/svg/renderer/testdata/Ghostscript_Tiger.svg
/// ```

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Inline SVG workloads (always available, no file I/O required).
// ---------------------------------------------------------------------------

/// Tiny workload: three basic shapes.
constexpr std::string_view kSimpleShapesSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <rect x="10" y="10" width="80" height="80" fill="red"/>
  <circle cx="150" cy="50" r="40" fill="blue"/>
  <ellipse cx="100" cy="150" rx="60" ry="30" fill="green"/>
</svg>
)SVG";

/// Medium workload: curves, a gradient, and overlapping paths.
constexpr std::string_view kModerateSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
  <defs>
    <linearGradient id="g1" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="red"/>
      <stop offset="1" stop-color="blue"/>
    </linearGradient>
  </defs>
  <path d="M50,50 C100,0 200,0 250,50 L300,150 Q250,300 150,280 L80,200
           C30,160 20,100 50,50 Z" fill="#336699" opacity="0.8"/>
  <path d="M200,20 L350,180 L280,380 L120,380 L50,180 Z"
        fill="none" stroke="#993366" stroke-width="3"/>
  <path d="M100,200 Q150,100 200,200 T300,200"
        fill="none" stroke="orange" stroke-width="2"/>
  <rect x="50" y="300" width="300" height="80" fill="url(#g1)" rx="10"/>
</svg>
)SVG";

/// Heavier workload: a star-burst pattern with many overlapping stroked paths.
constexpr std::string_view kComplexSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 500 500">
  <g transform="translate(250,250)">
    <path d="M0,-200 L30,-60 L190,-60 L50,30 L80,180 L0,80
             L-80,180 L-50,30 L-190,-60 L-30,-60 Z"
          fill="#e8b030" stroke="#333" stroke-width="2"/>
    <circle r="40" fill="#fff" opacity="0.6"/>
    <path d="M-150,-150 Q0,-250 150,-150 T150,150 Q0,250 -150,150 T-150,-150 Z"
          fill="none" stroke="darkred" stroke-width="1.5"/>
    <path d="M-120,0 C-120,-160 120,-160 120,0 S-120,160 -120,0 Z"
          fill="rgba(70,130,180,0.3)" stroke="steelblue" stroke-width="1"/>
    <rect x="-180" y="-180" width="360" height="360" rx="40"
          fill="none" stroke="#555" stroke-width="0.5" stroke-dasharray="8,4"/>
    <ellipse rx="200" ry="100" fill="none" stroke="#888" stroke-width="0.8"/>
    <line x1="-220" y1="0" x2="220" y2="0" stroke="#aaa" stroke-width="0.5"/>
    <line x1="0" y1="-220" x2="0" y2="220" stroke="#aaa" stroke-width="0.5"/>
    <path d="M-100,-80 C-40,-200 40,-200 100,-80 L60,120
             C20,200 -20,200 -60,120 Z"
          fill="rgba(255,99,71,0.25)" stroke="tomato" stroke-width="1"/>
    <path d="M-60,-180 L60,-180 L100,0 L60,180 L-60,180 L-100,0 Z"
          fill="none" stroke="purple" stroke-width="1" opacity="0.5"/>
  </g>
</svg>
)SVG";

// ---------------------------------------------------------------------------
// Workload descriptor.
// ---------------------------------------------------------------------------

struct Workload {
  std::string name;
  std::string source;
};

// ---------------------------------------------------------------------------
// Timing helpers.
// ---------------------------------------------------------------------------

/// Per-iteration timing sample.
struct Sample {
  double parseMs = 0.0;
  double drawMs = 0.0;
  double snapshotMs = 0.0;
  double gpuRenderPassMs = 0.0;
  double gpuTotalMs = 0.0;
};

struct Stats {
  double median = 0.0;
  double min = 0.0;
  double max = 0.0;
};

double toMs(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

Stats computeStats(std::vector<double>& values) {
  if (values.empty()) {
    return {};
  }
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  const double med = (n % 2 == 1) ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
  return {med, values.front(), values.back()};
}

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------

struct Config {
  int iterations = 10;
  int warmup = 2;
};

Config parseArgs(int argc, char* argv[], std::vector<std::string>& outFiles) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.starts_with("--iterations=")) {
      cfg.iterations = std::max(1, std::atoi(arg.substr(13).data()));
    } else if (arg.starts_with("--warmup=")) {
      cfg.warmup = std::max(0, std::atoi(arg.substr(9).data()));
    } else {
      outFiles.emplace_back(arg);
    }
  }
  return cfg;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return {};
  }
  file.seekg(0, std::ios::end);
  const auto len = file.tellg();
  file.seekg(0);
  std::string data;
  data.resize(static_cast<size_t>(len));
  file.read(data.data(), len);
  return data;
}

// ---------------------------------------------------------------------------
// Benchmark driver.
// ---------------------------------------------------------------------------

struct PhaseStats {
  Stats parse;
  Stats draw;
  Stats snapshot;
  Stats gpuRenderPass;
  Stats gpuTotal;
};

PhaseStats benchmarkWorkload(const Workload& workload, donner::svg::RendererGeode& renderer,
                             const Config& cfg) {
  const int total = cfg.warmup + cfg.iterations;
  std::vector<Sample> samples;
  samples.reserve(static_cast<size_t>(total));

  for (int i = 0; i < total; ++i) {
    Sample s;

    // -- Parse --
    auto t0 = Clock::now();
    donner::ParseWarningSink warningSink = donner::ParseWarningSink::Disabled();
    auto result =
        donner::svg::parser::SVGParser::ParseSVG(workload.source, warningSink);
    auto t1 = Clock::now();
    s.parseMs = toMs(t1 - t0);

    if (result.hasError()) {
      std::fprintf(stderr, "  [SKIP] parse error: %s\n",
                   std::string(result.error().reason).c_str());
      return {};
    }
    donner::svg::SVGDocument doc = std::move(result.result());

    // -- Draw (CPU wall-clock) --
    t0 = Clock::now();
    renderer.draw(doc);
    t1 = Clock::now();
    s.drawMs = toMs(t1 - t0);

    // -- GPU timings --
    auto gpu = renderer.lastFrameTimings();
    s.gpuRenderPassMs = static_cast<double>(gpu.renderPassNs) / 1e6;
    s.gpuTotalMs = static_cast<double>(gpu.totalGpuNs) / 1e6;

    // -- Snapshot (GPU readback) --
    t0 = Clock::now();
    auto bitmap = renderer.takeSnapshot();
    t1 = Clock::now();
    s.snapshotMs = toMs(t1 - t0);
    (void)bitmap;  // Discard — we only care about timing.

    samples.push_back(s);
  }

  // Discard warmup samples.
  std::vector<double> parseVals, drawVals, snapVals, gpuRpVals, gpuTotVals;
  const auto reserve = static_cast<size_t>(cfg.iterations);
  parseVals.reserve(reserve);
  drawVals.reserve(reserve);
  snapVals.reserve(reserve);
  gpuRpVals.reserve(reserve);
  gpuTotVals.reserve(reserve);

  for (int i = cfg.warmup; i < total; ++i) {
    const auto& s = samples[static_cast<size_t>(i)];
    parseVals.push_back(s.parseMs);
    drawVals.push_back(s.drawMs);
    snapVals.push_back(s.snapshotMs);
    gpuRpVals.push_back(s.gpuRenderPassMs);
    gpuTotVals.push_back(s.gpuTotalMs);
  }

  return {computeStats(parseVals), computeStats(drawVals), computeStats(snapVals),
          computeStats(gpuRpVals), computeStats(gpuTotVals)};
}

void printPhase(const char* label, const Stats& stats) {
  std::printf("  %-10s median=%.3fms  min=%.3fms  max=%.3fms\n", label, stats.median, stats.min,
              stats.max);
}

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> extraFiles;
  const Config cfg = parseArgs(argc, argv, extraFiles);

  // -- Build workload list --
  std::vector<Workload> workloads;
  workloads.push_back({"simple_shapes (inline)", std::string(kSimpleShapesSvg)});
  workloads.push_back({"moderate_paths (inline)", std::string(kModerateSvg)});
  workloads.push_back({"complex_scene (inline)", std::string(kComplexSvg)});

  // Default file workloads (available via the `data` BUILD attribute).
  const std::vector<std::string> defaultFiles = {
      "donner/svg/renderer/testdata/Ghostscript_Tiger.svg",
      "donner/svg/renderer/testdata/lion.svg",
  };

  auto tryAddFile = [&workloads](const std::string& path) {
    std::string data = readFile(path);
    if (data.empty()) {
      std::fprintf(stderr, "warning: skipping unreadable file: %s\n", path.c_str());
      return;
    }
    const auto filename = std::filesystem::path(path).filename().string();
    char label[256];
    std::snprintf(label, sizeof(label), "%s (file, %zu bytes)", filename.c_str(), data.size());
    workloads.push_back({label, std::move(data)});
  };

  for (const auto& f : defaultFiles) {
    tryAddFile(f);
  }
  for (const auto& f : extraFiles) {
    tryAddFile(f);
  }

  // -- Create device and renderer --
  auto device = donner::geode::GeodeDevice::CreateHeadless();
  if (!device) {
    std::fprintf(stderr, "error: failed to create GeodeDevice (no GPU?)\n");
    return 1;
  }

  auto sharedDevice = std::shared_ptr<donner::geode::GeodeDevice>(std::move(device));
  donner::svg::RendererGeode renderer(sharedDevice, /*verbose=*/false);
  renderer.enableTimestamps(true);

  // -- Header --
  std::printf("=== Geode Renderer Benchmark ===\n");
  std::printf("backend=RendererGeode (WebGPU/Slug)\n");
  std::printf("timestamps=%s\n", sharedDevice->supportsTimestamps() ? "enabled" : "unsupported");
  std::printf("iterations=%d  warmup=%d\n\n", cfg.iterations, cfg.warmup);

  // -- Run workloads --
  std::vector<double> drawMedians;
  drawMedians.reserve(workloads.size());

  for (const auto& workload : workloads) {
    std::printf("SVG: %s (%zu bytes)\n", workload.name.c_str(), workload.source.size());

    PhaseStats ps = benchmarkWorkload(workload, renderer, cfg);
    printPhase("Parse:", ps.parse);
    printPhase("Draw:", ps.draw);
    printPhase("Snapshot:", ps.snapshot);
    if (sharedDevice->supportsTimestamps()) {
      printPhase("GPU-RP:", ps.gpuRenderPass);
      printPhase("GPU-Tot:", ps.gpuTotal);
    }
    std::printf("\n");

    if (ps.draw.median > 0.0) {
      drawMedians.push_back(ps.draw.median);
    }
  }

  // -- Summary --
  if (!drawMedians.empty()) {
    double logSum = 0.0;
    for (double m : drawMedians) {
      logSum += std::log(m);
    }
    const double geoMean = std::exp(logSum / static_cast<double>(drawMedians.size()));
    std::printf("=== Summary ===\n");
    std::printf("  workloads=%zu\n", drawMedians.size());
    std::printf("  geometric_mean_draw_ms=%.3f\n", geoMean);
  }

  return 0;
}
