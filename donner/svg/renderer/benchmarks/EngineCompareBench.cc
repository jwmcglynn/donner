/// @file
/// Cross-engine benchmark adapter for Donner's Geode renderer, with an
/// optional in-tree TinySkia CPU-backend mode for direct GPU-vs-CPU
/// comparison of Donner's own backends.
///
/// The adapter intentionally measures a newly parsed document's first frame and an unchanged
/// second frame separately. Parse and frame allocation telemetry is collected in dedicated
/// untimed passes so the atomic counters do not distort those wall-clock samples.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace {

using Clock = std::chrono::steady_clock;
using AllocationSnapshot = donner::benchmarks::allocations::Snapshot;

struct Config {
  int iterations = 10;
  int warmup = 2;
  std::string input;
  std::string outputPng;
  std::string backend = "geode";
};

double toMs(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

double median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 == 0) {
    return (values[middle - 1] + values[middle]) / 2.0;
  }
  return values[middle];
}

Config parseArgs(int argc, char* argv[]) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg.starts_with("--iterations=")) {
      config.iterations = std::max(1, std::atoi(arg.substr(13).data()));
    } else if (arg.starts_with("--warmup=")) {
      config.warmup = std::max(0, std::atoi(arg.substr(9).data()));
    } else if (arg.starts_with("--output=")) {
      config.outputPng = arg.substr(9);
    } else if (arg.starts_with("--backend=")) {
      config.backend = arg.substr(10);
    } else if (config.input.empty()) {
      config.input = arg;
    }
  }
  return config;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint64_t readProcStatusKb(std::string_view field) {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with(field)) {
      std::uint64_t value = 0;
      if (std::sscanf(line.c_str() + field.size(), "%" PRIu64, &value) == 1) {
        return static_cast<std::uint64_t>(value);
      }
      return 0;
    }
  }
  return 0;
}

bool parseDocument(std::string_view source, donner::svg::SVGDocument& document) {
  donner::ParseWarningSink warningSink = donner::ParseWarningSink::Disabled();
  auto parsed = donner::svg::parser::SVGParser::ParseSVG(source, warningSink);
  if (parsed.hasError()) {
    std::fprintf(stderr, "parse error: %s\n", std::string(parsed.error().reason).c_str());
    return false;
  }
  document = std::move(parsed.result());
  return true;
}

template <typename Renderer>
donner::svg::RendererBitmap renderSettled(Renderer& renderer, donner::svg::SVGDocument& document) {
  renderer.draw(document);
  return renderer.takeSnapshot();
}

/// Timed first/second-frame loop. `makeRenderer` constructs a fresh renderer
/// per iteration, mirroring per-document renderer lifetime in production.
template <typename MakeRenderer>
bool runTimedLoop(const Config& config, const std::string& source, MakeRenderer makeRenderer,
                  std::vector<double>& parseTimes, std::vector<double>& firstFrameTimes,
                  std::vector<double>& secondFrameTimes,
                  donner::svg::RendererBitmap& finalBitmap) {
  const int total = config.warmup + config.iterations;
  for (int i = 0; i < total; ++i) {
    donner::svg::SVGDocument document;
    auto start = Clock::now();
    if (!parseDocument(source, document)) {
      return false;
    }
    const double parseMs = toMs(Clock::now() - start);

    auto renderer = makeRenderer();
    start = Clock::now();
    donner::svg::RendererBitmap firstBitmap = renderSettled(renderer, document);
    const double firstFrameMs = toMs(Clock::now() - start);
    start = Clock::now();
    donner::svg::RendererBitmap secondBitmap = renderSettled(renderer, document);
    const double secondFrameMs = toMs(Clock::now() - start);
    if (firstBitmap.empty() || secondBitmap.empty()) {
      std::fprintf(stderr, "renderer returned an empty bitmap\n");
      return false;
    }

    if (i >= config.warmup) {
      parseTimes.push_back(parseMs);
      firstFrameTimes.push_back(firstFrameMs);
      secondFrameTimes.push_back(secondFrameMs);
      finalBitmap = std::move(secondBitmap);
    }
  }
  return true;
}

/// Untimed allocation pass: parse once, then first and second settled render
/// under allocation scopes.
template <typename MakeRenderer>
bool runAllocationPass(const std::string& source, MakeRenderer makeRenderer,
                       AllocationSnapshot& parseAllocations, AllocationSnapshot& firstAllocations,
                       AllocationSnapshot& secondAllocations) {
  donner::svg::SVGDocument allocationDocument;
  donner::benchmarks::allocations::Scope parseAllocationScope;
  if (!parseDocument(source, allocationDocument)) {
    return false;
  }
  parseAllocations = parseAllocationScope.stop();

  auto allocationRenderer = makeRenderer();
  donner::benchmarks::allocations::Scope firstAllocationScope;
  auto firstAllocationBitmap = renderSettled(allocationRenderer, allocationDocument);
  firstAllocations = firstAllocationScope.stop();
  donner::benchmarks::allocations::Scope secondAllocationScope;
  auto secondAllocationBitmap = renderSettled(allocationRenderer, allocationDocument);
  secondAllocations = secondAllocationScope.stop();
  if (firstAllocationBitmap.empty() || secondAllocationBitmap.empty()) {
    std::fprintf(stderr, "renderer returned an empty bitmap\n");
    return false;
  }
  return true;
}

