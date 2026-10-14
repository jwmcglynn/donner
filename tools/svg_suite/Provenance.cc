#include "tools/svg_suite/Provenance.h"

#include <fcntl.h>
#include <picosha2.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace donner::svg_suite {
namespace {
using Json = nlohmann::json;

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

Json FingerprintJson(const FileFingerprint& value) {
  return {{"bytes", value.bytes}, {"sha256", value.sha256}};
}

struct Budget {
  ProvenanceLimits limits;
  size_t files = 0;
  uintmax_t bytes = 0;
};

std::string CaptureFile(const std::filesystem::path& path, Budget& budget,
                        std::map<std::filesystem::path, FileFingerprint>& files) {
  if (budget.files >= budget.limits.maxFiles || budget.bytes > budget.limits.maxTotalBytes) {
    return "provenance file count or byte budget exceeded";
  }
  const uintmax_t available = budget.limits.maxTotalBytes - budget.bytes;
  const auto result = FingerprintFile(path, std::min(available, budget.limits.maxFileBytes));
  if (!result.fingerprint) {
    return result.error;
  }
  ++budget.files;
  budget.bytes += result.fingerprint->bytes;
  files.emplace(path.lexically_normal(), *result.fingerprint);
  return {};
}

std::string CaptureCorpus(const ProvenanceCorpus& corpus, Budget& budget,
                          std::map<std::filesystem::path, FileFingerprint>& files) {
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(corpus.root, error);
  const std::filesystem::recursive_directory_iterator end;
  size_t entries = 0;
  while (!error && iterator != end) {
    if (++entries > 500000 || iterator.depth() > 64) {
      return "provenance directory enumeration limit exceeded";
    }
    const auto path = iterator->path();
    const auto resolved = std::filesystem::canonical(path, error);
    if (error || !IsWithin(corpus.root, resolved)) {
      return "missing or escaped provenance source: " + path.generic_string();
    }
    if (iterator->is_directory(error)) {
      if (iterator->is_symlink(error)) {
        return "directory symlink in provenance corpus";
      }
    } else if (iterator->is_regular_file(error)) {
      const auto issue = CaptureFile(path, budget, files);
      if (!issue.empty()) {
        return issue;
      }
    } else {
      return "nonregular provenance source: " + path.generic_string();
    }
    if (!error) {
      iterator.increment(error);
    }
  }
  return error ? "cannot enumerate provenance corpus: " + error.message() : "";
}

std::string ValidateOptions(const ProvenanceOptions& options) {
  if (options.corpora.empty() || options.limits.maxFiles == 0 || options.limits.maxFileBytes == 0 ||
      options.limits.maxTotalBytes == 0) {
    return "provenance requires corpora and positive limits";
  }
  if (options.invocation.size() > 10000 || options.rendererArguments.size() > 10000 ||
      options.engineRevision.size() > 4096 || options.backend.size() > 4096 ||
      options.buildFlags.size() > 65536) {
    return "provenance command metadata exceeds limits";
  }
  size_t bytes = 0;
  for (const auto* arguments : {&options.invocation, &options.rendererArguments}) {
    for (const auto& argument : *arguments) {
      if (argument.size() > 65536 || bytes > 1024 * 1024 - argument.size()) {
        return "provenance argument bytes exceed limits";
      }
      bytes += argument.size();
    }
  }
  return {};
}

std::string CaptureSources(const ProvenanceOptions& options, RunProvenance& run,
                           const std::filesystem::path& outputRoot) {
  std::error_code error;
  run.renderer = std::filesystem::canonical(options.renderer, error);
  if (error) {
    return "cannot resolve renderer for provenance";
  }
  Budget budget{options.limits};
  std::map<std::filesystem::path, FileFingerprint> engine;
  auto issue = CaptureFile(run.renderer, budget, engine);
  if (!issue.empty()) {
    return issue;
  }
  run.rendererFingerprint = engine.begin()->second;
  std::set<std::string> names;
  for (auto corpus : options.corpora) {
    if (corpus.name.empty() || corpus.name.size() > 4096 || corpus.revision.size() > 4096 ||
        corpus.revisionEvidence.size() > 4096 || !names.insert(corpus.name).second) {
      return "empty, oversized or duplicate provenance corpus name";
    }
    corpus.root = std::filesystem::canonical(corpus.root, error);
    if (error || !std::filesystem::is_directory(corpus.root, error) || error ||
        IsWithin(corpus.root, outputRoot) || IsWithin(corpus.root, run.workRoot)) {
      return "provenance output must be outside existing corpus directories";
    }
    issue = CaptureCorpus(corpus, budget, run.files);
    if (!issue.empty()) {
      return issue;
    }
    run.corpora.push_back(std::move(corpus));
  }
  return {};
}

Json BuildRecord(const ProvenanceOptions& options, const RunProvenance& run) {
  Json corpora = Json::array();
  for (const auto& corpus : run.corpora) {
    Json files = Json::object();
    for (const auto& [path, fingerprint] : run.files) {
      if (IsWithin(corpus.root, path)) {
        files[path.lexically_relative(corpus.root).generic_string()] = FingerprintJson(fingerprint);
      }
    }
    corpora.push_back({{"name", corpus.name},
                       {"root", corpus.root.generic_string()},
                       {"revision", corpus.revision},
                       {"revision_evidence", corpus.revisionEvidence},
                       {"content_sha256", picosha2::hash256_hex_string(files.dump())},
                       {"files", std::move(files)}});
  }
  return {{"format", "donner-svg-suite-run-1"},
          {"renderer",
           {{"path", run.renderer.generic_string()},
            {"fingerprint", FingerprintJson(run.rendererFingerprint)},
            {"declared",
             {{"revision", options.engineRevision},
              {"backend", options.backend},
              {"build_flags", options.buildFlags},
              {"verification", "caller-declared, unverified"}}}}},
          {"corpora", std::move(corpora)},
          {"invocation_argv", options.invocation},
          {"invocation_directory", std::filesystem::current_path().generic_string()},
          {"renderer_arguments_template", options.rendererArguments},
          {"work_directory", run.workRoot.generic_string()},
          {"timeout_ms", options.timeoutMs},
          {"trusted_inputs", options.trustedInputs},
          {"comparison",
           {{"threshold", options.comparison.threshold},
            {"max_mismatched_pixels", options.comparison.maxMismatchedPixels},
            {"include_aa", options.comparison.includeAntiAliasing}}},
          {"verification_scope",
           "input/reference before and after each case; all sources at run end; "
           "point-in-time hashes, not an adversarial mutation sandbox"}};
}

std::string WriteExclusive(const std::filesystem::path& path, std::string_view data) {
  const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
  if (descriptor < 0) {
    return path.filename().string() + " already exists or cannot be created";
  }
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = write(descriptor, data.data() + offset, data.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      close(descriptor);
      return "cannot write complete run.json";
    }
    offset += static_cast<size_t>(written);
  }
  const bool synced = fsync(descriptor) == 0;
  const bool closed = close(descriptor) == 0;
  return synced && closed ? "" : "cannot finalize run.json";
}
}  // namespace

