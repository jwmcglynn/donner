/// @file
/// Turns a bakeoff run directory (backends.json, results.jsonl, png/, optional code_size.json and
/// host.txt) into report.md and summary.json. Pixel differences are computed here, against the
/// tiny-skia snapshot of the same scene and size, through Donner's shared pixelmatch comparison.
///
/// Usage: bakeoff_report <run-dir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "tools/svg_suite/ImageCompare.h"

namespace donner::bakeoff {
namespace {

namespace fs = std::filesystem;
// Ordered so tables follow the feature order the harness emits.
using json = nlohmann::ordered_json;

/// Pixelmatch color threshold for the reported difference count; antialiased pixels are excluded
/// by pixelmatch's own detection.
constexpr double kPixelmatchThreshold = 0.1;

struct Result {
  json record;
  std::string backend;
  std::string scene;
  int size = 0;
  std::optional<int> mismatchedPixels;
  std::int64_t totalPixels = 0;
  std::string compareError;
};

std::string Key(const std::string& scene, int size) {
  return scene + "@" + std::to_string(size);
}

std::optional<json> ReadJson(const fs::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  json value = json::parse(file, nullptr, /*allow_exceptions=*/false);
  if (value.is_discarded()) {
    return std::nullopt;
  }
  return value;
}

std::string Fixed(double value, int digits = 3) {
  if (!std::isfinite(value)) {
    return "n/a";
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
  return buffer;
}

std::string Mebibytes(std::int64_t bytes) {
  if (bytes < 0) {
    return "n/a";
  }
  return Fixed(static_cast<double>(bytes) / (1024.0 * 1024.0), 1);
}

double Number(const json& value, const char* key) {
  const auto it = value.find(key);
  return it != value.end() && it->is_number() ? it->get<double>() : NAN;
}

std::int64_t Integer(const json& value, const char* key) {
  const auto it = value.find(key);
  return it != value.end() && it->is_number_integer() ? it->get<std::int64_t>() : -1;
}

std::int64_t Delta(const json& record, const char* fromKey, const char* toKey, const char* field) {
  const std::int64_t from = Integer(record.value(fromKey, json::object()), field);
  const std::int64_t to = Integer(record.value(toKey, json::object()), field);
  return from < 0 || to < 0 ? -1 : to - from;
}

void ComparePixels(const fs::path& runDir, Result& result) {
  const fs::path actualPath =
      runDir / "png" / result.backend / (result.scene + "_" + std::to_string(result.size) + ".png");
  const fs::path expectedPath =
      runDir / "png" / "tiny_skia" / (result.scene + "_" + std::to_string(result.size) + ".png");
  if (!fs::exists(actualPath) || !fs::exists(expectedPath)) {
    result.compareError = "missing snapshot";
    return;
  }
  const svg_suite::ImageResult actual = svg_suite::ReadPng(actualPath);
  const svg_suite::ImageResult expected = svg_suite::ReadPng(expectedPath);
  if (!actual.image || !expected.image) {
    result.compareError = actual.image ? expected.error : actual.error;
    return;
  }
  const auto view = [](const svg_suite::Image& image) {
    return svg_suite::ImageView{image.width, image.height, image.strideInPixels,
                                std::span<const uint8_t>(image.data)};
  };
  svg_suite::ComparisonOptions options;
  options.threshold = kPixelmatchThreshold;
  options.includeAntiAliasing = false;
  const svg_suite::ComparisonResult comparison =
      svg_suite::CompareImages(view(*expected.image), view(*actual.image), options);
  if (!comparison.mismatchedPixels) {
    result.compareError = comparison.error;
    return;
  }
  result.mismatchedPixels = *comparison.mismatchedPixels;
  result.totalPixels = static_cast<std::int64_t>(actual.image->width) * actual.image->height;
}

struct HostLoad {
  double load1m = 0.0;  //!< 1-minute load average before the run.
  int ciJobs = 0;       //!< Concurrently running CI job processes before the run.
};

struct Report {
  int cpuCores = 0;
  std::map<std::string, HostLoad> hostLoad;  // backend|scene|size -> load before the run
  json backends;
  std::map<std::string, json> backendByName;
  std::vector<Result> results;
  std::map<std::string, std::map<std::string, const Result*>> byKey;  // key -> backend -> result
  std::map<std::string, std::set<std::string>> sceneFeatures;         // key -> features used
};

std::string LoadKey(const std::string& backend, const std::string& scene, int size) {
  return backend + "|" + scene + "|" + std::to_string(size);
}

/// True when the run started on a host busier than half its cores, or beside a CI job.
bool Loaded(const Report& report, const Result& result) {
  const auto it = report.hostLoad.find(LoadKey(result.backend, result.scene, result.size));
  if (it == report.hostLoad.end()) {
    return false;
  }
  return it->second.ciJobs > 0 ||
         (report.cpuCores > 0 && it->second.load1m > 0.5 * report.cpuCores);
}

/// Rejects names that could escape the run directory when used as path components.
bool SafeName(const std::string& name) {
  return !name.empty() && name.find('/') == std::string::npos &&
         name.find("..") == std::string::npos;
}

bool RendersFully(const Report& report, const Result& result) {
  const json& info = report.backendByName.at(result.backend);
  if (info.value("donner_baseline", false) || result.backend == "null") {
    return true;
  }
  return result.record.value("renders_fully", true);
}

bool RunOk(const Result& result) {
  return result.record.value("gpu_completion_confirmed", true) &&
         !result.record.value("snapshot_empty", false) &&
         result.record.value("timed_readbacks", 0) == 0;
}

std::vector<std::string> SuiteBackends(const Report& report, const std::string& suite,
                                       bool multiThreaded) {
  std::vector<std::string> names;
  for (const json& info : report.backends) {
    const bool mt = info.value("threads", 1) != 1;
    if (info.value("suite", "") == suite && mt == multiThreaded) {
      names.push_back(info.value("name", ""));
    }
  }
  return names;
}

std::string DroppedFeatures(const Result& result) {
  std::string out;
  const json features = result.record.value("features", json::object());
  for (const auto& [feature, counts] : features.items()) {
    if (counts.value("dropped", 0) > 0) {
      out += (out.empty() ? "" : ", ") + feature;
    }
  }
  return out;
}

void PerformanceTable(std::ostream& md, const Report& report, const std::vector<std::string>& names,
                      const std::string& baseline, json& summary) {
  if (names.empty()) {
    md << "No backends in this table.\n\n";
    return;
  }
  md << "| scene | size |";
  for (const std::string& name : names) {
    md << " " << name << " first ms | " << name << " median ms (p95) |";
  }
  md << " host |\n|---|---|";
  for (std::size_t i = 0; i < names.size(); ++i) {
    md << "---|---|";
  }
  md << "---|\n";

  std::map<std::string, std::vector<double>> ratios;
  std::size_t comparable = 0;
  std::size_t loadedRows = 0;
  std::vector<std::string> excluded;
  for (const auto& [key, perBackend] : report.byKey) {
    bool allFull = true;
    for (const std::string& name : names) {
      const auto it = perBackend.find(name);
      allFull = allFull && it != perBackend.end() && RunOk(*it->second) &&
                RendersFully(report, *it->second);
    }
    if (!allFull) {
      excluded.push_back(key);
      continue;
    }
    ++comparable;
    const Result& any = *perBackend.at(names.front());
    bool loaded = false;
    md << "| " << any.scene << " | " << any.size << " |";
    for (const std::string& name : names) {
      const Result& result = *perBackend.at(name);
      const json steady = result.record.value("steady", json::object());
      loaded = loaded || Loaded(report, result);
      md << " " << Fixed(Number(result.record, "first_frame_ms")) << " | "
         << Fixed(Number(steady, "median_ms")) << " (" << Fixed(Number(steady, "p95_ms")) << ")";
      if (result.mismatchedPixels) {
        md << " [" << *result.mismatchedPixels << " px diff]";
      }
      md << " |";
    }
    md << " " << (loaded ? "loaded" : "ok") << " |\n";
    if (loaded) {
      ++loadedRows;
      continue;
    }
    if (!baseline.empty() && perBackend.contains(baseline)) {
      const double base =
          Number(perBackend.at(baseline)->record.value("steady", json::object()), "median_ms");
      for (const std::string& name : names) {
        const double value =
            Number(perBackend.at(name)->record.value("steady", json::object()), "median_ms");
        if (base > 0 && value > 0) {
          ratios[name].push_back(value / base);
        }
      }
    }
  }
  md << "\nComparable samples: " << comparable << ", of which " << loadedRows
     << " ran on a loaded host (excluded from the geometric mean; rerun them). Excluded (some "
     << "backend dropped a feature or failed): " << excluded.size() << ".\n\n";
  if (!ratios.empty()) {
    md << "Geometric mean of steady median frame time relative to " << baseline
       << " over comparable samples (lower is faster):\n\n";
    md << "| backend | relative time |\n|---|---|\n";
    for (const std::string& name : names) {
      const auto& values = ratios[name];
      if (values.empty()) {
        continue;
      }
      double logSum = 0;
      for (const double v : values) {
        logSum += std::log(v);
      }
      const double geomean = std::exp(logSum / static_cast<double>(values.size()));
      md << "| " << name << " | " << Fixed(geomean, 3) << "x |\n";
      summary["relative_steady_time"][baseline][name] = geomean;
    }
    md << "\n";
  }
}

int Generate(const fs::path& runDir) {
  Report report;
  const std::optional<json> backends = ReadJson(runDir / "backends.json");
  if (!backends || !backends->is_array()) {
    std::fprintf(stderr, "cannot read %s\n", (runDir / "backends.json").c_str());
    return 2;
  }
  report.backends = *backends;
  for (const json& info : report.backends) {
    report.backendByName[info.value("name", "")] = info;
  }

  if (std::ifstream host(runDir / "host.txt"); host) {
    std::string hostLine;
    while (std::getline(host, hostLine)) {
      if (hostLine.rfind("cpu_cores: ", 0) == 0) {
        report.cpuCores = std::atoi(hostLine.c_str() + 11);
      }
    }
  }
  if (std::ifstream loads(runDir / "host_load.tsv"); loads) {
    std::string loadLine;
    std::getline(loads, loadLine);  // Header.
    while (std::getline(loads, loadLine)) {
      std::istringstream fields(loadLine);
      std::string time, backend, scene, size, load1m, load5m, ciJobs;
      if (std::getline(fields, time, '\t') && std::getline(fields, backend, '\t') &&
          std::getline(fields, scene, '\t') && std::getline(fields, size, '\t') &&
          std::getline(fields, load1m, '\t') && std::getline(fields, load5m, '\t') &&
          std::getline(fields, ciJobs, '\t')) {
        report.hostLoad[LoadKey(backend, scene, std::atoi(size.c_str()))] =
            HostLoad{std::atof(load1m.c_str()), std::atoi(ciJobs.c_str())};
      }
    }
  }

  std::ifstream lines(runDir / "results.jsonl");
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) {
      continue;
    }
    json record = json::parse(line, nullptr, false);
    if (record.is_discarded() || !report.backendByName.contains(record.value("backend", ""))) {
      continue;
    }
    Result result;
    result.backend = record.value("backend", "");
    result.scene = record.value("scene", "");
    if (!SafeName(result.backend) || !SafeName(result.scene)) {
      continue;
    }
    result.size = record.value("requested_size", 0);
    result.record = std::move(record);
    report.results.push_back(std::move(result));
  }