void printAllocation(std::string_view engine, std::string_view phase,
                     const AllocationSnapshot& snapshot) {
  std::printf("ALLOC engine=%.*s phase=%.*s calls=%" PRIu64 " bytes=%" PRIu64
              " frees=%" PRIu64 "\n",
              static_cast<int>(engine.size()), engine.data(), static_cast<int>(phase.size()),
              phase.data(), static_cast<std::uint64_t>(snapshot.allocationCalls),
              static_cast<std::uint64_t>(snapshot.allocationBytes),
              static_cast<std::uint64_t>(snapshot.freeCalls));
}

}  // namespace

int main(int argc, char* argv[]) {
  const Config config = parseArgs(argc, argv);
  if (config.input.empty()) {
    std::fprintf(stderr,
                 "usage: engine_compare_bench [--backend=geode|tiny-skia] [--iterations=N] "
                 "[--warmup=N] [--output=FILE] SVG\n");
    return 2;
  }
  if (config.backend != "geode" && config.backend != "tiny-skia") {
    std::fprintf(stderr, "unknown backend: %s (expected geode or tiny-skia)\n",
                 config.backend.c_str());
    return 2;
  }
  const bool geodeMode = config.backend == "geode";
  const std::string_view engineName = geodeMode ? "Donner" : "TinySkia";

  const std::string source = readFile(config.input);
  if (source.empty()) {
    std::fprintf(stderr, "unable to read SVG: %s\n", config.input.c_str());
    return 2;
  }

  // One-time setup, measured separately: Geode initializes a GPU device;
  // TinySkia constructs its renderer (no GPU). Setup does not recur per frame.
  const std::uint64_t rssBeforeKb = readProcStatusKb("VmRSS:");
  const auto setupStart = Clock::now();
  donner::benchmarks::allocations::Scope setupAllocationScope;
  std::shared_ptr<donner::geode::GeodeDevice> sharedDevice;
  if (geodeMode) {
    auto device = donner::geode::GeodeDevice::CreateHeadless();
    if (!device) {
      std::fprintf(stderr, "unable to create Geode device\n");
      return 1;
    }
    sharedDevice = std::shared_ptr<donner::geode::GeodeDevice>(std::move(device));
  } else {
    donner::svg::RendererTinySkia setupRenderer(/*verbose=*/false);
    (void)setupRenderer;
  }
  const AllocationSnapshot setupAllocations = setupAllocationScope.stop();
  const double setupMs = toMs(Clock::now() - setupStart);
  const std::uint64_t rssAfterSetupKb = readProcStatusKb("VmRSS:");

  std::vector<double> parseTimes;
  std::vector<double> firstFrameTimes;
  std::vector<double> secondFrameTimes;
  donner::svg::RendererBitmap finalBitmap;
  if (geodeMode) {
    if (!runTimedLoop(config, source,
                      [&sharedDevice] {
                        return donner::svg::RendererGeode(sharedDevice, /*verbose=*/false);
                      },
                      parseTimes, firstFrameTimes, secondFrameTimes, finalBitmap)) {
      return 1;
    }
  } else {
    if (!runTimedLoop(config, source,
                      [] { return donner::svg::RendererTinySkia(/*verbose=*/false); },
                      parseTimes, firstFrameTimes, secondFrameTimes, finalBitmap)) {
      return 1;
    }
  }

  AllocationSnapshot parseAllocations;
  AllocationSnapshot firstAllocations;
  AllocationSnapshot secondAllocations;
  if (geodeMode) {
    if (!runAllocationPass(source,
                           [&sharedDevice] {
                             return donner::svg::RendererGeode(sharedDevice, /*verbose=*/false);
                           },
                           parseAllocations, firstAllocations, secondAllocations)) {
      return 1;
    }
  } else {
    if (!runAllocationPass(source, [] { return donner::svg::RendererTinySkia(/*verbose=*/false); },
                           parseAllocations, firstAllocations, secondAllocations)) {
      return 1;
    }
  }

  if (!config.outputPng.empty() &&
      !donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
          config.outputPng.c_str(), finalBitmap.pixels, finalBitmap.dimensions.x,
          finalBitmap.dimensions.y, finalBitmap.rowBytes / 4)) {
    std::fprintf(stderr, "unable to write PNG: %s\n", config.outputPng.c_str());
    return 1;
  }

  const std::uint64_t peakRssKb = readProcStatusKb("VmHWM:");
  std::printf(
      "RESULT engine=%.*s setup_ms=%.3f parse_ms=%.3f first_ms=%.3f second_ms=%.3f "
      "width=%d height=%d rss_before_kb=%" PRIu64 " rss_setup_kb=%" PRIu64
      " peak_rss_kb=%" PRIu64 "\n",
      static_cast<int>(engineName.size()), engineName.data(), setupMs, median(parseTimes),
      median(firstFrameTimes), median(secondFrameTimes), finalBitmap.dimensions.x,
      finalBitmap.dimensions.y, static_cast<std::uint64_t>(rssBeforeKb),
      static_cast<std::uint64_t>(rssAfterSetupKb), static_cast<std::uint64_t>(peakRssKb));
  printAllocation(engineName, "setup", setupAllocations);
  printAllocation(engineName, "parse", parseAllocations);
  printAllocation(engineName, "first", firstAllocations);
  printAllocation(engineName, "second", secondAllocations);
  return 0;
}
