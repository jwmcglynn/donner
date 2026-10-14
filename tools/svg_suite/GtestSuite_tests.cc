/// @file
/// Exercise the GoogleTest runner in fresh processes with real image-case completion.

#include "tools/svg_suite/GtestSuite.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/svg_suite/Provenance.h"

namespace donner::svg_suite {
namespace {
std::filesystem::path executable;

class SkipSecondCase : public testing::EmptyTestEventListener {
public:
  void OnTestStart(const testing::TestInfo& test) override {
    if (std::string_view(test.test_suite_name()) == "SvgSuite" &&
        std::string_view(test.name()).ends_with("_7365636f6e642e737667")) {
      GTEST_SKIP() << "intentional second-case skip";
    }
  }
};

int RunFixture(int argc, char** argv) {
  const std::filesystem::path renderer = std::filesystem::absolute(argv[0]);
  const std::filesystem::path root(argv[2]);
  const std::string_view fixtureMode = argc > 3 ? argv[3] : "";
  const bool failFirst = fixtureMode == "--fixture-fail-first";
  if (fixtureMode == "--fixture-empty-xml") {
    std::ofstream("test_detail.xml") << "stale-report-sentinel";
  }
  std::filesystem::create_directories(root / "corpus");
  const Image image{1, 1, 1, {255, 0, 0, 255}};
  for (const char* name : {"first", "second"}) {
    std::ofstream(root / "corpus" / (std::string(name) + ".svg")) << "<svg/>";
    if (!WritePng(root / "corpus" / (std::string(name) + ".png"), {1, 1, 1, image.data}).empty()) {
      return 3;
    }
  }
  argc -= 2;
  argv += 2;
  GTEST_FLAG_SET(output, "xml:" + (root / "artifacts/gtest.xml").string());
  testing::InitGoogleTest(&argc, argv);
  if (fixtureMode == "--fixture-skip-second") {
    testing::UnitTest::GetInstance()->listeners().Append(new SkipSecondCase);
  }
  const auto discovery = DiscoverCases({{"fixture", root / "corpus"}});
  ProvenanceOptions options;
  options.corpora = {{"fixture", root / "corpus"}};
  options.renderer = renderer;
  options.workRoot = root / "work";
  const auto provenance = CreateRunProvenance(options, root / "artifacts");
  if (!provenance.run) {
    return 3;
  }
  const auto issue = RegisterTests(
      discovery,
      [image, failFirst](const StaticCase& test, int, int) -> ImageResult {
        if (failFirst && test.svgPath.stem() == "first") {
          return {{}, "intentional fixture failure"};
        }
        return {image, {}};
      },
      root / "artifacts");
  if (!issue.empty()) {
    return 3;
  }
  RunExecutionSummary execution;
  const int result = RunSelectedTests(&execution);
  if (fixtureMode == "--fixture-remove-xml") {
    std::filesystem::remove(execution.xmlReport);
  }
  const auto error = WriteRunVerification(*provenance.run, "", result, execution);
  return error.empty() ? result : 3;
}

class GtestSuiteTest : public testing::Test {
protected:
  void SetUp() override {
    const char* temporary = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary, nullptr);
    std::string pattern = (std::filesystem::path(temporary) / "gtest-suite-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  int run(std::vector<std::string> flags) {
    std::vector<std::string> arguments{executable.string(), "--fixture-suite", root_.string()};
    arguments.insert(arguments.end(), flags.begin(), flags.end());
    std::vector<char*> pointers;
    for (auto& argument : arguments) {
      pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    const std::string childDirectory = root_.string();
    const pid_t child = fork();
    if (child < 0) {
      return -1;
    }
    if (child == 0) {
      // GoogleTest records its original working directory during static initialization.
      if (chdir(childDirectory.c_str()) != 0) {
        _exit(126);
      }
      execv(pointers.front(), pointers.data());
      _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
      return -1;
    }
    return WEXITSTATUS(status);
  }

  std::string verification() const {
    std::ifstream input(root_ / "artifacts/run-verification.json");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  std::filesystem::path root_;
};

TEST_F(GtestSuiteTest, ListingCasesCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_list_tests"}), 2);
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_cases\": 0"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, ZeroRepeatsCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_repeat=0"}), 2);
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_cases\": 0"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, EmptyFilterCannotCountAsExecution) {
  EXPECT_EQ(run({"--gtest_filter=NothingMatches.*"}), 2);
}

TEST_F(GtestSuiteTest, MatchingFilterExecutesAndPasses) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*"}), 0);
}

TEST_F(GtestSuiteTest, NarrowFilterCannotClaimCompleteCorpus) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*_66697273742e737667"}), 0);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"accepted\": true"));
  EXPECT_THAT(record, testing::HasSubstr("\"accepted_scope\": \"selected-cases\""));
  EXPECT_THAT(record, testing::HasSubstr("\"discovered_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"complete_corpus_accepted\": false"));
}