  for (Result& result : report.results) {
    if (result.backend != "tiny_skia" && result.backend != "null") {
      ComparePixels(runDir, result);
    }
    if (result.backend == "null") {
      const json features = result.record.value("features", json::object());
      for (const auto& [feature, counts] : features.items()) {
        report.sceneFeatures[Key(result.scene, result.size)].insert(feature);
      }
    }
  }
  for (const Result& result : report.results) {
    report.byKey[Key(result.scene, result.size)][result.backend] = &result;
  }

  json summary;
  std::ostringstream md;
  md << "# Renderer bakeoff results\n\n";
  const fs::path runName =
      runDir.filename().empty() ? runDir.parent_path().filename() : runDir.filename();
  md << "Generated by `bakeoff_report` from `" << runName.string() << "`.\n\n";
  if (std::ifstream host(runDir / "host.txt"); host) {
    md << "## Host\n\n```\n" << host.rdbuf() << "```\n\n";
  }

  md << "## Backends\n\n| backend | suite | library | API | threads | configuration |\n"
        "|---|---|---|---|---|---|\n";
  for (const json& info : report.backends) {
    const int threads = info.value("threads", 1);
    md << "| " << info.value("name", "") << " | " << info.value("suite", "") << " | "
       << info.value("library", "") << " | " << info.value("api", "") << " | "
       << (threads == 0 ? std::string("library default") : std::to_string(threads)) << " | "
       << info.value("configuration", "") << " |\n";
  }