FingerprintResult FingerprintFile(const std::filesystem::path& path, uintmax_t maxBytes) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return {{}, "missing or nonregular fingerprint input: " + path.generic_string()};
  }
  const auto beforeSize = std::filesystem::file_size(path, error);
  if (error || beforeSize > maxBytes) {
    return {{}, "fingerprint input exceeds byte budget: " + path.generic_string()};
  }
  const auto beforeTime = std::filesystem::last_write_time(path, error);
  if (error) {
    return {{}, "cannot stat fingerprint input"};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {{}, "cannot open fingerprint input: " + path.generic_string()};
  }
  picosha2::hash256_one_by_one hasher;
  std::array<char, 65536> buffer;
  uintmax_t bytes = 0;
  while (input) {
    input.read(buffer.data(), buffer.size());
    const auto count = static_cast<size_t>(input.gcount());
    if (count > maxBytes - bytes) {
      return {{}, "fingerprint input grew beyond byte budget"};
    }
    hasher.process(buffer.begin(), buffer.begin() + count);
    bytes += count;
  }
  if (!input.eof()) {
    return {{}, "cannot read fingerprint input"};
  }
  const auto afterSize = std::filesystem::file_size(path, error);
  if (error || beforeSize != bytes || afterSize != bytes) {
    return {{}, "fingerprint input changed during hashing"};
  }
  const auto afterTime = std::filesystem::last_write_time(path, error);
  if (error || beforeTime != afterTime) {
    return {{}, "fingerprint input changed during hashing"};
  }
  hasher.finish();
  return {FileFingerprint{bytes, picosha2::get_hash_hex_string(hasher)}, {}};
}

