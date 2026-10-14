#include "tools/svg_suite/GtestSuite.h"

#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <utility>

namespace donner::svg_suite {
namespace {
std::atomic<size_t> completedCases{0};

class StaticImageTest : public testing::Test {
public:
  StaticImageTest(StaticCase test, Renderer renderer, std::filesystem::path artifactRoot,
                  ComparisonOptions options)
      : test_(std::move(test)),
        renderer_(std::move(renderer)),
        artifactRoot_(std::move(artifactRoot)),
        options_(options) {}

  void TestBody() override {
    const CaseResult result = RunCase(test_, renderer_, artifactRoot_, options_);
    completedCases.fetch_add(1, std::memory_order_relaxed);
    EXPECT_TRUE(result.passed) << test_.svgPath << ": " << result.error << "; mismatched pixels: "
                               << (result.mismatchedPixels
                                       ? std::to_string(*result.mismatchedPixels)
                                       : "unavailable")
                               << "; artifacts: " << result.artifactDirectory;
  }

private:
  StaticCase test_;
  Renderer renderer_;
  std::filesystem::path artifactRoot_;
  ComparisonOptions options_;
};
}  // namespace

std::string RegisterTests(const DiscoveryResult& discovery, Renderer renderer,
                          const std::filesystem::path& artifactRoot,
                          const ComparisonOptions& options) {
  if (!discovery.error.empty()) {
    return discovery.error;
  }
  if (discovery.cases.empty() || !renderer) {
    return "cannot register empty corpus or missing renderer";
  }
  for (const StaticCase& test : discovery.cases) {
    testing::RegisterTest("SvgSuite", test.id.c_str(), nullptr, test.svgPath.string().c_str(),
                          __FILE__, __LINE__,
                          [test, renderer, artifactRoot, options]() -> StaticImageTest* {
                            return new StaticImageTest(test, renderer, artifactRoot, options);
                          });
  }
  return {};
}

int RunSelectedTests() {
  completedCases.store(0, std::memory_order_relaxed);
  const int status = RUN_ALL_TESTS();
  if (completedCases.load(std::memory_order_relaxed) == 0) {
    std::cerr << "No SVG suite cases executed; selection or listing is not a passing run.\n";
    return 2;
  }
  return status;
}
}  // namespace donner::svg_suite
