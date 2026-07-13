/// @file
/// Thin CLI over Donner's existing image comparison, for the portable SVG2 test
/// suite runner (design 0057, Rollout step 4).
///
/// The portable runner compares an adapter's rendered PNG against a corpus
/// oracle PNG. So the Donner parity lane applies the SAME comparison as the
/// existing resvg C++ fixture (donner/svg/renderer/tests/resvg_test_suite.cc),
/// this tool reuses Donner's own pieces rather than adding a second
/// implementation: it loads both PNGs with
/// RendererImageTestUtils::readRgbaImageFromPngFile and compares them with
/// pixelmatch::pixelmatch, exactly as ImageComparisonTestFixture does.
///
/// Usage:
///   svg2_image_compare --expected golden.png --actual out.png
///                      --threshold 0.02 --max-mismatched-pixels 100
///                      [--include-aa]
///
/// It writes a one-line JSON verdict to stdout:
///   {"status":"ok","mismatched_pixels":N,"max_mismatched_pixels":M,
///    "threshold":T,"passed":bool}
/// A read/parse failure reports {"status":"infrastructure-error",...} and a
/// non-zero exit. The verdict never depends on process exit status alone.

#include <pixelmatch/pixelmatch.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "nlohmann/json.hpp"

namespace {

using donner::svg::RendererImageTestUtils;

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

  const auto expected = RendererImageTestUtils::readRgbaImageFromPngFile(expectedPath.c_str());
  if (!expected) {
    return emitInfrastructureError("cannot read expected PNG: " + expectedPath);
  }
  const auto actual = RendererImageTestUtils::readRgbaImageFromPngFile(actualPath.c_str());
  if (!actual) {
    return emitInfrastructureError("cannot read actual PNG: " + actualPath);
  }

  nlohmann::json document;
  document["status"] = "ok";
  document["threshold"] = threshold;
  document["max_mismatched_pixels"] = maxMismatchedPixels;

  if (expected->width != actual->width || expected->height != actual->height ||
      expected->strideInPixels != actual->strideInPixels) {
    document["passed"] = false;
    document["reason"] = "dimension-mismatch";
    document["expected"] = {expected->width, expected->height};
    document["actual"] = {actual->width, actual->height};
    std::cout << document.dump() << "\n";
    return 0;
  }

  std::vector<uint8_t> diff(expected->data.size());
  pixelmatch::Options options;
  options.threshold = threshold;
  options.includeAA = includeAntiAliasing;
  const int mismatchedPixels =
      pixelmatch::pixelmatch(expected->data, actual->data, diff, expected->width, expected->height,
                             expected->strideInPixels, options);

  document["mismatched_pixels"] = mismatchedPixels;
  document["passed"] = mismatchedPixels <= maxMismatchedPixels;
  std::cout << document.dump() << "\n";
  return 0;
}
