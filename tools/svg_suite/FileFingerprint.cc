#include <picosha2.h>

#include <array>
#include <fstream>

#include "tools/svg_suite/Provenance.h"
#include "tools/svg_suite/TextEncoding.h"

namespace donner::svg_suite {

namespace {
std::string ValidateFingerprintPath(const std::filesystem::path& path) {
  if (!Utf8Path(path)) {
    return "fingerprint path requires bounded NUL-free UTF-8";
  }
  std::error_code error;
  const auto resolved = std::filesystem::canonical(path, error);
  if (!error && !Utf8Path(resolved)) {
    return "resolved fingerprint path requires bounded NUL-free UTF-8";
  }
  if (error || !std::filesystem::is_regular_file(path, error) || error) {
    return "missing or nonregular fingerprint input: " + *Utf8Path(path);
  }
  return {};
}

FingerprintResult StreamFingerprint(const std::filesystem::path& path, uintmax_t maxBytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {{}, "cannot open fingerprint input: " + *Utf8Path(path)};
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
  hasher.finish();
  return {FileFingerprint{bytes, picosha2::get_hash_hex_string(hasher)}, {}};
}

bool SameFingerprintSource(const std::filesystem::path& path, uintmax_t beforeSize,
                           std::filesystem::file_time_type beforeTime, uintmax_t bytes) {
  std::error_code error;
  const auto afterSize = std::filesystem::file_size(path, error);
  if (error || beforeSize != bytes || afterSize != bytes) {
    return false;
  }
  const auto afterTime = std::filesystem::last_write_time(path, error);
  return !error && afterTime == beforeTime;
}
}  // namespace

FingerprintResult FingerprintFile(const std::filesystem::path& path, uintmax_t maxBytes) {
  const auto validation = ValidateFingerprintPath(path);
  if (!validation.empty()) {
    return {{}, validation};
  }
  std::error_code error;
  const auto beforeSize = std::filesystem::file_size(path, error);
  if (error || beforeSize > maxBytes) {
    return {{}, "fingerprint input exceeds byte budget: " + *Utf8Path(path)};
  }
  const auto beforeTime = std::filesystem::last_write_time(path, error);
  if (error) {
    return {{}, "cannot stat fingerprint input"};
  }
  const auto result = StreamFingerprint(path, maxBytes);
  if (!result.fingerprint) {
    return result;
  }
  if (!SameFingerprintSource(path, beforeSize, beforeTime, result.fingerprint->bytes)) {
    return {{}, "fingerprint input changed during hashing"};
  }
  return result;
}

std::string VerifyFile(const std::filesystem::path& path, const FileFingerprint& expected) {
  const auto actual = FingerprintFile(path, expected.bytes);
  if (!actual.fingerprint) {
    return actual.error;
  }
  return *actual.fingerprint == expected ? "" : "source changed since snapshot: " + *Utf8Path(path);
}

const FileFingerprint* FindFingerprint(const RunProvenance& run,
                                       const std::filesystem::path& path) {
  const auto found = run.files.find(path.lexically_normal());
  return found == run.files.end() ? nullptr : &found->second;
}
}  // namespace donner::svg_suite