  md << "\n## Feature support\n\nN = native, E = emulated by the adapter, - = unsupported (draws "
        "using it are dropped and the sample is excluded from speed comparison).\n\n| feature |";
  for (const json& info : report.backends) {
    md << " " << info.value("name", "") << " |";
  }
  md << "\n|---|";
  for (std::size_t i = 0; i < report.backends.size(); ++i) {
    md << "---|";
  }
  md << "\n";
  if (!report.backends.empty()) {
    const json capabilities = report.backends.front().value("capabilities", json::object());
    for (const auto& [feature, unused] : capabilities.items()) {
      md << "| " << feature << " |";
      for (const json& info : report.backends) {
        const std::string support = info.value("capabilities", json::object())
                                        .value(feature, json::object())
                                        .value("support", "unsupported");
        md << " " << (support == "native" ? "N" : support == "emulated" ? "E" : "-") << " |";
      }
      md << "\n";
    }
  }

  md << "\n## GPU suite: performance\n\n";
  PerformanceTable(md, report, SuiteBackends(report, "gpu", false), "geode", summary);
  md << "## Software suite: performance (single-threaded)\n\n";
  PerformanceTable(md, report, SuiteBackends(report, "software", false), "tiny_skia", summary);
  if (const auto mt = SuiteBackends(report, "software", true); !mt.empty()) {
    md << "## Software suite: library multithreading\n\nShown separately; tiny-skia-cpp has no "
          "renderer threading, so these are not compared against it.\n\n";
    PerformanceTable(md, report, mt, "", summary);
  }

