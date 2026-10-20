/// @file
/// Runs one (backend, scene, size) measurement and appends a JSON Lines record. The driver script
/// starts a fresh process per measurement so process memory peaks and allocator state belong to a
/// single backend and scene.
///
/// Usage:
///   renderer_bakeoff --describe
///   renderer_bakeoff --list-synthetic
///   renderer_bakeoff --backend NAME (--scene-file PATH | --synthetic NAME) [--size N]
///       [--warmup N] [--frames N] [--out-json PATH] [--out-png PATH]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "harness/BenchBackend.h"
#include "harness/FeatureSupport.h"
#include "harness/JsonWriter.h"
#include "harness/ProcessMemory.h"
#include "harness/Stats.h"
#include "harness/SyntheticScenes.h"
#include "tools/svg_suite/ImageCompare.h"

namespace donner::bakeoff {
namespace {

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Largest output edge accepted, matching common GPU texture limits.
constexpr int kMaximumSize = 8192;

struct Options {
  bool describe = false;
  bool listSynthetic = false;
  std::string backend;
  std::string sceneFile;
  std::string synthetic;
  int size = 1024;
  int warmup = 10;
  int frames = 50;
  std::string outJson;
  std::string outPng;
};

/// Parses a decimal integer in [minimum, maximum].
std::optional<int> ParseInt(const char* text, int minimum, int maximum) {
  char* end = nullptr;
  const long parsed = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
    return std::nullopt;
  }
  return static_cast<int>(parsed);
}

std::optional<Options> ParseOptions(int argc, char** argv) {
  Options options;
  struct StringFlag {
    std::string_view name;
    std::string Options::* field;
  };
  struct IntFlag {
    std::string_view name;
    int Options::* field;
    int minimum;
    int maximum;
  };
  const StringFlag stringFlags[] = {{"--backend", &Options::backend},
                                    {"--scene-file", &Options::sceneFile},
                                    {"--synthetic", &Options::synthetic},
                                    {"--out-json", &Options::outJson},
                                    {"--out-png", &Options::outPng}};
  const IntFlag intFlags[] = {{"--size", &Options::size, 1, kMaximumSize},
                              {"--warmup", &Options::warmup, 0, 100000},
                              {"--frames", &Options::frames, 1, 100000}};

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--describe" || arg == "--list-synthetic") {
      (arg == "--describe" ? options.describe : options.listSynthetic) = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "unknown argument or missing value: %s\n", argv[i]);
      return std::nullopt;
    }
    const char* value = argv[++i];
    const auto stringFlag = std::find_if(std::begin(stringFlags), std::end(stringFlags),
                                         [arg](const StringFlag& f) { return f.name == arg; });
    if (stringFlag != std::end(stringFlags)) {
      options.*(stringFlag->field) = value;
      continue;
    }
    const auto intFlag = std::find_if(std::begin(intFlags), std::end(intFlags),
                                      [arg](const IntFlag& f) { return f.name == arg; });
    if (intFlag == std::end(intFlags)) {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i - 1]);
      return std::nullopt;
    }
    const std::optional<int> parsed = ParseInt(value, intFlag->minimum, intFlag->maximum);
    if (!parsed) {
      std::fprintf(stderr, "%s must be an integer in [%d, %d]\n", argv[i - 1], intFlag->minimum,
                   intFlag->maximum);
      return std::nullopt;
    }
    options.*(intFlag->field) = *parsed;
  }
  return options;
}

void WriteCapabilities(JsonWriter& json, const CapabilityTable& capabilities) {
  json.beginObject("capabilities");
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    const auto feature = static_cast<Feature>(i);
    json.beginObject(FeatureName(feature));
    json.string("support", SupportName(capabilities.get(feature)));
    if (!capabilities.notes[i].empty()) {
      json.string("note", capabilities.notes[i]);
    }
    json.endObject();
  }
  json.endObject();
}

