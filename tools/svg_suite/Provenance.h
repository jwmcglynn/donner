#pragma once
/// @file
/// Bounded source hashing and create-only evidence for a native suite invocation.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tools/svg_suite/ImageCompare.h"

namespace donner::svg_suite {

/// Streaming bounds include the engine and all corpus files together.
struct ProvenanceLimits {
  size_t maxFiles = 100000;
  uintmax_t maxFileBytes = 512ull * 1024 * 1024;
  uintmax_t maxTotalBytes = 4ull * 1024 * 1024 * 1024;
};

/// SHA-256 of the actual file bytes, not a caller's revision label.
struct FileFingerprint {
  uintmax_t bytes = 0;
  std::string sha256;
  bool operator==(const FileFingerprint&) const = default;
};

struct FingerprintResult {
  std::optional<FileFingerprint> fingerprint;
  std::string error;
};

/// Hash a bounded regular file in fixed-size chunks using PicoSHA2.
FingerprintResult FingerprintFile(const std::filesystem::path& path, uintmax_t maxBytes);

/// Hash again and reject changed bytes, missing files, or hashing errors.
std::string VerifyFile(const std::filesystem::path& path, const FileFingerprint& expected);

/// Revision labels describe provenance claims; content fingerprints remain authoritative evidence.
struct ProvenanceCorpus {
  std::string name;
  std::filesystem::path root;
  std::string revision = "unspecified";
  std::string revisionEvidence = "caller-declared, unverified";
};

struct ProvenanceOptions {
  std::vector<ProvenanceCorpus> corpora;
  std::filesystem::path renderer;
  std::filesystem::path workRoot;
  std::vector<std::string> invocation;
  std::vector<std::string> rendererArguments;
  std::string engineRevision = "unspecified";
  std::string backend = "unspecified";
  std::string buildFlags = "unspecified";
  int64_t timeoutMs = 10000;
  bool trustedInputs = false;
  ComparisonOptions comparison;
  ProvenanceLimits limits;
};

/// Shared run identity referenced by each metrics.json. Sources are verified at the boundaries
/// documented by VerifyRunProvenance, not a filesystem snapshot or adversarial mutation sandbox.
struct RunProvenance {
  std::string runId;
  std::filesystem::path recordPath;
  std::filesystem::path workRoot;
  std::filesystem::path renderer;
  FileFingerprint rendererFingerprint;
  std::vector<ProvenanceCorpus> corpora;
  std::map<std::filesystem::path, FileFingerprint> files;
  ProvenanceLimits limits;
};

struct ProvenanceResult {
  std::shared_ptr<const RunProvenance> run;
  std::string error;
};

/// Hash all corpus files, including fonts/resources, and exclusively create read-only run.json.
/// Existing run.json is an error. artifactRoot must be outside all source corpora.
ProvenanceResult CreateRunProvenance(const ProvenanceOptions& options,
                                     const std::filesystem::path& artifactRoot);

/// Re-enumerate and hash engine/corpora after execution. Added, removed and changed files reject
/// the aggregate run. Concurrent mutate-and-restore attacks require external immutable inputs.
std::string VerifyRunProvenance(const RunProvenance& run);

/// Preserve final source validation and test exit status in create-only run-verification.json.
std::string WriteRunVerification(const RunProvenance& run, std::string_view verificationError,
                                 int testExitCode);

/// Return the captured fingerprint at the exact source path, or nullptr if it was not captured.
const FileFingerprint* FindFingerprint(const RunProvenance& run, const std::filesystem::path& path);

}  // namespace donner::svg_suite
