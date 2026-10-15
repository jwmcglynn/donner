/// @file
/// JSON command-line wrapper over the shared bounded PNG and pixelmatch helper.

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "tools/svg_suite/ImageCompare.h"

namespace {

namespace suite = donner::svg_suite;

int emitInfrastructureError(const std::string& message) {
  nlohmann::json document;
  document["status"] = "infrastructure-error";
  document["diagnostics"] = message;
  std::cout << document.dump() << "\n";
  return 2;
}

std::string requireValue(int argc, char** argv, int* index, std::string_view flag) {
  if (*index + 1 >= argc) {
    std::cerr << "missing value for " << flag << "\n";
    std::exit(2);
  }
  return argv[++(*index)];
}

// Exceptions are disabled in this build, so parse numbers without throwing
// (std::stod/std::stoi would abort on malformed input).
bool parseDouble(const std::string& text, double* out) {
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
    return false;
  }
  *out = value;
  return true;
}

bool parseInt(const std::string& text, int* out) {
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto [pointer, code] = std::from_chars(begin, end, *out);
  return code == std::errc() && pointer == end;
}

}  // namespace

int main(int argc, char** argv) {
  std::string expectedPath;
  std::string actualPath;
  double threshold = 0.0;
  int maxMismatchedPixels = 0;
  bool includeAntiAliasing = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--expected") {
      expectedPath = requireValue(argc, argv, &i, arg);
    } else if (arg == "--actual") {
      actualPath = requireValue(argc, argv, &i, arg);
    } else if (arg == "--threshold") {
      const std::string value = requireValue(argc, argv, &i, arg);
      if (!parseDouble(value, &threshold)) {
        return emitInfrastructureError("invalid --threshold value: " + value);
      }
    } else if (arg == "--max-mismatched-pixels") {
      const std::string value = requireValue(argc, argv, &i, arg);
      if (!parseInt(value, &maxMismatchedPixels)) {
        return emitInfrastructureError("invalid --max-mismatched-pixels value: " + value);
      }
    } else if (arg == "--include-aa") {
      includeAntiAliasing = true;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }

  if (expectedPath.empty() || actualPath.empty()) {
    return emitInfrastructureError("both --expected and --actual are required");
  }

  const auto expected = suite::ReadPng(expectedPath);
  if (!expected.image) {
    return emitInfrastructureError("cannot read expected PNG: " + expected.error);
  }
  const auto actual = suite::ReadPng(actualPath);
  if (!actual.image) {
    return emitInfrastructureError("cannot read actual PNG: " + actual.error);
  }
  const auto& reference = *expected.image;
  const auto& rendered = *actual.image;
  const auto comparison = suite::CompareImages(
      {reference.width, reference.height, reference.strideInPixels, reference.data},
      {rendered.width, rendered.height, rendered.strideInPixels, rendered.data},
      {threshold, maxMismatchedPixels, includeAntiAliasing});
  if (!comparison.error.empty() && comparison.error != "dimension-mismatch") {
    return emitInfrastructureError(comparison.error);
  }
  nlohmann::json document = {{"status", "ok"},
                             {"threshold", threshold},
                             {"max_mismatched_pixels", maxMismatchedPixels},
                             {"passed", comparison.passed}};
  if (comparison.error == "dimension-mismatch") {
    document["reason"] = comparison.error;
    document["expected"] = {reference.width, reference.height};
    document["actual"] = {rendered.width, rendered.height};
  } else {
    document["mismatched_pixels"] = *comparison.mismatchedPixels;
  }
  std::cout << document.dump() << "\n";
  return 0;
}