int Describe() {
  JsonWriter json;
  json.beginArray();
  for (const BackendInfo& info : RegisteredBackends()) {
    json.beginObject();
    json.string("name", info.name);
    json.string("suite", SuiteName(info.suite));
    json.string("library", info.library);
    json.string("api", info.api);
    json.integer("threads", info.threads);
    json.string("configuration", info.configuration);
    json.boolean("donner_baseline", info.donnerBaseline);
    WriteCapabilities(json, info.capabilities);
    json.endObject();
  }
  json.endArray();
  std::printf("%s\n", json.str().c_str());
  return 0;
}

void WriteMemory(JsonWriter& json, std::string_view key, const MemorySample& sample) {
  json.beginObject(key);
  json.integer("resident_bytes", sample.residentBytes);
  json.integer("footprint_bytes", sample.footprintBytes);
  json.integer("peak_bytes", sample.peakBytes);
  json.integer("graphics_bytes", sample.graphicsBytes);
  json.endObject();
}

void WriteStats(JsonWriter& json, std::string_view key, const SampleStats& stats) {
  json.beginObject(key);
  json.integer("count", static_cast<std::int64_t>(stats.count));
  json.number("min_ms", stats.min);
  json.number("median_ms", stats.median);
  json.number("p95_ms", stats.p95);
  json.number("max_ms", stats.max);
  json.number("mean_ms", stats.mean);
  json.number("stddev_ms", stats.stddev);
  json.endObject();
}

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

