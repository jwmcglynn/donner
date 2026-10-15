/// @file
/// A renderer callback consumer that links without the process or run-record writer.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "tools/svg_suite/GtestSuite.h"

namespace {
using namespace donner::svg_suite;

int runConsumer(const std::filesystem::path& directory) {
  const auto corpus = directory / "corpus";
  std::filesystem::create_directory(corpus);
  std::ofstream(corpus / "red.svg") << "<svg xmlns='http://www.w3.org/2000/svg' width='1' "
                                       "height='1'><rect width='1' height='1' fill='red'/></svg>";
  const Image image{1, 1, 1, {255, 0, 0, 255}};
  const auto pngError = WritePng(corpus / "red.png", {1, 1, 1, image.data});
  const auto discovery = DiscoverCases({{"callback", corpus}});
  if (!pngError.empty() || !discovery.error.empty() || discovery.cases.size() != 1) {
    std::cerr << "Cannot create or discover callback fixture\n";
    return 2;
  }
  size_t calls = 0;
  const auto error = RegisterTests(
      discovery,
      [&calls, image](const StaticCase&, int width, int height) -> ImageResult {
        ++calls;
        if (width != 1 || height != 1) {
          return {{}, "unexpected callback dimensions"};
        }
        return {image, {}};
      },
      directory / "artifacts");
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 2;
  }
  RunExecutionSummary summary;
  const int status = RunSelectedTests(&summary);
  if (calls != 1 || !summary.corpusComplete() || summary.xmlReport.empty()) {
    std::cerr << "Callback did not complete its case and XML evidence\n";
    return 2;
  }
  return status;
}
}  // namespace

int main(int argc, char** argv) {
  const char* temporary = std::getenv("TEST_TMPDIR");
  const auto base =
      temporary ? std::filesystem::path(temporary) : std::filesystem::temp_directory_path();
  const auto directory =
      base / ("svg-callback-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code error;
  if (!std::filesystem::create_directory(directory, error)) {
    std::cerr << "Cannot create fresh callback fixture directory\n";
    return 2;
  }
  if (GTEST_FLAG_GET(output).empty()) {
    const char* xml = std::getenv("XML_OUTPUT_FILE");
    GTEST_FLAG_SET(output,
                   "xml:" + (xml ? std::filesystem::path(xml) : directory / "gtest.xml").string());
  }
  testing::InitGoogleTest(&argc, argv);
  const int result = runConsumer(directory);
  std::filesystem::remove_all(directory, error);
  return result;
}
