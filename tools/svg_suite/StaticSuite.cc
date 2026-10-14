#include "tools/svg_suite/StaticSuite.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

#include "tools/svg_suite/Provenance.h"
#include "tools/svg_suite/TextEncoding.h"

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
  if (!error && !Utf8Path(resolved)) {
    return "resolved corpus path requires bounded NUL-free UTF-8";
  }
  if (error || !IsWithin(root, resolved) || !std::filesystem::is_regular_file(resolved, error) ||
      error) {
    return "missing, nonregular or escaped corpus file: " + *Utf8Path(path);
  }
  const uintmax_t size = std::filesystem::file_size(resolved, error);
  if (error || size == 0 || size > byteLimit) {
    return "empty or oversized corpus file: " + *Utf8Path(path);
  }
  return {};
}

ImageView View(const Image& image) {
  return {image.width, image.height, image.strideInPixels, image.data};
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
  metrics["run_record"] = *Utf8Path(run.recordPath);
  auto process = CaseOutputDirectory(run.workRoot, test) / "process" / "process.json";
  metrics["process_record"] = *Utf8Path(process);
  for (const auto& [name, path] : {std::pair("input_fingerprint", test.svgPath),
                                   std::pair("reference_fingerprint", test.expectedPath)}) {
    if (const auto* fingerprint = FindFingerprint(run, path)) {
      metrics[name] = {{"sha256", fingerprint->sha256}, {"bytes", fingerprint->bytes}};
    }
  }
}
std::string CheckSymlink(const std::filesystem::directory_entry& entry,
                         const std::filesystem::path& root) {
  if (!Utf8Path(entry.path())) {
    return "corpus entry requires bounded NUL-free UTF-8";
  }
  std::error_code error;
  if (!entry.is_symlink(error)) {
    return error ? "cannot inspect corpus entry: " + error.message() : "";
  }
  const auto resolved = std::filesystem::canonical(entry.path(), error);
  if (!error && !Utf8Path(resolved)) {
    return "resolved corpus entry requires bounded NUL-free UTF-8";
  }
  if (error || !IsWithin(root, resolved)) {
    return "symlink escapes corpus root: " + *Utf8Path(entry.path());
  }
  if (std::filesystem::is_directory(resolved, error)) {
    return "directory symlinks are not supported in corpus: " + *Utf8Path(entry.path());
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
  const std::string relative = *Utf8Path(path.lexically_relative(root));
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

std::string CheckCaseProvenance(const StaticCase& test) {
  if (!test.provenance) {
    return {};
  }
  const auto& run = *test.provenance;
  if (!IsValidIdentityText(run.runId, 4096) || !Utf8Path(run.recordPath) ||
      !Utf8Path(CaseOutputDirectory(run.workRoot, test) / "process/process.json")) {
    return "case provenance requires bounded NUL-free UTF-8";
  }
  for (const auto* path : {&test.svgPath, &test.expectedPath}) {
    const auto* fingerprint = FindFingerprint(run, *path);
    if (fingerprint && !IsValidIdentityText(fingerprint->sha256, 4096)) {
      return "case fingerprint requires bounded NUL-free UTF-8";
    }
  }
  return {};
}

std::string CheckCaseText(const StaticCase& test) {
  if (!IsValidIdentityText(test.id) || !Utf8Path(test.svgPath.lexically_relative(test.root))) {
    return "case identity requires bounded NUL-free UTF-8";
  }
  for (const auto* path : {&test.root, &test.svgPath, &test.expectedPath}) {
    if (!Utf8Path(*path)) {
      return "case paths require bounded NUL-free UTF-8";
    }
  }
  return CheckCaseProvenance(test);
}

std::string CheckCaseInputs(const StaticCase& test) {
  const auto encoding = CheckCaseText(test);
  if (!encoding.empty()) {
    return encoding;
  }
  if (test.iteration && *test.iteration == 0) {
    return "case iteration must be positive";
  }
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

std::string PrepareCaseDirectory(const StaticCase& test, const std::filesystem::path& outputRoot,
                                 std::filesystem::path& output) {
  std::error_code error;
  const auto caseOutput = CaseOutputDirectory(outputRoot, test) / "case";
  if (!Utf8Path(caseOutput)) {
    return "case artifact path requires bounded NUL-free UTF-8";
  }
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

std::string PrepareArtifacts(const StaticCase& test, const std::filesystem::path& artifactRoot,
                             std::filesystem::path& output) {
  if (!Utf8Path(artifactRoot)) {
    return "artifact path requires bounded NUL-free UTF-8";
  }
  std::error_code error;
  const auto outputRoot = std::filesystem::weakly_canonical(artifactRoot, error);
  if (!error && !Utf8Path(outputRoot)) {
    return "resolved artifact path requires bounded NUL-free UTF-8";
  }
  if (error || outputRoot.empty() || IsWithin(test.root, outputRoot)) {
    return "artifact root must be writable and outside the corpus";
  }
  std::filesystem::create_directories(outputRoot, error);
  if (error) {
    return "cannot create artifact root: " + error.message();
  }
  return PrepareCaseDirectory(test, outputRoot, output);
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
  const auto identityError = CheckCaseText(test);
  if (!identityError.empty() || !Utf8Path(result.artifactDirectory)) {
    AddError(result, "metrics identity requires bounded NUL-free UTF-8");
    return;
  }
  nlohmann::json metrics = {
      {"test_id", test.id},
      {"input", *Utf8Path(test.svgPath.lexically_relative(test.root))},
      {"status", result.error.empty() ? (result.passed ? "pass" : "fail") : "error"},
      {"diagnostics", DiagnosticUtf8(result.error)},
      {"diagnostics_truncated", result.error.size() > kMaxDiagnosticBytes},
      {"diagnostics_encoding", "utf8-with-replacement"},
      {"elapsed_ms", elapsedMs},
      {"threshold", options.threshold},
      {"include_aa", options.includeAntiAliasing},
      {"max_mismatched_pixels", options.maxMismatchedPixels}};
  metrics["iteration"] = test.iteration ? nlohmann::json(*test.iteration) : nlohmann::json(nullptr);
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
bool HasValidCorpusIdentity(const CorpusRoot& corpus) {
  return IsValidIdentityText(corpus.name, 4096) && Utf8Path(corpus.path).has_value();
}
}  // namespace

std::filesystem::path CaseOutputDirectory(const std::filesystem::path& root,
                                          const StaticCase& test) {
  std::filesystem::path result = root;
  for (size_t position = 0; position < test.id.size(); position += 120) {
    result /= test.id.substr(position, 120);
  }
  if (test.iteration) {
    result /= "iteration-" + std::to_string(*test.iteration);
  }
  return result;
}

DiscoveryResult DiscoverCases(const std::vector<CorpusRoot>& roots) {
  DiscoveryResult result;
  std::set<std::string> names;
  if (roots.empty() || roots.size() > kMaxArguments) {
    return {{}, "no corpus roots selected"};
  }
  for (const CorpusRoot& corpus : roots) {
    if (!HasValidCorpusIdentity(corpus)) {
      return {{}, "corpus identity requires bounded NUL-free UTF-8"};
    }
    if (corpus.name.empty() || !names.insert(corpus.name).second) {
      return {{}, "empty or duplicate corpus name"};
    }
    std::error_code error;
    const auto root = std::filesystem::canonical(corpus.path, error);
    if (error || !Utf8Path(root)) {
      return {{}, "cannot resolve bounded UTF-8 corpus root"};
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