int Run(const Options& options) {
  const BackendInfo* info = FindBackend(options.backend);
  if (info == nullptr) {
    std::fprintf(stderr, "unknown backend: %s\n", options.backend.c_str());
    return 2;
  }

  std::string sceneName;
  std::optional<std::string> source;
  if (!options.sceneFile.empty()) {
    sceneName = std::filesystem::path(options.sceneFile).stem().string();
    source = ReadFile(options.sceneFile);
  } else if (!options.synthetic.empty()) {
    sceneName = options.synthetic;
    source = GenerateSyntheticScene(options.synthetic);
  }
  if (!source) {
    std::fprintf(stderr, "no scene: pass --scene-file PATH or --synthetic NAME\n");
    return 2;
  }

  double loadAverage[3] = {-1, -1, -1};
  (void)getloadavg(loadAverage, 3);

  const MemorySample memoryStart = SampleProcessMemory();
  Clock::time_point start = Clock::now();
  std::unique_ptr<BenchBackend> backend = info->create();
  const double setupMs = MillisecondsSince(start);
  if (backend == nullptr) {
    std::fprintf(stderr, "backend %s is unavailable on this host\n", info->name.c_str());
    return 3;
  }
  const MemorySample memoryAfterSetup = SampleProcessMemory();

  ParseWarningSink warnings;
  start = Clock::now();
  ParseResult<svg::SVGDocument> parsed = svg::parser::SVGParser::ParseSVG(*source, warnings);
  const double parseMs = MillisecondsSince(start);
  if (parsed.hasError()) {
    std::fprintf(stderr, "parse failed for %s\n", sceneName.c_str());
    return 4;
  }
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(options.size, options.size);

  bool gpuOk = true;
  backend->resetFeatureUsage();
  // Readback is excluded from every timed region; count any that happen inside one.
  (void)backend->renderer().consumeReadbackStats();
  start = Clock::now();
  backend->renderer().draw(document);
  gpuOk = backend->finishGpuWork() && gpuOk;
  const double firstFrameMs = MillisecondsSince(start);
  const MemorySample memoryAfterFirst = SampleProcessMemory();
  // Feature counts describe one frame: the first, which is also the frame that prepares resources.
  std::optional<FeatureUsage> firstFrameUsage;
  if (const FeatureUsage* usage = backend->featureUsage()) {
    firstFrameUsage = *usage;
  }

  const int firstFrameReadbacks = backend->renderer().consumeReadbackStats().count;
  for (int i = 0; i < options.warmup; ++i) {
    backend->renderer().draw(document);
    gpuOk = backend->finishGpuWork() && gpuOk;
  }

  std::vector<double> frameMs;
  frameMs.reserve(static_cast<std::size_t>(options.frames));
  for (int i = 0; i < options.frames; ++i) {
    start = Clock::now();
    backend->renderer().draw(document);
    gpuOk = backend->finishGpuWork() && gpuOk;
    frameMs.push_back(MillisecondsSince(start));
  }
  const MemorySample memorySteady = SampleProcessMemory();
  // Warmup frames are untimed, but a readback there would also reach the timed frames.
  const int timedReadbacks = firstFrameReadbacks + backend->renderer().consumeReadbackStats().count;
  const std::int64_t gpuBytes = backend->gpuAllocatedBytes();

  // Everything below is outside the timed region: readback, encoding and bookkeeping.
  const svg::RendererBitmap bitmap = backend->renderer().takeSnapshot();
  std::string pngError;
  if (!options.outPng.empty() && !bitmap.empty()) {
    pngError = svg_suite::WritePng(
        options.outPng,
        svg_suite::ImageView{bitmap.dimensions.x, bitmap.dimensions.y, bitmap.rowBytes / 4,
                             std::span<const uint8_t>(bitmap.pixels)});
  }

  JsonWriter json;
  json.beginObject();
  json.string("backend", info->name);
  json.string("suite", SuiteName(info->suite));
  json.string("library", info->library);
  json.string("api", info->api);
  json.integer("threads", info->threads);
  json.string("scene", sceneName);
  json.integer("requested_size", options.size);
  json.integer("width", backend->renderer().width());
  json.integer("height", backend->renderer().height());
  json.integer("warmup_frames", options.warmup);
  json.number("load_average_1m", loadAverage[0]);
  json.number("setup_ms", setupMs);
  json.number("parse_ms", parseMs);
  json.number("first_frame_ms", firstFrameMs);
  WriteStats(json, "steady", ComputeStats(frameMs));
  json.boolean("gpu_completion_confirmed", gpuOk);
  WriteMemory(json, "memory_start", memoryStart);
  WriteMemory(json, "memory_after_setup", memoryAfterSetup);
  WriteMemory(json, "memory_after_first_frame", memoryAfterFirst);
  WriteMemory(json, "memory_steady", memorySteady);
  json.integer("gpu_allocated_bytes", gpuBytes);
  json.boolean("snapshot_empty", bitmap.empty());
  json.integer("timed_readbacks", timedReadbacks);
  if (!pngError.empty()) {
    json.string("png_error", pngError);
  }

  if (firstFrameUsage) {
    const FeatureUsage* usage = &*firstFrameUsage;
    json.boolean("renders_fully", usage->rendersFully());
    json.beginObject("features");
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
      const auto feature = static_cast<Feature>(i);
      if (usage->uses(feature) == 0) {
        continue;
      }
      json.beginObject(FeatureName(feature));
      json.integer("uses", static_cast<std::int64_t>(usage->uses(feature)));
      json.integer("dropped", static_cast<std::int64_t>(usage->dropped(feature)));
      json.endObject();
    }
    json.endObject();
  }
  json.endObject();

  if (options.outJson.empty()) {
    std::printf("%s\n", json.str().c_str());
  } else {
    std::ofstream out(options.outJson, std::ios::app);
    out << json.str() << "\n";
    if (!out) {
      std::fprintf(stderr, "failed to append %s\n", options.outJson.c_str());
      return 5;
    }
  }
  if (!gpuOk) {
    return 6;
  }
  if (timedReadbacks != 0) {
    std::fprintf(stderr, "%s performed %d readbacks inside the timed region\n", info->name.c_str(),
                 timedReadbacks);
    return 7;
  }
  // A backend that produced no pixels (for example a GPU backend without a device) did not
  // render the scene; its timings are recorded but the run fails.
  if (bitmap.empty() && info->suite != Suite::Reference) {
    std::fprintf(stderr, "%s produced an empty frame\n", info->name.c_str());
    return 8;
  }
  return 0;
}

}  // namespace
}  // namespace donner::bakeoff

int main(int argc, char** argv) {
  const std::optional<donner::bakeoff::Options> options = donner::bakeoff::ParseOptions(argc, argv);
  if (!options) {
    return 2;
  }
  if (options->listSynthetic) {
    for (const std::string_view name : donner::bakeoff::SyntheticSceneNames()) {
      std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
    }
    return 0;
  }
  if (options->describe) {
    return donner::bakeoff::Describe();
  }
  return donner::bakeoff::Run(*options);
}
