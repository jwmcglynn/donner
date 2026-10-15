#include "tools/svg_suite/Provenance.h"

#include <fcntl.h>
#include <picosha2.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

#include "tools/svg_suite/TextEncoding.h"

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
  if (!Utf8Path(path)) {
    return "provenance path requires bounded NUL-free UTF-8";
  }
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

std::string CheckProvenanceDirectory(const std::filesystem::directory_entry& entry) {
  std::error_code error;
  const bool symlink = entry.is_symlink(error);
  return symlink || error ? "directory symlink or unreadable provenance directory" : "";
}

std::string CaptureEntry(const std::filesystem::directory_entry& entry,
                         const ProvenanceCorpus& corpus, Budget& budget,
                         std::map<std::filesystem::path, FileFingerprint>& files) {
  const auto path = entry.path();
  if (!Utf8Path(path)) {
    return "provenance entry requires bounded NUL-free UTF-8";
  }
  std::error_code error;
  const auto resolved = std::filesystem::canonical(path, error);
  if (!error && !Utf8Path(resolved)) {
    return "resolved provenance entry requires bounded NUL-free UTF-8";
  }
  if (error || !IsWithin(corpus.root, resolved)) {
    return "missing or escaped provenance source: " + path.generic_string();
  }
  if (entry.is_directory(error)) {
    return CheckProvenanceDirectory(entry);
  }
  if (error) {
    return "cannot inspect provenance entry";
  }
  if (entry.is_regular_file(error)) {
    return CaptureFile(path, budget, files);
  }
  return "nonregular or unreadable provenance source: " + path.generic_string();
}

std::string CaptureCorpus(const ProvenanceCorpus& corpus, Budget& budget,
                          std::map<std::filesystem::path, FileFingerprint>& files) {
  if (!Utf8Path(corpus.root)) {
    return "provenance root requires bounded NUL-free UTF-8";
  }
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(corpus.root, error);
  const std::filesystem::recursive_directory_iterator end;
  size_t entries = 0;
  while (!error && iterator != end) {
    if (++entries > 500000 || iterator.depth() > 64) {
      return "provenance directory enumeration limit exceeded";
    }
    const auto issue = CaptureEntry(*iterator, corpus, budget, files);
    if (!issue.empty()) {
      return issue;
    }
    iterator.increment(error);
  }
  return error ? "cannot enumerate provenance corpus: " + error.message() : "";
}

bool ValidCommandMetadata(const ProvenanceOptions& options) {
  return IsValidIdentityText(options.engineRevision, 4096) &&
         IsValidIdentityText(options.backend, 4096) && IsValidIdentityText(options.buildFlags) &&
         Utf8Path(options.renderer).has_value() && Utf8Path(options.workRoot).has_value();
}

bool ValidInvocation(const ProvenanceOptions& options) {
  const auto invocationBytes = ArgumentBytes(options.invocation);
  const auto rendererBytes = ArgumentBytes(options.rendererArguments);
  return invocationBytes && rendererBytes && *rendererBytes <= kMaxArgumentBytes - *invocationBytes;
}

std::string ValidateCorpusIdentities(const std::vector<ProvenanceCorpus>& corpora) {
  if (corpora.size() > kMaxArguments) {
    return "provenance corpus count exceeds limits";
  }
  std::set<std::string> names;
  for (const auto& corpus : corpora) {
    if (!IsValidIdentityText(corpus.name, 4096) || !IsValidIdentityText(corpus.revision, 4096) ||
        !IsValidIdentityText(corpus.revisionEvidence, 4096) || !Utf8Path(corpus.root)) {
      return "provenance corpus identity requires bounded NUL-free UTF-8";
    }
    if (corpus.name.empty() || !names.insert(corpus.name).second) {
      return "empty or duplicate provenance corpus name";
    }
  }
  return {};
}

std::string ValidateOptions(const ProvenanceOptions& options) {
  if (options.corpora.empty() || options.limits.maxFiles == 0 || options.limits.maxFileBytes == 0 ||
      options.limits.maxTotalBytes == 0) {
    return "provenance requires corpora and positive limits";
  }
  if (!ValidCommandMetadata(options)) {
    return "provenance metadata requires bounded NUL-free UTF-8";
  }
  if (!ValidInvocation(options)) {
    return "provenance arguments require bounded NUL-free UTF-8";
  }
  return ValidateCorpusIdentities(options.corpora);
}

std::string ResolveCorpus(ProvenanceCorpus& corpus, const RunProvenance& run,
                          const std::filesystem::path& outputRoot) {
  std::error_code error;
  corpus.root = std::filesystem::canonical(corpus.root, error);
  if (error || !Utf8Path(corpus.root) || !std::filesystem::is_directory(corpus.root, error) ||
      error || IsWithin(corpus.root, outputRoot) || IsWithin(corpus.root, run.workRoot)) {
    return "provenance output must be outside existing corpus directories";
  }
  return {};
}