  md << "## Donner traversal cost (null recorder)\n\n| scene | size | first ms | median ms |\n"
        "|---|---|---|---|\n";
  for (const auto& [key, perBackend] : report.byKey) {
    if (const auto it = perBackend.find("null"); it != perBackend.end()) {
      const json& r = it->second->record;
      md << "| " << it->second->scene << " | " << it->second->size << " | "
         << Fixed(Number(r, "first_frame_ms")) << " | "
         << Fixed(Number(r.value("steady", json::object()), "median_ms")) << " |\n";
    }
  }

  md << "\n## Memory\n\nMedians across all samples per backend. Footprint is the macOS physical "
        "footprint (resident set on Linux); deltas are measured from process start, so they "
        "include backend setup.\n\n| backend | setup delta MiB | steady delta MiB | peak delta MiB "
        "| graphics delta MiB |\n|---|---|---|---|---|\n";
  for (const json& info : report.backends) {
    const std::string name = info.value("name", "");
    std::vector<std::int64_t> setup, steady, peak, graphics;
    for (const Result& result : report.results) {
      if (result.backend != name) {
        continue;
      }
      const char* field =
          Integer(result.record.value("memory_start", json::object()), "footprint_bytes") >= 0
              ? "footprint_bytes"
              : "resident_bytes";
      setup.push_back(Delta(result.record, "memory_start", "memory_after_setup", field));
      steady.push_back(Delta(result.record, "memory_start", "memory_steady", field));
      peak.push_back(Delta(result.record, "memory_start", "memory_steady", "peak_bytes"));
      graphics.push_back(Delta(result.record, "memory_start", "memory_steady", "graphics_bytes"));
    }
    const auto median = [](std::vector<std::int64_t> values) -> std::int64_t {
      values.erase(std::remove(values.begin(), values.end(), -1), values.end());
      if (values.empty()) {
        return -1;
      }
      std::sort(values.begin(), values.end());
      return values[values.size() / 2];
    };
    md << "| " << name << " | " << Mebibytes(median(setup)) << " | " << Mebibytes(median(steady))
       << " | " << Mebibytes(median(peak)) << " | " << Mebibytes(median(graphics)) << " |\n";
  }

