#include "tools/svg_suite/GtestSuite.h"

#include <gtest/gtest.h>

#include <iostream>
#include <set>
#include <string_view>
#include <utility>

namespace donner::svg_suite {
namespace {
size_t completedInvocations = 0;
std::set<std::string> completedCases;
size_t currentIteration = 0;

class IterationListener : public testing::EmptyTestEventListener {
public:
  void OnTestIterationStart(const testing::UnitTest&, int iteration) override {
    currentIteration = static_cast<size_t>(iteration) + 1;
  }
};

class ExecutionListener : public testing::EmptyTestEventListener {
public:
  void OnTestEnd(const testing::TestInfo& test) override {
    if (std::string_view(test.test_suite_name()) == "SvgSuite" && test.result()->Skipped()) {
      ++skippedInvocations;
    }
  }

  size_t skippedInvocations = 0;
};

std::filesystem::path XmlReportPath(std::string_view output) {
  if (output == "xml") {
    return std::filesystem::absolute("test_detail.xml");
  }
  if (!output.starts_with("xml:")) {
    return {};
  }
  const std::filesystem::path path(output.substr(4));
  std::error_code error;
  if (path.filename().empty() || std::filesystem::is_directory(path, error)) {
    return {};  // GoogleTest chooses a unique filename for directory output; do not guess it.
  }
  return std::filesystem::absolute(path, error);
}

std::string CheckFreshXmlDestination(const std::filesystem::path& path) {
  if (path.empty()) {
    return "XML evidence requires xml or xml:<file>; JSON and directory output are unsupported";
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return "Cannot inspect XML report destination: " + error.message();
  }
  if (status.type() != std::filesystem::file_type::not_found) {
    return "XML report destination already exists; choose a fresh file";
  }
  return {};
}

void RecordCorpusCounts(RunExecutionSummary& summary) {
  const auto* unit = testing::UnitTest::GetInstance();
  for (int index = 0; index < unit->total_test_suite_count(); ++index) {
    const auto* suite = unit->GetTestSuite(index);
    if (std::string_view(suite->name()) == "SvgSuite") {
      summary.discoveredCases = static_cast<size_t>(suite->total_test_count());
      summary.selectedCases = static_cast<size_t>(suite->test_to_run_count());
      return;
    }
  }
}

class StaticImageTest : public testing::Test {
public:
  StaticImageTest(StaticCase test, Renderer renderer, std::filesystem::path artifactRoot,
                  ComparisonOptions options)
      : test_(std::move(test)),
        renderer_(std::move(renderer)),
        artifactRoot_(std::move(artifactRoot)),
        options_(options) {}

  void TestBody() override {
    if (GTEST_FLAG_GET(repeat) != 1) {
      ASSERT_GT(currentIteration, 0u);
      test_.iteration = currentIteration;
    }
    const CaseResult result = RunCase(test_, renderer_, artifactRoot_, options_);
    ++completedInvocations;
    completedCases.insert(test_.id);
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
  static const bool listenerInstalled = [] {
    testing::UnitTest::GetInstance()->listeners().Append(new IterationListener);
    return true;
  }();
  (void)listenerInstalled;
  for (const StaticCase& test : discovery.cases) {
    testing::RegisterTest("SvgSuite", test.id.c_str(), nullptr, test.svgPath.string().c_str(),
                          __FILE__, __LINE__,
                          [test, renderer, artifactRoot, options]() -> StaticImageTest* {
                            return new StaticImageTest(test, renderer, artifactRoot, options);
                          });
  }
  return {};
}

int RunSelectedTests(RunExecutionSummary* execution) {
  completedInvocations = 0;
  completedCases.clear();
  RunExecutionSummary summary;
  summary.repeatCount = GTEST_FLAG_GET(repeat);
  summary.listOnly = GTEST_FLAG_GET(list_tests);
  summary.gtestOutput = GTEST_FLAG_GET(output);
  const auto reportPath = XmlReportPath(summary.gtestOutput);
  summary.xmlReportError = CheckFreshXmlDestination(reportPath);
  if (!summary.xmlReportError.empty()) {
    std::cerr << summary.xmlReportError << '\n';
    RecordCorpusCounts(summary);
    if (execution != nullptr) {
      *execution = std::move(summary);
    }
    return 2;
  }
  auto& listeners = testing::UnitTest::GetInstance()->listeners();
  auto* observer = new ExecutionListener;
  listeners.Append(observer);
  const int status = RUN_ALL_TESTS();
  summary.skippedInvocations = observer->skippedInvocations;
  delete listeners.Release(observer);
  RecordCorpusCounts(summary);
  summary.completedCases = completedCases.size();
  summary.completedInvocations = completedInvocations;
  if (completedInvocations > 0) {
    std::error_code error;
    if (std::filesystem::is_regular_file(std::filesystem::symlink_status(reportPath, error))) {
      summary.xmlReport = reportPath;
    } else {
      summary.xmlReportError = "XML report was not generated as a regular file";
    }
  }
  const bool selectionComplete = summary.selectionComplete() && summary.xmlReportError.empty();
  if (execution != nullptr) {
    *execution = std::move(summary);
  }
  if (!selectionComplete) {
    std::cerr << "Selected SVG suite cases were skipped or not executed; this cannot pass.\n";
    return status == 0 ? 2 : status;
  }
  return status;
}
}  // namespace donner::svg_suite