TEST_F(GtestSuiteTest, FullCorpusRecordsCompletionAndDefaultXml) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*"}), 0);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"discovered_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_invocations\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"complete_corpus_accepted\": true"));
  EXPECT_THAT(record, testing::HasSubstr("\"accepted_scope\": \"complete-corpus\""));
  EXPECT_THAT(record, testing::HasSubstr("artifacts/gtest.xml"));
  EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "artifacts/gtest.xml"));
}

TEST_F(GtestSuiteTest, RepeatsDoNotCountAsDistinctCompletedCases) {
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*_66697273742e737667", "--gtest_repeat=2"}), 1);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_invocations\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"requested_repeat_count\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, FailFastSkipsDoNotCountAsCompletedCases) {
  EXPECT_EQ(run({"--fixture-fail-first", "--gtest_filter=SvgSuite.*", "--gtest_fail_fast"}), 1);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"skipped_invocations\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"corpus_complete\": false"));
}

TEST_F(GtestSuiteTest, ExplicitXmlOutputIsPreserved) {
  const auto path = root_ / "chosen.xml";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=xml:" + path.string()}), 0);
  EXPECT_TRUE(std::filesystem::is_regular_file(path));
  EXPECT_FALSE(std::filesystem::exists(root_ / "artifacts/gtest.xml"));
  EXPECT_THAT(verification(), testing::HasSubstr("chosen.xml"));
}

TEST_F(GtestSuiteTest, SuccessfulAndSkippedSelectionCannotReturnSuccess) {
  EXPECT_EQ(run({"--fixture-skip-second", "--gtest_filter=SvgSuite.*"}), 2);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"selected_cases\": 2"));
  EXPECT_THAT(record, testing::HasSubstr("\"completed_cases\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"skipped_invocations\": 1"));
  EXPECT_THAT(record, testing::HasSubstr("\"accepted\": false"));
}

TEST_F(GtestSuiteTest, JsonOutputCannotCertifyWithoutXml) {
  const auto output = root_ / "chosen.json";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=json:" + output.string()}), 2);
  EXPECT_FALSE(std::filesystem::exists(output));
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"complete_corpus_accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"xml_report\": null"));
}

TEST_F(GtestSuiteTest, DirectoryXmlOutputCannotCertifyAnUnknownReport) {
  const auto output = root_ / "reports";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=xml:" + output.string() + "/"}), 2);
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"xml_report\": null"));
}

TEST_F(GtestSuiteTest, ExistingExplicitXmlCannotBeOverwrittenOrCertified) {
  const auto output = root_ / "existing.xml";
  std::ofstream(output) << "existing-report-sentinel";
  EXPECT_EQ(run({"--gtest_filter=SvgSuite.*", "--gtest_output=xml:" + output.string()}), 2);
  std::ifstream input(output);
  const std::string contents{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
  EXPECT_EQ(contents, "existing-report-sentinel");
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
}

TEST_F(GtestSuiteTest, RemovedXmlReportCannotCertifyCompletedCases) {
  EXPECT_EQ(run({"--fixture-remove-xml", "--gtest_filter=SvgSuite.*"}), 3);
  EXPECT_THAT(verification(), testing::HasSubstr("\"completed_cases\": 2"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"complete_corpus_accepted\": false"));
  EXPECT_THAT(verification(), testing::HasSubstr("\"xml_report\": null"));
}

TEST_F(GtestSuiteTest, EmptyXmlSuffixCannotReferenceStaleDefaultReport) {
  EXPECT_EQ(run({"--fixture-empty-xml", "--gtest_filter=SvgSuite.*", "--gtest_output=xml:"}), 2);
  const std::string record = verification();
  EXPECT_THAT(record, testing::HasSubstr("\"gtest_output\": \"xml:\""));
  EXPECT_THAT(record, testing::HasSubstr("\"xml_report\": null"));
  std::ifstream stale(root_ / "test_detail.xml");
  const std::string contents{std::istreambuf_iterator<char>(stale),
                             std::istreambuf_iterator<char>()};
  EXPECT_EQ(contents, "stale-report-sentinel");
  EXPECT_FALSE(std::filesystem::exists(root_ / (root_.filename().string() + ".xml")));
}

}  // namespace
}  // namespace donner::svg_suite

int main(int argc, char** argv) {
  if (argc >= 3 && std::string_view(argv[1]) == "--fixture-suite") {
    return donner::svg_suite::RunFixture(argc, argv);
  }
  donner::svg_suite::executable = std::filesystem::absolute(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
