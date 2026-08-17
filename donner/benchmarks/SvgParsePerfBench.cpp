/// @file
/// Parse-only benchmark for the SVG parser.
///
/// Measures exactly the work that the cross-engine benchmark reports as
/// `parse_ms`: a fresh \ref donner::svg::SVGDocument built from source text by
/// \ref donner::svg::parser::SVGParser::ParseSVG, with warnings disabled, once
/// per iteration. No renderer is constructed and no frame is drawn, so the
/// sample is not diluted by raster or GPU work and the binary can be sampled
/// with a profiler without the parser being buried under rendering symbols.
///
/// Reported time is the median across the timed iterations, which is the same
/// statistic the cross-engine benchmark uses, so numbers from the two tools are
/// directly comparable for the parse phase.
///
/// Usage:
///   svg_parse_perf_bench [--iterations=N] [--warmup=N] [--repeat=N] FILE...
///
/// Each input file produces one `RESULT scene=<name> parse_ms=<median>` line
/// per repeat, and repeats are interleaved across files so that machine drift
/// affects every scene equally.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"

namespace {

using Clock = std::chrono::steady_clock;

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

std::string readFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct Scene {
  std::string name;
  std::string source;
};

/// Parses one document, returning false if the source did not parse. Keeping the
/// document alive until the timer stops means teardown is excluded, matching the
/// cross-engine benchmark's parse phase.
bool parseOnce(const std::string& source, double& elapsedMs) {
  donner::ParseWarningSink warningSink = donner::ParseWarningSink::Disabled();
  const auto start = Clock::now();
  auto parsed = donner::svg::parser::SVGParser::ParseSVG(source, warningSink);
  elapsedMs = toMs(Clock::now() - start);
  if (parsed.hasError()) {
    std::fprintf(stderr, "parse error: %s\n", std::string(parsed.error().reason).c_str());
    return false;
  }
  // Keep the document alive past the timer so destruction is not timed, then
  // discard it so each iteration starts from an empty registry.
  donner::svg::SVGDocument document = std::move(parsed.result());
  (void)document;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  int iterations = 25;
  int warmup = 3;
  int repeat = 1;
  std::vector<std::string> inputs;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg.starts_with("--iterations=")) {
      iterations = std::max(1, std::atoi(std::string(arg.substr(13)).c_str()));
    } else if (arg.starts_with("--warmup=")) {
      warmup = std::max(0, std::atoi(std::string(arg.substr(9)).c_str()));
    } else if (arg.starts_with("--repeat=")) {
      repeat = std::max(1, std::atoi(std::string(arg.substr(9)).c_str()));
    } else {
      inputs.emplace_back(arg);
    }
  }

  if (inputs.empty()) {
    std::fprintf(
        stderr, "usage: svg_parse_perf_bench [--iterations=N] [--warmup=N] [--repeat=N] FILE...\n");
    return 2;
  }

  std::vector<Scene> scenes;
  scenes.reserve(inputs.size());
  for (const std::string& input : inputs) {
    const std::filesystem::path path(input);
    std::string source = readFile(path);
    if (source.empty()) {
      std::fprintf(stderr, "unable to read SVG: %s\n", input.c_str());
      return 2;
    }
    scenes.push_back(Scene{path.filename().string(), std::move(source)});
  }

  for (int run = 0; run < repeat; ++run) {
    for (const Scene& scene : scenes) {
      std::vector<double> samples;
      samples.reserve(static_cast<std::size_t>(iterations));
      for (int i = 0; i < warmup + iterations; ++i) {
        double elapsedMs = 0.0;
        if (!parseOnce(scene.source, elapsedMs)) {
          return 1;
        }
        if (i >= warmup) {
          samples.push_back(elapsedMs);
        }
      }

      std::printf("RESULT scene=%s run=%d iterations=%d parse_ms=%.4f\n", scene.name.c_str(), run,
                  iterations, median(samples));
      std::fflush(stdout);
    }
  }

  return 0;
}
