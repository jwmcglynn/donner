#include "tools/svg_suite/StaticSuite.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

#include "tools/svg_suite/Provenance.h"

namespace donner::svg_suite {
namespace {
constexpr uintmax_t kMaxSvgBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxCases = 100000;

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::string Hex(std::string_view text) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  for (const unsigned char value : text) {
    result += kDigits[value >> 4];
    result += kDigits[value & 15];
  }
  return result;
}

std::string CheckFile(const std::filesystem::path& root, const std::filesystem::path& path,
                      uintmax_t byteLimit) {
  std::error_code error;
  const auto resolved = std::filesystem::canonical(path, error);
  if (error || !IsWithin(root, resolved) || !std::filesystem::is_regular_file(resolved, error) ||
      error) {
    return "missing, nonregular or escaped corpus file: " + path.generic_string();
  }
  const uintmax_t size = std::filesystem::file_size(resolved, error);
  if (error || size == 0 || size > byteLimit) {
    return "empty or oversized corpus file: " + path.generic_string();
  }
  return {};
}

ImageView View(const Image& image) {
  return {image.width, image.height, image.strideInPixels, image.data};
}

std::filesystem::path ArtifactPath(const std::filesystem::path& root, std::string_view id) {
  std::filesystem::path result = root;
  for (size_t position = 0; position < id.size(); position += 120) {
    result /= id.substr(position, 120);
  }
  return result / "case";
}
std::string VerifyCaseSources(const StaticCase& test) {
  if (!test.provenance) {
    return {};
  }
  for (const auto& path : {test.svgPath, test.expectedPath}) {
    const auto* fingerprint = FindFingerprint(*test.provenance, path);
    if (!fingerprint) {
      return "case source is absent from run provenance";
    }
    const auto issue = CheckFile(test.root, path, 32u * 1024u * 1024u);
    if (!issue.empty()) {
      return issue;
    }
    const auto verification = VerifyFile(path, *fingerprint);
    if (!verification.empty()) {
      return "case source changed: " + verification;
    }
  }
  return {};
}

void AddProvenance(nlohmann::json& metrics, const StaticCase& test) {
  if (!test.provenance) {
    metrics["provenance"] = "not-recorded";
    return;
  }
  const auto& run = *test.provenance;
  metrics["run_id"] = run.runId;
  metrics["run_record"] = run.recordPath.generic_string();
  auto process = ArtifactPath(run.workRoot, test.id).parent_path() / "process" / "process.json";
  metrics["process_record"] = process.generic_string();
  for (const auto& [name, path] : {std::pair("input_fingerprint", test.svgPath),
                                   std::pair("reference_fingerprint", test.expectedPath)}) {
    if (const auto* fingerprint = FindFingerprint(run, path)) {
      metrics[name] = {{"sha256", fingerprint->sha256}, {"bytes", fingerprint->bytes}};
    }
  }
}
std::string CheckSymlink(const std::filesystem::directory_entry& entry,
                         const std::filesystem::path& root) {
  std::error_code error;
  if (!entry.is_symlink(error)) {
    return error ? "cannot inspect corpus entry: " + error.message() : "";
  }
  const auto resolved = std::filesystem::canonical(entry.path(), error);
  if (error || !IsWithin(root, resolved)) {
    return "symlink escapes corpus root: " + entry.path().generic_string();
  }
  if (std::filesystem::is_directory(resolved, error)) {
    return "directory symlinks are not supported in corpus: " + entry.path().generic_string();
  }
  return error ? "cannot inspect corpus symlink: " + error.message() : "";
}

std::string AddCase(const CorpusRoot& corpus, const std::filesystem::path& root,
                    const std::filesystem::path& path, std::vector<StaticCase>& cases) {
  std::filesystem::path expected = path;
  expected.replace_extension(".png");
  std::string issue = CheckFile(root, path, kMaxSvgBytes);
  if (issue.empty()) {
    issue = CheckFile(root, expected, 32u * 1024u * 1024u);
  }
  if (!issue.empty()) {
    return issue;
  }
  const std::string relative = path.lexically_relative(root).generic_string();
  cases.push_back({"Case_" + Hex(corpus.name) + "_" + Hex(relative), root, path, expected});
  return cases.size() > kMaxCases ? "corpus exceeds case count limit" : "";
}

std::string EnumerateCorpus(const CorpusRoot& corpus, const std::filesystem::path& root,
                            std::vector<StaticCase>& cases) {
  std::error_code error;
  const auto testsRoot =
      std::filesystem::is_directory(root / "tests", error) ? root / "tests" : root;
  error.clear();
  std::filesystem::recursive_directory_iterator iterator(testsRoot, error);
  const std::filesystem::recursive_directory_iterator end;
  const size_t startCount = cases.size();
  size_t entries = 0;
  while (!error && iterator != end) {
    if (++entries > 500000 || iterator.depth() > 64) {
      return "corpus exceeds directory enumeration limits";
    }
    std::string issue = CheckSymlink(*iterator, root);
    if (issue.empty() && iterator->path().extension() == ".svg") {
      issue = AddCase(corpus, root, iterator->path(), cases);
    }
    if (!issue.empty()) {
      return issue;
    }
    iterator.increment(error);
  }
  if (error) {
    return "cannot enumerate corpus: " + error.message();
  }
  return cases.size() == startCount ? "empty corpus: " + corpus.name : "";
}

std::string CheckCaseInputs(const StaticCase& test) {
  std::string issue = CheckFile(test.root, test.svgPath, kMaxSvgBytes);
  if (issue.empty()) {
    issue = CheckFile(test.root, test.expectedPath, 32u * 1024u * 1024u);
  }
  if (!issue.empty()) {
    return issue;
  }
  if (test.id.empty() ||
      test.id.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
    return "invalid case ID";
  }
  return {};
}

std::string PrepareArtifacts(const StaticCase& test, const std::filesystem::path& artifactRoot,
                             std::filesystem::path& output) {
  std::error_code error;
  const auto outputRoot = std::filesystem::weakly_canonical(artifactRoot, error);
  if (error || outputRoot.empty() || IsWithin(test.root, outputRoot)) {
    return "artifact root must be writable and outside the corpus";
  }
  std::filesystem::create_directories(outputRoot, error);
  if (error) {
    return "cannot create artifact root: " + error.message();
  }
  const auto caseOutput = ArtifactPath(outputRoot, test.id);
  std::filesystem::path componentPath = outputRoot;
  for (const auto& component : caseOutput.lexically_relative(outputRoot)) {
    componentPath /= component;
    if (std::filesystem::is_symlink(componentPath, error)) {
      return "symlink in artifact path";
    }
    error.clear();
  }
  if (!std::filesystem::create_directories(caseOutput, error) || error) {
    return "artifact case directory already exists or cannot be created";
  }
  output = caseOutput;
  return {};
}

struct CaseImages {
  ImageResult expected;
  ImageResult actual;
  ComparisonResult comparison;
};

void AddError(CaseResult& result, const std::string& error) {
  if (!error.empty()) {
    result.passed = false;
    result.error += (result.error.empty() ? "" : "; ") + error;
  }
}

void RenderAndCompare(const StaticCase& test, const Renderer& renderer,
                      const ComparisonOptions& options, CaseImages& images, CaseResult& result) {
  if (!result.error.empty() || !images.expected.image) {
    return;
  }
  if (!renderer) {
    result.error = "renderer callback is missing";
    return;
  }
  images.actual = renderer(test, images.expected.image->width, images.expected.image->height);
  result.error = images.actual.error;
  if (!images.actual.image && result.error.empty()) {
    result.error = "renderer returned neither image nor error";
  }
  if (images.actual.image && result.error.empty()) {
    images.comparison =
        CompareImages(View(*images.expected.image), View(*images.actual.image), options);
    result.error = images.comparison.error;
    result.passed = images.comparison.passed;
    result.mismatchedPixels = images.comparison.mismatchedPixels;
  }
}

void SaveImage(const char* name, const std::optional<Image>& image, CaseResult& result) {
  if (image) {
    const std::string issue = WritePng(result.artifactDirectory / name, View(*image));
    if (!issue.empty()) {
      AddError(result, std::string(name) + ": " + issue);
    }
  }
}

void WriteMetrics(const StaticCase& test, const ComparisonOptions& options,
                  const CaseImages& images, double elapsedMs, CaseResult& result) {
  nlohmann::json metrics = {
      {"test_id", test.id},
      {"input", test.svgPath.lexically_relative(test.root).generic_string()},
      {"status", result.error.empty() ? (result.passed ? "pass" : "fail") : "error"},
      {"diagnostics", result.error},
      {"elapsed_ms", elapsedMs},
      {"threshold", options.threshold},
      {"include_aa", options.includeAntiAliasing},
      {"max_mismatched_pixels", options.maxMismatchedPixels}};
  AddProvenance(metrics, test);
  metrics["mismatched_pixels"] =
      result.mismatchedPixels ? nlohmann::json(*result.mismatchedPixels) : nlohmann::json(nullptr);
  if (images.expected.image) {
    metrics["expected_dimensions"] = {images.expected.image->width, images.expected.image->height};
  }
  if (images.actual.image) {
    metrics["actual_dimensions"] = {images.actual.image->width, images.actual.image->height};
  }
  std::ofstream report(result.artifactDirectory / "metrics.json");
  report << metrics.dump(2) << '\n';
  report.close();
  if (!report) {
    result.passed = false;
    result.error = "cannot write metrics.json";
  }
}
}  // namespace