std::string CaptureSources(const ProvenanceOptions& options, RunProvenance& run,
                           const std::filesystem::path& outputRoot) {
  std::error_code error;
  run.renderer = std::filesystem::canonical(options.renderer, error);
  if (error || !Utf8Path(run.renderer)) {
    return "cannot resolve bounded UTF-8 renderer for provenance";
  }
  Budget budget{options.limits};
  std::map<std::filesystem::path, FileFingerprint> engine;
  auto issue = CaptureFile(run.renderer, budget, engine);
  if (!issue.empty()) {
    return issue;
  }
  run.rendererFingerprint = engine.begin()->second;
  for (auto corpus : options.corpora) {
    issue = ResolveCorpus(corpus, run, outputRoot);
    if (!issue.empty()) {
      return issue;
    }
    issue = CaptureCorpus(corpus, budget, run.files);
    if (!issue.empty()) {
      return issue;
    }
    run.corpora.push_back(std::move(corpus));
  }
  return {};
}

Json BuildRecord(const ProvenanceOptions& options, const RunProvenance& run,
                 const std::string& invocationDirectory) {
  Json corpora = Json::array();
  for (const auto& corpus : run.corpora) {
    Json files = Json::object();
    for (const auto& [path, fingerprint] : run.files) {
      if (IsWithin(corpus.root, path)) {
        files[*Utf8Path(path.lexically_relative(corpus.root))] = FingerprintJson(fingerprint);
      }
    }
    corpora.push_back({{"name", corpus.name},
                       {"root", *Utf8Path(corpus.root)},
                       {"revision", corpus.revision},
                       {"revision_evidence", corpus.revisionEvidence},
                       {"content_sha256", picosha2::hash256_hex_string(files.dump())},
                       {"files", std::move(files)}});
  }
  return {{"format", "donner-svg-suite-run-1"},
          {"renderer",
           {{"path", *Utf8Path(run.renderer)},
            {"fingerprint", FingerprintJson(run.rendererFingerprint)},
            {"declared",
             {{"revision", options.engineRevision},
              {"backend", options.backend},
              {"build_flags", options.buildFlags},
              {"verification", "caller-declared, unverified"}}}}},
          {"corpora", std::move(corpora)},
          {"invocation_argv", options.invocation},
          {"invocation_directory", invocationDirectory},
          {"renderer_arguments_template", options.rendererArguments},
          {"work_directory", *Utf8Path(run.workRoot)},
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
struct RunDirectories {
  std::filesystem::path outputRoot;
  std::filesystem::path workRoot;
  std::string invocationDirectory;
  std::string error;
};

RunDirectories ResolveRunDirectories(const ProvenanceOptions& options,
                                     const std::filesystem::path& artifactRoot) {
  if (!Utf8Path(artifactRoot)) {
    return {{}, {}, {}, "provenance artifact path requires bounded NUL-free UTF-8"};
  }
  std::error_code error;
  const auto cwd = std::filesystem::current_path(error);
  const auto invocationDirectory = error ? std::nullopt : Utf8Path(cwd);
  if (!invocationDirectory) {
    return {{}, {}, {}, "invocation directory requires bounded NUL-free UTF-8"};
  }
  const auto outputRoot = std::filesystem::weakly_canonical(artifactRoot, error);
  if (error || outputRoot.empty() || !Utf8Path(outputRoot)) {
    return {{}, {}, {}, "cannot resolve provenance output directory"};
  }
  const auto workRoot = std::filesystem::weakly_canonical(options.workRoot, error);
  if (error || !Utf8Path(workRoot)) {
    return {{}, {}, {}, "cannot resolve bounded UTF-8 process work directory"};
  }
  return {outputRoot, workRoot, *invocationDirectory, {}};
}

}  // namespace

ProvenanceResult CreateRunProvenance(const ProvenanceOptions& options,
                                     const std::filesystem::path& artifactRoot) {
  auto issue = ValidateOptions(options);
  if (!issue.empty()) {
    return {{}, issue};
  }
  auto run = std::make_shared<RunProvenance>();
  run->limits = options.limits;
  const auto directories = ResolveRunDirectories(options, artifactRoot);
  if (!directories.error.empty()) {
    return {{}, directories.error};
  }
  run->workRoot = directories.workRoot;
  const auto& outputRoot = directories.outputRoot;
  std::error_code error;
  issue = CaptureSources(options, *run, outputRoot);
  if (!issue.empty()) {
    return {{}, issue};
  }
  Json record = BuildRecord(options, *run, directories.invocationDirectory);
  run->runId = picosha2::hash256_hex_string(record.dump());
  record["run_id"] = run->runId;
  std::filesystem::create_directories(outputRoot, error);
  if (error) {
    return {{}, "cannot create provenance output directory"};
  }
  run->recordPath = outputRoot / "run.json";
  if (!Utf8Path(run->recordPath)) {
    return {{}, "run record path requires bounded NUL-free UTF-8"};
  }
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
          {"gtest_output", IsValidIdentityText(execution.gtestOutput) ? Json(execution.gtestOutput)
                                                                      : Json(nullptr)},
          {"corpus_complete", execution.corpusComplete()}};
}

struct XmlReportOutcome {
  Json record = nullptr;
  std::string error;
};