std::string VerifyFile(const std::filesystem::path& path, const FileFingerprint& expected) {
  const auto actual = FingerprintFile(path, expected.bytes);
  if (!actual.fingerprint) {
    return actual.error;
  }
  return *actual.fingerprint == expected
             ? ""
             : "source changed since snapshot: " + path.generic_string();
}

ProvenanceResult CreateRunProvenance(const ProvenanceOptions& options,
                                     const std::filesystem::path& artifactRoot) {
  auto issue = ValidateOptions(options);
  if (!issue.empty()) {
    return {{}, issue};
  }
  std::error_code error;
  const auto outputRoot = std::filesystem::weakly_canonical(artifactRoot, error);
  if (error || outputRoot.empty()) {
    return {{}, "cannot resolve provenance output directory"};
  }
  auto run = std::make_shared<RunProvenance>();
  run->limits = options.limits;
  run->workRoot = std::filesystem::weakly_canonical(options.workRoot, error);
  if (error) {
    return {{}, "cannot resolve process work directory"};
  }
  issue = CaptureSources(options, *run, outputRoot);
  if (!issue.empty()) {
    return {{}, issue};
  }
  Json record = BuildRecord(options, *run);
  run->runId = picosha2::hash256_hex_string(record.dump());
  record["run_id"] = run->runId;
  std::filesystem::create_directories(outputRoot, error);
  if (error) {
    return {{}, "cannot create provenance output directory"};
  }
  run->recordPath = outputRoot / "run.json";
  issue = WriteExclusive(run->recordPath, record.dump(2) + "\n");
  return issue.empty() ? ProvenanceResult{std::move(run), {}} : ProvenanceResult{{}, issue};
}

std::string VerifyRunProvenance(const RunProvenance& run) {
  auto issue = VerifyFile(run.renderer, run.rendererFingerprint);
  if (!issue.empty()) {
    return "renderer provenance verification failed: " + issue;
  }
  Budget budget{run.limits, 1, run.rendererFingerprint.bytes};
  std::map<std::filesystem::path, FileFingerprint> current;
  for (const auto& corpus : run.corpora) {
    issue = CaptureCorpus(corpus, budget, current);
    if (!issue.empty()) {
      return issue;
    }
  }
  return current == run.files ? "" : "corpus membership or bytes changed during run";
}

namespace {
Json ExecutionRecord(const RunExecutionSummary& execution) {
  return {{"discovered_cases", execution.discoveredCases},
          {"selected_cases", execution.selectedCases},
          {"completed_cases", execution.completedCases},
          {"completed_invocations", execution.completedInvocations},
          {"skipped_invocations", execution.skippedInvocations},
          {"requested_repeat_count", execution.repeatCount},
          {"list_only", execution.listOnly},
          {"gtest_output", execution.gtestOutput},
          {"corpus_complete", execution.corpusComplete()}};
}

Json XmlReportRecord(const RunExecutionSummary& execution) {
  if (execution.completedInvocations == 0 || execution.xmlReport.empty()) {
    return nullptr;
  }
  const auto report = FingerprintFile(execution.xmlReport, 32u * 1024u * 1024u);
  if (!report.fingerprint) {
    return nullptr;
  }
  return {{"path", execution.xmlReport.generic_string()},
          {"sha256", report.fingerprint->sha256},
          {"bytes", report.fingerprint->bytes},
          {"scope", "GoogleTest final iteration; execution counts include all repeats"}};
}
}  // namespace

std::string WriteRunVerification(const RunProvenance& run, std::string_view verificationError,
                                 int testExitCode, const RunExecutionSummary& execution) {
  const bool accepted =
      verificationError.empty() && testExitCode == 0 && execution.selectionComplete();
  const Json record = {
      {"run_id", run.runId},
      {"run_record", "run.json"},
      {"sources_unchanged_at_verification", verificationError.empty()},
      {"verification_error", verificationError},
      {"test_exit_code", testExitCode},
      {"accepted", accepted},
      {"accepted_scope", execution.corpusComplete() ? "complete-corpus" : "selected-cases"},
      {"complete_corpus_accepted", accepted && execution.corpusComplete()},
      {"execution", ExecutionRecord(execution)},
      {"xml_report", XmlReportRecord(execution)}};
  return WriteExclusive(run.recordPath.parent_path() / "run-verification.json",
                        record.dump(2) + "\n");
}

const FileFingerprint* FindFingerprint(const RunProvenance& run,
                                       const std::filesystem::path& path) {
  const auto found = run.files.find(path.lexically_normal());
  return found == run.files.end() ? nullptr : &found->second;
}
}  // namespace donner::svg_suite