DiscoveryResult DiscoverCases(const std::vector<CorpusRoot>& roots) {
  DiscoveryResult result;
  std::set<std::string> names;
  if (roots.empty()) {
    return {{}, "no corpus roots selected"};
  }
  for (const CorpusRoot& corpus : roots) {
    if (corpus.name.empty() || !names.insert(corpus.name).second) {
      return {{}, "empty or duplicate corpus name"};
    }
    std::error_code error;
    const auto root = std::filesystem::canonical(corpus.path, error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
      return {{}, "missing corpus root: " + corpus.path.generic_string()};
    }
    const std::string issue = EnumerateCorpus(corpus, root, result.cases);
    if (!issue.empty()) {
      return {{}, issue};
    }
  }
  std::sort(result.cases.begin(), result.cases.end(),
            [](const StaticCase& left, const StaticCase& right) { return left.id < right.id; });
  return result;
}

CaseResult RunCase(const StaticCase& test, const Renderer& renderer,
                   const std::filesystem::path& artifactRoot, const ComparisonOptions& options) {
  CaseResult result;
  result.error = CheckCaseInputs(test);
  if (result.error.empty()) {
    result.error = PrepareArtifacts(test, artifactRoot, result.artifactDirectory);
  }
  if (!result.error.empty()) {
    return result;
  }
  result.error = VerifyCaseSources(test);
  CaseImages images;
  if (result.error.empty()) {
    images.expected = ReadPng(test.expectedPath);
    result.error = images.expected.error;
  }
  const auto start = std::chrono::steady_clock::now();
  RenderAndCompare(test, renderer, options, images, result);
  AddError(result, VerifyCaseSources(test));
  const double elapsedMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  SaveImage("expected.png", images.expected.image, result);
  SaveImage("actual.png", images.actual.image, result);
  SaveImage("diff.png", images.comparison.diff, result);
  WriteMetrics(test, options, images, elapsedMs, result);
  return result;
}
}  // namespace donner::svg_suite
