#include <picosha2.h>

#include <array>
#include <fstream>

#include "tools/svg_suite/Provenance.h"

namespace donner::svg_suite {

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

const FileFingerprint* FindFingerprint(const RunProvenance& run,
                                       const std::filesystem::path& path) {
  const auto found = run.files.find(path.lexically_normal());
  return found == run.files.end() ? nullptr : &found->second;
}
}  // namespace donner::svg_suite