bool HasGoogleTestXmlEnvelope(std::string_view text) {
  constexpr std::string_view kDeclaration = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  if (!text.starts_with(kDeclaration)) {
    return false;
  }
  text.remove_prefix(kDeclaration.size());
  const auto start = text.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return false;
  }
  text.remove_prefix(start);
  text = text.substr(0, text.find_last_not_of(" \t\r\n") + 1);
  return (text.starts_with("<testsuites ") || text.starts_with("<testsuites>")) &&
         text.ends_with("</testsuites>");
}

std::string CheckXmlReportEnvelope(const std::filesystem::path& path,
                                   const FileFingerprint& fingerprint) {
  std::ifstream input(path, std::ios::binary);
  std::string text(static_cast<size_t>(fingerprint.bytes), '\0');
  if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) ||
      input.peek() != std::char_traits<char>::eof() ||
      picosha2::hash256_hex_string(text) != fingerprint.sha256) {
    return "XML report changed or could not be read completely";
  }
  // This checks the trusted producer's envelope, not arbitrary XML syntax or filesystem attacks.
  return HasGoogleTestXmlEnvelope(text) ? "" : "XML report lacks a complete GoogleTest envelope";
}

XmlReportOutcome ReadXmlReport(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error))) {
    return {nullptr, "XML report is missing or is not a regular file"};
  }
  const auto report = FingerprintFile(path, 32u * 1024u * 1024u);
  if (!report.fingerprint) {
    return {nullptr, "Cannot fingerprint XML report: " + report.error};
  }
  if (report.fingerprint->bytes == 0) {
    return {nullptr, "XML report is empty"};
  }
  const auto issue = CheckXmlReportEnvelope(path, *report.fingerprint);
  if (!issue.empty()) {
    return {nullptr, issue};
  }
  return {{{"path", *Utf8Path(path)},
           {"sha256", report.fingerprint->sha256},
           {"bytes", report.fingerprint->bytes},
           {"scope", "GoogleTest final iteration; execution counts include all repeats"}},
          {}};
}

XmlReportOutcome XmlReportRecord(const RunExecutionSummary& execution) {
  if (!IsValidIdentityText(execution.gtestOutput) || !Utf8Path(execution.xmlReport)) {
    return {nullptr, "XML evidence identity requires bounded NUL-free UTF-8"};
  }
  if (!execution.xmlReportError.empty()) {
    return {nullptr, execution.xmlReportError};
  }
  if (execution.completedInvocations == 0 || execution.xmlReport.empty()) {
    return {nullptr, "XML report is unavailable for this invocation"};
  }
  if (execution.gtestOutput != "xml" && !execution.gtestOutput.starts_with("xml:")) {
    return {nullptr, "XML evidence requires XML output"};
  }
  return ReadXmlReport(execution.xmlReport);
}
bool VerificationAccepted(bool validRunId, std::string_view verificationError, int testExitCode,
                          const RunExecutionSummary& execution, const XmlReportOutcome& xml) {
  return validRunId && verificationError.empty() && testExitCode == 0 &&
         execution.selectionComplete() && xml.error.empty();
}
}  // namespace

std::string WriteRunVerification(const RunProvenance& run, std::string_view verificationError,
                                 int testExitCode, const RunExecutionSummary& execution) {
  const auto destination = run.recordPath.parent_path() / "run-verification.json";
  if (!Utf8Path(run.recordPath) || !Utf8Path(destination)) {
    return "verification destination requires bounded NUL-free UTF-8";
  }
  const bool validRunId = IsValidIdentityText(run.runId, 4096);
  const std::string identityError =
      validRunId ? "" : "run identity requires bounded NUL-free UTF-8";
  const auto xml = XmlReportRecord(execution);
  const bool accepted =
      VerificationAccepted(validRunId, verificationError, testExitCode, execution, xml);
  const Json record = {
      {"run_id", validRunId ? Json(run.runId) : Json(nullptr)},
      {"identity_error", identityError},
      {"run_record", "run.json"},
      {"sources_unchanged_at_verification", verificationError.empty()},
      {"verification_error", DiagnosticUtf8(verificationError)},
      {"diagnostics_encoding", "utf8-with-replacement"},
      {"verification_error_truncated", verificationError.size() > kMaxDiagnosticBytes},
      {"test_exit_code", testExitCode},
      {"accepted", accepted},
      {"accepted_scope", execution.corpusComplete() ? "complete-corpus" : "selected-cases"},
      {"complete_corpus_accepted", accepted && execution.corpusComplete()},
      {"execution", ExecutionRecord(execution)},
      {"xml_report", xml.record},
      {"xml_report_error", DiagnosticUtf8(xml.error)},
      {"xml_report_error_truncated", xml.error.size() > kMaxDiagnosticBytes}};
  const auto writeError = WriteExclusive(destination, record.dump(2) + "\n");
  for (const std::string_view error :
       {std::string_view(writeError), std::string_view(identityError), std::string_view(xml.error),
        verificationError}) {
    if (!error.empty()) {
      return DiagnosticUtf8(error);
    }
  }
  return {};
}

}  // namespace donner::svg_suite