  md << "\n## Per-sample status\n\nEach cell: `full` or `partial: <dropped features>`, then the "
        "pixelmatch difference against tiny-skia (threshold "
     << kPixelmatchThreshold
     << ", antialiased pixels excluded) as pixels and percent of the frame.\n\n| scene | size | "
        "features used |";
  std::vector<std::string> compared;
  for (const json& info : report.backends) {
    const std::string name = info.value("name", "");
    if (name != "null" && name != "tiny_skia") {
      compared.push_back(name);
      md << " " << name << " |";
    }
  }
  md << "\n|---|---|---|";
  for (std::size_t i = 0; i < compared.size(); ++i) {
    md << "---|";
  }
  md << "\n";
  for (const auto& [key, perBackend] : report.byKey) {
    const Result& any = *perBackend.begin()->second;
    std::string features;
    for (const std::string& feature : report.sceneFeatures[key]) {
      features += (features.empty() ? "" : ", ") + feature;
    }
    md << "| " << any.scene << " | " << any.size << " | " << features << " |";
    for (const std::string& name : compared) {
      const auto it = perBackend.find(name);
      if (it == perBackend.end()) {
        md << " not run |";
        continue;
      }
      const Result& result = *it->second;
      std::string cell = !RunOk(result)                 ? "failed"
                         : RendersFully(report, result) ? "full"
                                                        : "partial: " + DroppedFeatures(result);
      if (result.mismatchedPixels) {
        cell +=
            "; " + std::to_string(*result.mismatchedPixels) + " px (" +
            Fixed(100.0 * *result.mismatchedPixels / std::max(std::int64_t{1}, result.totalPixels),
                  2) +
            "%)";
      } else if (!result.compareError.empty()) {
        cell += "; " + result.compareError;
      }
      md << " " << cell << " |";
      summary["samples"][key][name] = {
          {"renders_fully", RendersFully(report, result)},
          {"mismatched_pixels", result.mismatchedPixels ? json(*result.mismatchedPixels) : json()},
          {"steady_median_ms", Number(result.record.value("steady", json::object()), "median_ms")},
          {"first_frame_ms", Number(result.record, "first_frame_ms")}};
    }
    md << "\n";
  }

  if (const std::optional<json> codeSize = ReadJson(runDir / "code_size.json")) {
    md << "\n## Code size\n\n"
       << codeSize->value("notes", std::string()) << "\n\n"
       << "| backend | stripped executable bytes | delta vs baseline | static archive bytes | "
          "dependency closure |\n|---|---|---|---|---|\n";
    for (const json& entry : codeSize->value("entries", json::array())) {
      md << "| " << entry.value("backend", "") << " | " << Integer(entry, "stripped_bytes") << " | "
         << Integer(entry, "delta_bytes") << " | " << Integer(entry, "archive_bytes") << " | "
         << entry.value("closure", "") << " |\n";
    }
    summary["code_size"] = *codeSize;
  }

  md << "\n## Host load during the run\n\n`host_load.tsv` records the 1- and 5-minute load and the "
        "count of concurrently running CI jobs before every measurement. A performance row is "
        "marked `loaded` when any of its runs started with a 1-minute load above half the "
        "host's cores or beside a CI job.\n";

  std::ofstream(runDir / "report.md") << md.str();
  std::ofstream(runDir / "summary.json") << summary.dump(2) << "\n";
  std::printf("wrote %s\n", (runDir / "report.md").c_str());
  return 0;
}

}  // namespace
}  // namespace donner::bakeoff

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: bakeoff_report <run-dir>\n");
    return 2;
  }
  return donner::bakeoff::Generate(argv[1]);
}
